// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

//!variant aerial_perspective_froxel_lighting.comp
//!variant aerial_perspective_froxel_lighting_traced.comp
//!>       AP_TRACE_SCENE
//!end-variants

// Aerial perspective near field, lighting (docs/aerial-perspective-v2.md, section 4.2).
//
// One thread per froxel. Each frame it takes one jittered sample inside its froxel and adds it to
// the froxel's history:
//   * one environment ray, alternating per froxel and frame between the sun (scene visibility of
//     the sun) and the sky (a cosine-distributed upper-hemisphere ray: how much sky the point sees),
//     or the sky every frame while the sun is below the horizon;
//   * one local light from the froxel's cull cluster, picked by unshadowed luminance, with one shadow ray;
//   * the host's local-light injection, if it supplies one.
// Only lighting is stored, never the medium: cloud shadow, sun transmittance, phase and density are
// smooth and analytic and are applied every frame by the integration pass, so they never lag.
//
// Every sample stays in front of the visible surface its ray meets (the host's linear view Z,
// docs/aerial-perspective-v2.md 4.2.2): behind a one-sided shell it would see the open sun and sky.
//
// Without AP_TRACE_SCENE (no host TLAS) everything is unoccluded and the history only smooths the
// local-light estimate.

#include "numos/lib/aerial_perspective_lights.slangh"
#include "numos/lib/aerial_perspective_binding_indices.h"
#include "numos/util/instance_definitions.h"   // OBJECT_MASK_OPAQUE

layout(binding = AP_LIGHTING_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = AP_LIGHTING_CAMERA)
ConstantBuffer<Camera> atmosphereCamera;

#if defined(AP_TRACE_SCENE)
layout(binding = AP_LIGHTING_ACCELERATION_STRUCTURE)
RaytracingAccelerationStructure sceneTlas;
#endif

layout(binding = AP_LIGHTING_LIGHTS)
StructuredBuffer<AerialPerspectiveLight> aerialPerspectiveLights;

layout(binding = AP_LIGHTING_CLUSTERS)
StructuredBuffer<uint> aerialPerspectiveLightClusters;

// Previous frame: RGB local-light in-scatter per unit sigma_s, A sun visibility; aux R sky
// visibility, G age / 255, B local-light visibility (the shadowed share of the unshadowed local
// in-scatter, which the composite multiplies into its per-pixel analytic glow), A a copy of the sun
// visibility for the composite's debug view, which binds aux but not the lighting volume. Both
// RGBA16F.
layout(binding = AP_LIGHTING_HISTORY)
Texture3D<float4> historyLighting;

layout(binding = AP_LIGHTING_HISTORY_AUX)
Texture3D<float4> historyAux;

layout(binding = AP_LIGHTING_SAMPLER)
SamplerState linearSampler;

layout(binding = AP_LIGHTING_OUTPUT, rgba16f)
RWTexture3D<float4> lightingOutput;

// RGBA16F: an 8-bit visibility blended toward 0 rounds back to itself below 0.5 / (255 w), about
// 0.06 at 32 history frames, so a froxel once lit never went dark again.
layout(binding = AP_LIGHTING_OUTPUT_AUX, rgba16f)
RWTexture3D<float4> auxOutput;

// Host-written local-light in-scatter in the previous frame's grid (same layout and meaning as the
// RGB above). Read only when args.apHostLightInjection is set.
layout(binding = AP_LIGHTING_INJECTION)
Texture3D<float4> hostInjection;

// The host's linear view Z of this frame (FrameInputs render resolution), and whether there is one.
layout(binding = AP_LIGHTING_LINEAR_VIEW_Z)
Texture2D<float> sceneLinearViewZ;

layout(binding = AP_LIGHTING_SURFACE_ARGS)
ConstantBuffer<ApSurfaceArgs> surfaceArgs;

// The lighting pass's own switches (Renderer::Impl::dispatchAerialPerspectiveNear).
struct ApLightingArgs {
  // 1: while the sun ray is skipped (sun below the horizon), every frame is a sky turn.
  uint skyEveryFrameWithoutSun;
  // 1: a froxel whose history visibility lies inside (kApPenumbraLow, kApPenumbraHigh) traces that
  // term every frame.
  uint penumbraEveryFrame;
  // Blend weight a visibility term takes when its history is saturated (outside the penumbra band)
  // and this frame's ray, confirmed by kApConfirmRays more, says the opposite. 0 = off.
  float disagreementWeight;
  // How far outside the previous grid, in screen UV, a froxel's reprojection may land and still take
  // the nearest edge history as a prior (aged at most revealAge). 0 = off: such froxels start fresh.
  float edgeMargin;
  // The age cap of history that only approximates this froxel (an edge prior, or one a surface hid
  // last frame), so that it converges again in a few frames. 0 = no cap.
  float revealAge;
  // 1: a froxel whose history a surface hid last frame, and that is visible now, takes revealAge.
  uint revealUncovered;
  // 1: a sample moved in front of its froxel's near face does not update that froxel's visibility
  // while other pixels of the tile still see into it.
  uint holdMovedSamples;
  // 1: a froxel past the deepest surface of every column its readers can reach (hiddenDepth) traces
  // nothing and copies the history at its clamped sample point.
  uint skipHidden;
  // 1: a froxel's sun or sky turn follows a per-froxel hashed phase; 0: the (x + y + z + frame)
  // checkerboard.
  uint hashedTurnPhase;
  uint pad9;
  uint pad10;
  uint pad11;
};

// The deepest visible surface of each froxel column's screen tile (aerial_perspective_tile_depth), this
// frame and the frame the history was written in.
layout(binding = AP_LIGHTING_TILE_DEPTH)
Texture2D<float> tileDepth;

layout(binding = AP_LIGHTING_HISTORY_TILE_DEPTH)
Texture2D<float> historyTileDepth;

// The deepest tile depth over the columns within the hidden-froxel radius of each column
// (aerial_perspective_tile_depth_dilate).
layout(binding = AP_LIGHTING_HIDDEN_DEPTH)
Texture2D<float> hiddenDepth;

// A visibility estimate built from 0/1 rays has variance v (1 - v): largest in a penumbra, nearly
// none where the history says fully lit or fully shadowed.
static const float kApPenumbraLow = 0.05f;
static const float kApPenumbraHigh = 0.95f;

bool apInPenumbra(float visibility) {
  return visibility > kApPenumbraLow && visibility < kApPenumbraHigh;
}

// A saturated history contradicted by a 0/1 sample: a moving occluder, a time-of-day jump, or a rare
// true hit in a froxel that is almost fully shadowed. Only the first two should speed the blend up,
// so the contradiction must survive kApConfirmRays more rays from other points or directions. A rare
// hit at visibility v passes all of them with probability v^(1 + kApConfirmRays), which adds no
// visible bias.
static const uint kApConfirmRays = 2u;

bool apContradicts(float history, float value) {
  return (history <= kApPenumbraLow && value > 0.5f) || (history >= kApPenumbraHigh && value < 0.5f);
}

layout(binding = AP_LIGHTING_PARAMS)
ConstantBuffer<ApLightingArgs> lightingArgs;

uint apHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// Point n of the R2 sequence (the golden ratio in 2D) under a per-froxel rotation, in 32-bit fixed
// point so that it keeps its precision at any frame count.
vec2 apR2(uint n, uint2 seed) {
  const uint2 point = seed + n * uint2(3242174889u, 2447445413u);
  return vec2(point >> 8u) * (1.0f / 16777216.0f);
}

#if defined(AP_TRACE_SCENE)
// OBJECT_MASK_OPAQUE takes alpha-tested geometry with the solid world, FORCE_OPAQUE keeps
// clip-planed instances occluding, and the first hit ends the search. CULL_NON_OPAQUE would trace
// straight through both.
bool apOccluded(vec3 origin, vec3 direction, float tMax) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0f;
  ray.TMax = tMax;
  static const uint kRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
    | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
  RayQuery<kRayFlags> query;
  query.TraceRayInline(sceneTlas, kRayFlags, OBJECT_MASK_OPAQUE, ray);
  query.Proceed();
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Distance to the first opaque hit within tMax, or -1: the same geometry the visibility rays see.
float apFirstHitDistance(vec3 origin, vec3 direction, float tMax) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0f;
  ray.TMax = tMax;
  static const uint kRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
    | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
  RayQuery<kRayFlags> query;
  query.TraceRayInline(sceneTlas, kRayFlags, OBJECT_MASK_OPAQUE, ray);
  query.Proceed();
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : -1.0f;
}
#endif

// One jittered sample point of a froxel, kept in front of the visible surface its ray meets.
struct ApSample {
  vec3  rayVector;       // through the sample's screen position, forward component 1
  float surfaceForward;  // the host depth at that position, kApNoSurface on sky
  float sampleForward;   // the sample's forward distance after the clamp and the guard
  bool  depthClamped;    // the depth clamp moved it
  bool  guardMoved;      // the camera-ray guard moved it further forward
};

ApSample apTakeSample(vec3 cell, AtmosphereArgs args) {
  ApSample s;
  s.rayVector = apFroxelRayVector(cell.xy, atmosphereCamera);
  // In front of the surface this sample's ray meets: past it the sample would take the light of
  // whatever lies behind the wall. A froxel that holds the surface, or lies wholly behind it on this
  // ray, takes its sample just in front of the surface, so no history ever holds light from behind a
  // wall (keeping the old value instead would carry it into the neighbors' filtered history reads
  // whenever the camera moves).
  s.surfaceForward = apSurfaceForward(sceneLinearViewZ, surfaceArgs, cell.xy);
  const float jitteredForward = apFroxelSliceToDepth(cell.z, args);
  const float surfaceLimit = apSurfaceSampleLimit(s.surfaceForward, args);
  s.depthClamped = jitteredForward > surfaceLimit;
  s.sampleForward = min(jitteredForward, surfaceLimit);
  s.guardMoved = false;
#if defined(AP_TRACE_SCENE)
  // The host's depth can lie behind the geometry the rays see: a displaced surface writes its relief
  // point (Remix: rtx.displacement.enableDepthOffset, up to the full displacement depth behind the
  // mesh), PSR and glass write the virtual distance through them. Near the camera that exceeds the
  // offset above and the sample sat behind the mesh again. So a sample near or past the depth also
  // traces its own camera ray, against the same opaque geometry as its visibility rays.
  if (s.sampleForward >= (1.0f - kApSurfaceGuardFraction) * s.surfaceForward) {
    const float forwardPerDistance = 1.0f / length(s.rayVector);
    const float hit = apFirstHitDistance(args.cameraPosition, s.rayVector * forwardPerDistance,
                                         s.sampleForward / forwardPerDistance);
    if (hit >= 0.0f) {
      const float hitLimit = apSurfaceSampleLimit(hit * forwardPerDistance, args);
      s.guardMoved = hitLimit < s.sampleForward;
      s.sampleForward = min(s.sampleForward, hitLimit);
    }
  }
#endif
  return s;
}

static const vec3 kApR3 = vec3(0.8191725134f, 0.6710436067f, 0.5497004779f);

// Point n of the froxel's jitter sequence: a per-froxel offset into the R3 sequence, so successive
// frames cover the cell evenly and neighbors stay decorrelated.
vec3 apFroxelJitter(uint3 froxel, uint n) {
  const vec3 seed = vec3(apHash(froxel.x * 73856093u ^ froxel.y * 19349663u ^ froxel.z * 83492791u) & 0xFFFFu) / 65536.0f;
  return frac(seed + kApR3 * float(n % 4096u));
}

#if defined(AP_TRACE_SCENE)
// 1 when a cosine-distributed upper-hemisphere ray from `origin` escapes within a kilometer. `u` is a
// 2D sample; `stratum` of `strata` splits the azimuth.
float apSkyRay(vec3 origin, vec2 u, uint stratum, uint strata, AtmosphereArgs args) {
  const float u2 = (float(stratum) + u.y) / float(strata);
  const float r = sqrt(u.x);
  const vec3 dirYUp = vec3(r * cos(2.0f * kPi * u2), sqrt(max(1.0f - u.x, 0.0f)), r * sin(2.0f * kPi * u2));
  return apOccluded(origin, normalize(atmosphereYUpToWorld(dirYUp, args.isZUp != 0u, args.flipUpAxis != 0u)),
                    numosAirKmToWorld(1.0f, args)) ? 0.0f : 1.0f;
}
#endif

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 froxel : SV_DispatchThreadID) {
  const AtmosphereArgs args = atmosphereArgs;
  if (any(froxel >= uint3(args.apFroxelSizeX, args.apFroxelSizeY, args.apFroxelSlices))) {
    return;
  }

  const vec3 gridSize = vec3(args.apFroxelSizeX, args.apFroxelSizeY, args.apFroxelSlices);

  const vec3 cell = (vec3(froxel) + apFroxelJitter(froxel, args.apFrameIndex)) / gridSize;
  const float nearFace = apFroxelSliceToDepth(float(froxel.z) / gridSize.z, args);

  // ---- froxels nothing reads ----
  // Past the deepest surface of every column within the hidden radius, no pixel's integration, aux
  // read or filter tap reaches this froxel (the radius covers the composite's bilinear taps and the
  // local-light filter; the integration and its sky filter stop at each column's own surface). Indoors
  // that is most froxels past the first walls. Such a froxel traces nothing: it copies the history at
  // the point its sample would have been clamped to, the air in front of the surface, at most at the
  // reveal age, so it starts from that air once it comes into view.
  if (lightingArgs.skipHidden != 0u && args.apHistoryReset == 0u && nearFace > hiddenDepth.Load(int3(froxel.xy, 0))) {
    const vec3 skipRay = apFroxelRayVector(cell.xy, atmosphereCamera);
    const float skipForward = min(apFroxelSliceToDepth(cell.z, args),
      apSurfaceSampleLimit(apSurfaceForward(sceneLinearViewZ, surfaceArgs, cell.xy), args));
    vec3 skipUvw;
    if (apWorldToPreviousFroxelUvw(args.cameraPosition + skipRay * skipForward, atmosphereCamera, args, skipUvw)) {
      vec4 lighting = historyLighting.SampleLevel(linearSampler, skipUvw, 0);
      lighting.rgb *= args.radianceScaleHistoryRatio;
      vec4 aux = historyAux.SampleLevel(linearSampler, skipUvw, 0);
      const float cap = lightingArgs.revealAge > 0.0f ? lightingArgs.revealAge : 255.0f;
      aux.g = min(floor(aux.g * 255.0f + 0.5f) + 1.0f, cap) / 255.0f;
      lightingOutput[froxel] = lighting;
      auxOutput[froxel] = aux;
      return;
    }
  }

  const ApSample froxelSample = apTakeSample(cell, args);
  const vec3 rayVector = froxelSample.rayVector;
  const vec3 viewDirWorld = normalize(rayVector);
  const bool depthClamped = froxelSample.depthClamped;
  const bool guardMoved = froxelSample.guardMoved;
  const vec3 samplePos = args.cameraPosition + rayVector * froxelSample.sampleForward;

  // ---- history: the froxel center in the previous frame's grid ----
  // A center that reprojects just outside the previous grid's sides (the screen edge the camera
  // turns toward) takes the nearest edge froxels' history as a prior, aged at most revealAge,
  // rather than starting fresh: the air one froxel further out is usually lit alike.
  vec3 prevUvw = vec3(0.0f);
  bool historyValid = false;
  bool edgeHistory = false;
  if (args.apHistoryReset == 0u
      && apWorldToFroxelUvwUnbounded(apFroxelCenterWorld(froxel, atmosphereCamera, args), atmosphereCamera.prevWorldToView,
                                     atmosphereCamera.prevWorldToProjection, atmosphereCamera, args, prevUvw)
      && prevUvw.z >= 0.0f && prevUvw.z <= 1.0f) {
    historyValid = all(prevUvw.xy >= vec2(0.0f)) && all(prevUvw.xy <= vec2(1.0f));
    const float margin = lightingArgs.edgeMargin;
    if (!historyValid && margin > 0.0f && all(prevUvw.xy >= vec2(-margin)) && all(prevUvw.xy <= vec2(1.0f + margin))) {
      prevUvw.xy = saturate(prevUvw.xy);
      historyValid = true;
      edgeHistory = true;
    }
  }
  vec4 previous = vec4(0.0f, 0.0f, 0.0f, 1.0f);
  vec4 previousAux = vec4(1.0f, 0.0f, 0.0f, 0.0f);
  if (historyValid) {
    previous = historyLighting.SampleLevel(linearSampler, prevUvw, 0);
    previousAux = historyAux.SampleLevel(linearSampler, prevUvw, 0);
    // Last frame's light in this frame's pre-exposure (exactly 1 unless radianceScale stepped).
    previous.rgb *= args.radianceScaleHistoryRatio;
  }
  // A froxel whose history lay behind every surface of its tile last frame holds the air in front of
  // that surface (4.2.2), not its own. Once a surface stops hiding it, it takes the reveal age, so the
  // stale value goes in a few frames instead of at the history floor. While it stays hidden its age is
  // left alone: only reads of neighboring columns see it, and they would see the extra noise.
  const bool hiddenNow = nearFace > apSurfaceSampleLimit(tileDepth.Load(int3(froxel.xy, 0)), args);
  bool uncovered = false;
  if (historyValid && !edgeHistory && lightingArgs.revealUncovered != 0u) {
    const uint2 previousColumn = min(uint2(prevUvw.xy * gridSize.xy), uint2(gridSize.xy) - 1u);
    const float previousLimit = apSurfaceSampleLimit(historyTileDepth.Load(int3(previousColumn, 0)), args);
    uncovered = !hiddenNow && apFroxelSliceToDepth(prevUvw.z, args) > previousLimit;
  }
  const float historyAge = historyValid ? floor(previousAux.g * 255.0f + 0.5f) : 0.0f;
  const float age = (edgeHistory || uncovered) && lightingArgs.revealAge > 0.0f
    ? min(historyAge, lightingArgs.revealAge) : historyAge;
  const float newAge = min(age + 1.0f, 255.0f);
  const float weight = max(1.0f / newAge, args.apHistoryWeightMin);
  // The two visibilities are each sampled every other frame, so each blends at half the rate.
  const float alternateWeight = age < 1.0f ? 1.0f : min(max(2.0f / (newAge + 1.0f), 2.0f * args.apHistoryWeightMin), 1.0f);
  // The sky visibility has its own, longer floor: in shaded air it is a small share of rare hits.
  const float skyAlternateWeight = age < 1.0f ? 1.0f : min(max(2.0f / (newAge + 1.0f), 2.0f * args.apSkyHistoryWeightMin), 1.0f);

  // ---- sun and sky visibility ----
  // A froxel without history (it just entered the frustum, or the history was reset) has no
  // estimate yet. It must not start from "unoccluded" (1): in an interior that is the full outdoor
  // haze, which then fades as the alternating rays catch up and shows as a bright veil along every
  // screen edge the camera turns toward (docs/aerial-perspective-v2.md, 4.2 "Fresh froxels"). A
  // fresh froxel traces both terms this frame and takes them outright (alternateWeight is 1 at
  // age 0), so it starts from this frame's measurement and nothing assumed.
  // Without scene shadows the visibilities are 1 ("unoccluded"), never the history: a history traced
  // with them on would otherwise survive switching them off.
  //
  // A sample whose pixel lands on a surface nearer than this froxel is moved in front of that surface,
  // out of the froxel. Where other pixels of the tile see into or past the froxel (a silhouette
  // against the sky or a farther wall), the froxel stands for that air too, and a point hugging the
  // surface sees less sky and often less sun: blended in, it darkens the froxels along every
  // silhouette. Such a sample leaves both visibilities and the age as they are. A froxel that every
  // pixel of the tile hides takes all its samples in front of the surface, as before (4.2.2).
  float sunVisibility = 1.0f;
  float skyVisibility = 1.0f;
  bool visibilityHeld = false;
#if defined(AP_TRACE_SCENE)
  if (args.apSceneShadow != 0u) {
    sunVisibility = age < 1.0f ? 1.0f : previous.a;
    skyVisibility = age < 1.0f ? 1.0f : previousAux.r;
    visibilityHeld = lightingArgs.holdMovedSamples != 0u && age >= 1.0f
                  && froxelSample.sampleForward < nearFace && !hiddenNow;
    const bool isZUp = args.isZUp != 0u;
    const bool flip = args.flipUpAxis != 0u;
    const bool fresh = age < 1.0f;
    const vec3 sunDirYUp = normalize(args.sunDirection);
    // A sun below the horizon contributes nothing whatever the scene does, so its ray is skipped.
    const bool sunTraced = sunDirYUp.y > -0.1f;
    // Which frames are the froxel's sun turns. A checkerboard over (x + y + z) locks air that moves an
    // odd whole number of froxels a frame to one kind of turn: each frame's history comes from a
    // froxel whose turn was the same, so that air only ever sees the sun or only the sky. A hashed
    // phase per froxel breaks the lock and keeps the half-and-half split.
    const uint turnPhase = lightingArgs.hashedTurnPhase != 0u
      ? apHash(froxel.x * 1597334677u ^ froxel.y * 3812015801u ^ froxel.z * 2798796415u) >> 31u
      : froxel.x + froxel.y + froxel.z;
    const bool sunParity = ((turnPhase + args.apFrameIndex) & 1u) == 0u;
    // Without a sun ray to alternate with, the sky takes every frame. A term whose history lies in a
    // penumbra, where a 0/1 estimate is noisiest, also takes every frame. Either way it then blends
    // at the single rate.
    const bool penumbra = !fresh && lightingArgs.penumbraEveryFrame != 0u;
    const bool sunEveryFrame = penumbra && sunTraced && apInPenumbra(previous.a);
    const bool skyEveryFrame = (!sunTraced && lightingArgs.skyEveryFrameWithoutSun != 0u)
                            || (penumbra && apInPenumbra(previousAux.r));
    const bool sunTurn = !visibilityHeld && (fresh || sunParity || sunEveryFrame);
    const bool skyTurn = !visibilityHeld && (fresh || !sunParity || skyEveryFrame);
    const float sunWeight = fresh ? 1.0f : sunEveryFrame ? weight : alternateWeight;
    const float skyWeight = fresh ? 1.0f
      : skyEveryFrame ? max(1.0f / newAge, args.apSkyHistoryWeightMin) : skyAlternateWeight;
    const float boost = lightingArgs.disagreementWeight;
    if (sunTurn) {
      float visible = 1.0f;
      float w = sunWeight;
      if (sunTraced) {
        const vec3 sunDirWorld = normalize(atmosphereYUpToWorld(sunDirYUp, isZUp, flip));
        visible = apOccluded(samplePos, sunDirWorld, 1e9f) ? 0.0f : 1.0f;
        // A saturated history the ray contradicts, and kApConfirmRays more samples at other points of
        // the froxel agree with the ray: the shadow moved, so the history gives way fast.
        if (!fresh && boost > w && apContradicts(sunVisibility, visible)) {
          bool confirmed = true;
          for (uint k = 1u; k <= kApConfirmRays && confirmed; ++k) {
            const ApSample extra = apTakeSample(
              (vec3(froxel) + apFroxelJitter(froxel, args.apFrameIndex + k * 1365u)) / gridSize, args);
            const vec3 extraPos = args.cameraPosition + extra.rayVector * extra.sampleForward;
            confirmed = (apOccluded(extraPos, sunDirWorld, 1e9f) ? 0.0f : 1.0f) == visible;
          }
          if (confirmed) {
            w = boost;
          }
        }
      }
      sunVisibility = lerp(sunVisibility, visible, w);
    }
    if (skyTurn) {
      // Cosine-distributed about up, a kilometer long: long enough for any roof or canopy, short
      // enough that a distant ridge does not darken the sky of a whole valley. A fresh froxel takes
      // kApFreshSkyRays rays, one per azimuth quarter (stratified), so its first estimate of a
      // partial sky (a doorway, a canopy) is a quarter-step value instead of a 0/1 coin flip. Only
      // froxels entering the view pay for them: a pan reveals a few columns of the grid per frame.
      // The directions follow the froxel's own R2 sequence over its sky turns (every other frame, or
      // every frame without a sun ray or in a penumbra), so a still view covers the hemisphere evenly,
      // and a partial sky converges far faster than with independent random directions, which clump
      // and leave blotches in shaded haze.
      static const uint kApFreshSkyRays = 4u;
      const uint rays = fresh ? kApFreshSkyRays : 1u;
      const uint skyHash = apHash(froxel.x * 2654435761u ^ froxel.y * 2246822519u ^ froxel.z * 3266489917u);
      const uint2 skySeed = uint2(skyHash, apHash(skyHash));
      const uint skyTurnIndex = skyEveryFrame ? args.apFrameIndex : args.apFrameIndex >> 1u;
      float visibleSum = 0.0f;
      for (uint i = 0u; i < rays; ++i) {
        visibleSum += apSkyRay(samplePos, apR2(skyTurnIndex * rays + i, skySeed), i, rays, args);
      }
      float w = skyWeight;
      // As for the sun: the confirming rays take the same point of other rotations of the sequence.
      if (!fresh && boost > w && apContradicts(skyVisibility, visibleSum)) {
        bool confirmed = true;
        for (uint k = 1u; k <= kApConfirmRays && confirmed; ++k) {
          const uint2 confirmSeed = uint2(apHash(skySeed.x + k), apHash(skySeed.y ^ (k * 0x9e3779b9u)));
          confirmed = apSkyRay(samplePos, apR2(skyTurnIndex, confirmSeed), 0u, 1u, args) == visibleSum;
        }
        if (confirmed) {
          w = boost;
        }
      }
      skyVisibility = lerp(skyVisibility, visibleSum / float(rays), w);
    }
  }
#endif

  // ---- local lights: one pick from the cluster, one shadow ray ----
  vec3 localSample = vec3(0.0f);
  // The pick follows each light's unshadowed luminance, so the picked light's visibility
  // alone is an unbiased sample of the luminance-weighted visibility of the whole cluster.
  float localVisibility = age < 1.0f ? 1.0f : previousAux.b;
  bool localSampled = false;
  if (args.apLocalLightCount != 0u) {
    const uint clusterOffset = aerialPerspectiveLightClusterOffset(froxel.xy / AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE, froxel.z, args);
    const uint count = min(aerialPerspectiveLightClusters[clusterOffset], AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER);
    if (count != 0u) {
      const ApPhaseMix phaseMix = apPhaseMixAt(apSampleMedium(samplePos, args));
      // One light picked in proportion to its unshadowed luminance, by inverting the cluster's CDF with
      // the froxel's own golden-ratio sequence over frames. Successive frames then pick each light in
      // close to its exact share, where independent random picks left the luminance-weighted
      // visibility noisy wherever lights are partly shadowed.
      float lightWeights[AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER];
      float weightSum = 0.0f;
      for (uint slot = 0u; slot < count; ++slot) {
        const uint lightIndex = aerialPerspectiveLightClusters[clusterOffset + 1u + slot];
        const vec3 radiance = aerialPerspectiveLightRadiance(aerialPerspectiveLights[lightIndex], samplePos,
                                                             viewDirWorld, phaseMix, args);
        const float w = dot(radiance, vec3(0.2126f, 0.7152f, 0.0722f));
        lightWeights[slot] = w > 0.0f ? w : 0.0f;
        weightSum += lightWeights[slot];
      }
      vec3 chosenRadiance = vec3(0.0f);
      float chosenWeight = 0.0f;
      uint chosenIndex = 0u;
      if (weightSum > 0.0f) {
        const uint lightHash = apHash(froxel.x * 3266489917u ^ froxel.y * 668265263u ^ froxel.z * 374761393u);
        const float u = float((lightHash + args.apFrameIndex * 2654435769u) >> 8u) * (1.0f / 16777216.0f);
        const float target = u * weightSum;
        float cumulative = 0.0f;
        uint chosenSlot = count;
        for (uint slot = 0u; slot < count; ++slot) {
          if (lightWeights[slot] > 0.0f) {
            chosenSlot = slot;
            cumulative += lightWeights[slot];
            if (target < cumulative) {
              break;
            }
          }
        }
        if (chosenSlot < count) {
          chosenIndex = aerialPerspectiveLightClusters[clusterOffset + 1u + chosenSlot];
          chosenWeight = lightWeights[chosenSlot];
          chosenRadiance = aerialPerspectiveLightRadiance(aerialPerspectiveLights[chosenIndex], samplePos,
                                                          viewDirWorld, phaseMix, args);
        }
      }
      if (chosenWeight > 0.0f) {
        float visible = 1.0f;
#if defined(AP_TRACE_SCENE)
        if (args.apLocalLightShadows != 0u) {
          const AerialPerspectiveLight light = aerialPerspectiveLights[chosenIndex];
          const vec3 toLight = light.position - samplePos;
          const float distance = length(toLight);
          // Stop at the emitter's surface: a light usually sits inside the mesh that represents it.
          const float tMax = max(distance - max(light.equivalentRadius, 0.0f), 0.0f) * 0.999f;
          if (tMax > 0.0f && apOccluded(samplePos, toLight / max(distance, 1e-6f), tMax)) {
            visible = 0.0f;
          }
        }
#endif
        localSample = chosenRadiance * (weightSum / chosenWeight) * visible;
        localVisibility = age < 1.0f ? visible : lerp(localVisibility, visible, weight);
        localSampled = true;
      }
    }
  }
  if (args.apHostLightInjection != 0u && historyValid) {
    localSample += max(hostInjection.SampleLevel(linearSampler, prevUvw, 0).rgb, vec3(0.0f));
  }

  const vec3 local = age < 1.0f ? localSample : lerp(previous.rgb, localSample, weight);
  lightingOutput[froxel] = vec4(local, saturate(sunVisibility));
  // A carries the sun visibility for debug views 1 and 6; views 9 and 10 put the share of samples the
  // surface clamp moved there instead (only the debug view reads A).
  float auxA = saturate(sunVisibility);
  if (args.apDebugView == kApDebugViewSurfaceClamp || args.apDebugView == kApDebugViewSurfaceGuard) {
    const float moved = (args.apDebugView == kApDebugViewSurfaceClamp ? depthClamped : guardMoved) ? 1.0f : 0.0f;
    auxA = age < 1.0f ? moved : lerp(saturate(previousAux.a), moved, weight);
  }
  auxOutput[froxel] = vec4(saturate(skyVisibility), (visibilityHeld ? age : newAge) / 255.0f, saturate(localSampled || age >= 1.0f ? localVisibility : 1.0f),
                           auxA);
}
