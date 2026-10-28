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
//     the sun) and the sky (a cosine-distributed upper-hemisphere ray: how much sky the point sees);
//   * one local light, chosen by RIS from the froxel's cull cluster, with one shadow ray;
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

uint apHash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float apRandom(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  word = (word >> 22u) ^ word;
  return float(word >> 8) * (1.0f / 16777216.0f);
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

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 froxel : SV_DispatchThreadID) {
  const AtmosphereArgs args = atmosphereArgs;
  if (any(froxel >= uint3(args.apFroxelSizeX, args.apFroxelSizeY, args.apFroxelSlices))) {
    return;
  }

  const vec3 gridSize = vec3(args.apFroxelSizeX, args.apFroxelSizeY, args.apFroxelSlices);
  uint rng = apHash(froxel.x + apHash(froxel.y + apHash(froxel.z + apHash(args.apFrameIndex))));

  // Jitter inside the froxel: a per-froxel offset into the R3 sequence, so successive frames cover
  // the cell evenly and neighbours stay decorrelated.
  const vec3 r3 = vec3(0.8191725134f, 0.6710436067f, 0.5497004779f);
  const vec3 seed = vec3(apHash(froxel.x * 73856093u ^ froxel.y * 19349663u ^ froxel.z * 83492791u) & 0xFFFFu) / 65536.0f;
  const vec3 jitter = frac(seed + r3 * float(args.apFrameIndex % 4096u));
  const vec3 cell = (vec3(froxel) + jitter) / gridSize;
  const vec3 rayVector = apFroxelRayVector(cell.xy, atmosphereCamera);
  // In front of the surface this sample's ray meets: past it the sample would take the light of
  // whatever lies behind the wall. A froxel that holds the surface, or lies wholly behind it on this
  // ray, takes its sample just in front of the surface, so no history ever holds light from behind a
  // wall (keeping the old value instead would carry it into the neighbors' filtered history reads
  // whenever the camera moves).
  const float surfaceForward = apSurfaceForward(sceneLinearViewZ, surfaceArgs, cell.xy);
  const float jitteredForward = apFroxelSliceToDepth(cell.z, args);
  const float surfaceLimit = apSurfaceSampleLimit(surfaceForward, args);
  const bool depthClamped = jitteredForward > surfaceLimit;
  float sampleForward = min(jitteredForward, surfaceLimit);
  const vec3 viewDirWorld = normalize(rayVector);
  bool guardMoved = false;
#if defined(AP_TRACE_SCENE)
  // The host's depth can lie behind the geometry the rays see: a displaced surface writes its relief
  // point (Remix: rtx.displacement.enableDepthOffset, up to the full displacement depth behind the
  // mesh), PSR and glass write the virtual distance through them. Near the camera that exceeds the
  // offset above and the sample sat behind the mesh again. So a sample near or past the depth also
  // traces its own camera ray, against the same opaque geometry as its visibility rays.
  if (sampleForward >= (1.0f - kApSurfaceGuardFraction) * surfaceForward) {
    const float forwardPerDistance = 1.0f / length(rayVector);
    const float hit = apFirstHitDistance(args.cameraPosition, viewDirWorld, sampleForward / forwardPerDistance);
    if (hit >= 0.0f) {
      const float hitLimit = apSurfaceSampleLimit(hit * forwardPerDistance, args);
      guardMoved = hitLimit < sampleForward;
      sampleForward = min(sampleForward, hitLimit);
    }
  }
#endif
  const vec3 samplePos = args.cameraPosition + rayVector * sampleForward;

  // ---- history: the froxel centre in the previous frame's grid ----
  vec3 prevUvw;
  const bool historyValid = args.apHistoryReset == 0u
    && apWorldToPreviousFroxelUvw(apFroxelCenterWorld(froxel, atmosphereCamera, args), atmosphereCamera, args, prevUvw);
  vec4 previous = vec4(0.0f, 0.0f, 0.0f, 1.0f);
  vec4 previousAux = vec4(1.0f, 0.0f, 0.0f, 0.0f);
  if (historyValid) {
    previous = historyLighting.SampleLevel(linearSampler, prevUvw, 0);
    previousAux = historyAux.SampleLevel(linearSampler, prevUvw, 0);
    // Last frame's light in this frame's pre-exposure (exactly 1 unless radianceScale stepped).
    previous.rgb *= args.radianceScaleHistoryRatio;
  }
  const float age = historyValid ? floor(previousAux.g * 255.0f + 0.5f) : 0.0f;
  const float newAge = min(age + 1.0f, 255.0f);
  const float weight = max(1.0f / newAge, args.apHistoryWeightMin);
  // The two visibilities are each sampled every other frame, so each blends at half the rate.
  const float alternateWeight = age < 1.0f ? 1.0f : min(max(2.0f / (newAge + 1.0f), 2.0f * args.apHistoryWeightMin), 1.0f);

  // ---- sun and sky visibility ----
  // A froxel without history (it just entered the frustum, or the history was reset) has no
  // estimate yet. It must not start from "unoccluded" (1): in an interior that is the full outdoor
  // haze, which then fades as the alternating rays catch up and shows as a bright veil along every
  // screen edge the camera turns toward (docs/aerial-perspective-v2.md, 4.2 "Fresh froxels"). A
  // fresh froxel traces both terms this frame and takes them outright (alternateWeight is 1 at
  // age 0), so it starts from this frame's measurement and nothing assumed.
  // Without scene shadows the visibilities are 1 ("unoccluded"), never the history: a history traced
  // with them on would otherwise survive switching them off.
  float sunVisibility = 1.0f;
  float skyVisibility = 1.0f;
#if defined(AP_TRACE_SCENE)
  if (args.apSceneShadow != 0u) {
    sunVisibility = age < 1.0f ? 1.0f : previous.a;
    skyVisibility = age < 1.0f ? 1.0f : previousAux.r;
    const bool isZUp = args.isZUp != 0u;
    const bool flip = args.flipUpAxis != 0u;
    const bool fresh = age < 1.0f;
    const bool sunTurn = fresh || ((froxel.x + froxel.y + froxel.z + args.apFrameIndex) & 1u) == 0u;
    const bool skyTurn = fresh || !sunTurn;
    const vec3 sunDirYUp = normalize(args.sunDirection);
    if (sunTurn) {
      // A sun below the horizon contributes nothing whatever the scene does; skip the ray.
      float visible = 1.0f;
      if (sunDirYUp.y > -0.1f) {
        visible = apOccluded(samplePos, normalize(atmosphereYUpToWorld(sunDirYUp, isZUp, flip)), 1e9f) ? 0.0f : 1.0f;
      }
      sunVisibility = lerp(sunVisibility, visible, alternateWeight);
    }
    if (skyTurn) {
      // Cosine-distributed about up, a kilometre long: long enough for any roof or canopy, short
      // enough that a distant ridge does not darken the sky of a whole valley. A fresh froxel takes
      // kApFreshSkyRays rays, one per azimuth quarter (stratified), so its first estimate of a
      // partial sky (a doorway, a canopy) is a quarter-step value instead of a 0/1 coin flip. Only
      // froxels entering the view pay for them: a pan reveals a few columns of the grid per frame.
      static const uint kApFreshSkyRays = 4u;
      const uint rays = fresh ? kApFreshSkyRays : 1u;
      float visibleSum = 0.0f;
      for (uint i = 0u; i < rays; ++i) {
        const float u1 = apRandom(rng);
        const float u2 = (float(i) + apRandom(rng)) / float(rays);
        const float r = sqrt(u1);
        const vec3 dirYUp = vec3(r * cos(2.0f * kPi * u2), sqrt(max(1.0f - u1, 0.0f)), r * sin(2.0f * kPi * u2));
        visibleSum += apOccluded(samplePos, normalize(atmosphereYUpToWorld(dirYUp, isZUp, flip)),
                                 numosAirKmToWorld(1.0f, args)) ? 0.0f : 1.0f;
      }
      skyVisibility = lerp(skyVisibility, visibleSum / float(rays), alternateWeight);
    }
  }
#endif

  // ---- local lights: one RIS pick from the cluster, one shadow ray ----
  vec3 localSample = vec3(0.0f);
  // RIS picks a light in proportion to its unshadowed luminance, so the picked light's visibility
  // alone is an unbiased sample of the luminance-weighted visibility of the whole cluster.
  float localVisibility = age < 1.0f ? 1.0f : previousAux.b;
  bool localSampled = false;
  if (args.apLocalLightCount != 0u) {
    const uint clusterOffset = aerialPerspectiveLightClusterOffset(froxel.xy / AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE, froxel.z, args);
    const uint count = min(aerialPerspectiveLightClusters[clusterOffset], AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER);
    if (count != 0u) {
      const ApPhaseMix phaseMix = apPhaseMixAt(apSampleMedium(samplePos, args));
      // Weighted reservoir sampling over the cluster, target = unshadowed luminance.
      float weightSum = 0.0f;
      vec3 chosenRadiance = vec3(0.0f);
      float chosenWeight = 0.0f;
      uint chosenIndex = 0u;
      for (uint slot = 0u; slot < count; ++slot) {
        const uint lightIndex = aerialPerspectiveLightClusters[clusterOffset + 1u + slot];
        const vec3 radiance = aerialPerspectiveLightRadiance(aerialPerspectiveLights[lightIndex], samplePos,
                                                             viewDirWorld, phaseMix, args);
        const float w = dot(radiance, vec3(0.2126f, 0.7152f, 0.0722f));
        if (!(w > 0.0f)) {
          continue;
        }
        weightSum += w;
        if (apRandom(rng) * weightSum < w) {
          chosenRadiance = radiance;
          chosenWeight = w;
          chosenIndex = lightIndex;
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
  auxOutput[froxel] = vec4(saturate(skyVisibility), newAge / 255.0f, saturate(localSampled || age >= 1.0f ? localVisibility : 1.0f),
                           auxA);
}
