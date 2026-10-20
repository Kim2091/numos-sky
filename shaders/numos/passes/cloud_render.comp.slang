// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

//!variant cloud_render.comp
//!variant cloud_render_fast.comp
//!> CLOUD_SUN_BEER_MAP=1
//!variant cloud_render_reference.comp
//!> CLOUD_REFERENCE_PATH_TRACE=1
//!end-variants

// Cloud render pass (Nubis Cubed, Schneider 2023).
//
// Per-pixel view raymarch through the cloud layers with the Nubis Cubed lighting (pp. 137, 142):
// the two-lobe HG direct term with the multiple-scattering remap, and the ambient term
// pow(1 - dim_profile, 0.5) * exp(-D_ambient). Writes premultiplied rgb and view-ray transmittance
// to cloudRenderOutput at the cloud render resolution. The host composites it over sky-miss pixels
// when cloudRenderRTEnable is set.
//
// The pass runs after the primary rays, so PrimaryLinearViewZ holds this frame's primary depth. The
// march clamps at that surface (marchCloudLayers' tMaxClamp), so cloud never marches past geometry;
// a sky pixel has no clamp. cloudDepthOutput holds the entry distance, the transmittance-weighted
// mean cloud depth and the surface distance, all in km.
//
// Coordinate frame: ray-slab intersection runs in camera-at-origin Y-up space, so the per-step t is
// measured from the camera. The positions passed to the density and lighting helpers are
// world-anchored (args.cameraWorldPosYUpKm + viewDirYUp * t), so the cloud fields stay fixed to the
// world and show parallax as the camera moves instead of moving rigidly with it.
//
// Rays use the host camera's projection (camera.slangh). The sun direction comes from
// args.sunDirection, already Y-up.

// ATMOSPHERE_AVAILABLE makes the helpers in atmosphere_common.slangh read the textures declared
// below instead of the binding-free fallbacks the LUT bakers use. The globals they expect are
// declared at this pass's slots before the include.
#define ATMOSPHERE_AVAILABLE

// AtmosphereArgs must be defined before the slot 0 constant buffer.
#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"
#include "numos/util/camera.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 17)
ConstantBuffer<Camera> cloudCamera;

// Linear repeat sampler for the volume taps, matching the frac() tile wrap. Slot 1 is unused.
layout(binding = 2)
SamplerState AtmosphereCloudNoise3DSampler;

// Cloud shadow maps (lib/cloud_shadow_map.slangh): the sun map, the horizon map and the vertical
// optical depth, read through the slot-2 sampler.
layout(binding = 20)
Texture3D<float2> AtmosphereCloudShadowMap;
layout(binding = 21)
Texture3D<float> AtmosphereCloudHorizonMap;
layout(binding = 22)
Texture3D<float4> AtmosphereCloudVerticalOd;
// Beer form of the sun map, for the performance variant (CLOUD_SUN_BEER_MAP).
layout(binding = 24)
Texture3D<float4> AtmosphereCloudBeerMap;
#define CLOUD_SHADOW_MAP_AVAILABLE
// Layer 2 patch-field map (cloud_layer2_map.comp), for layer 2's light paths.
layout(binding = 23)
Texture3D<float2> AtmosphereCloudLayer2Map;
#define CLOUD_LAYER2_MAP_AVAILABLE

layout(binding = 5)
Texture2DArray<float> BlueNoise;

layout(binding = 6, rgba16f)
RWTexture2D<float4> cloudRenderOutput;

// Sky-view LUT for the page-142 ambient term. sampleSkyAmbientForVolume takes it as an explicit
// parameter so the helper compiles in any pass. The sampler clamps in elevation; repeat would wrap
// the nadir onto the zenith.
layout(binding = 7)
Texture2D<float4> AtmosphereSkyViewLut;

layout(binding = 9)
SamplerState AtmosphereSkyViewSampler;

// The cloud body SDF (front buffer; km, negative inside, texture y vertical, see cloud_nvdf.h) and
// the 128^3 RGBA8 detail volume, read by sampleCloudDensityNubis3 through the slot 2 repeat sampler.
// The samplers clamp the SDF's vertical coordinate half a texel inside, so repeat never wraps the
// slab bottom onto the top.
layout(binding = 13)
Texture3D<float> AtmosphereCloudNvdfSdf;

layout(binding = 14)
Texture3D<float4> AtmosphereCloudDetailNoise3D;

// Slots 15 and 16 are in this pass's own descriptor set, unrelated to the common binding numbers in
// common_binding_indices.h. PrimaryLinearViewZ is the host's primary-ray linear view depth for this
// frame (Renderer::Impl::dispatchCloudRender). cloudDepthOutput is written for every pixel, like
// cloudRenderOutput.
layout(binding = 15)
Texture2D<float> PrimaryLinearViewZ;

// Entry, mean and surface distance (km), plus the density evaluation count for debug views.
layout(binding = 16, rgba32f)
RWTexture2D<float4> cloudDepthOutput;

// Last frame's output: read by the screen interleave and by the temporal filter.
layout(binding = 18)
Texture2D<float4> PrevCloudRender;
layout(binding = 19)
Texture2D<float4> PrevCloudDepth;

// Cell march fresh samples, one texel per cell: written by the march pass, read by the rebuild pass.
layout(binding = 25, rgba16f)
RWTexture2D<float4> cloudFreshColor;
layout(binding = 26, rgba32f)
RWTexture2D<float4> cloudFreshDepth;
layout(binding = 27, rg16f)
RWTexture2D<float2> cloudFreshAux;  // x: the flash's share (g_cloudFlashAccum), y: layer 2's share
// The temporal filter's per-pixel state (docs/cloud-temporal-filter.md): .x history length in
// samples, .y / .z the fast history's luminance and transmittance.
layout(binding = 28, rgba16f)
RWTexture2D<float4> cloudStateOutput;
layout(binding = 29)
Texture2D<float4> PrevCloudState;

// Last frame's radiance in this frame's pre-exposure (args.radianceScaleHistoryRatio is exactly 1
// except on the frame the scale steps); the transmittance is unitless.
vec4 loadPrevCloudRender(int2 texel) {
  const vec4 value = PrevCloudRender.Load(int3(texel, 0));
  return vec4(value.rgb * atmosphereArgs.radianceScaleHistoryRatio, value.a);
}

// Included after the texture globals so the ATMOSPHERE_AVAILABLE helpers bind to them.
#include "numos/lib/atmosphere_common.slangh"

// cloud_march_common.slangh holds the march and shade context this pass shares with the
// secondary-ray cloud LUT bake (cloud_secondary_lut.comp.slang); it must follow
// atmosphere_common.slangh.
//
// Only the screen pass evaluates the in-cloud lightning flash. The LUT bake persists across frames
// and stays flash-free (see the gate comment in cloud_march_common.slangh).
#define CLOUD_MARCH_LIGHTNING 1
// Per-sample sun transmittance at the cloud's own altitude.
layout(binding = 10)
Texture2D<float4> AtmosphereTransmittanceLut;
#define ATMOSPHERE_TRANSMITTANCE_LUT_AVAILABLE


#include "numos/lib/cloud_march_common.slangh"
#ifdef CLOUD_REFERENCE_PATH_TRACE
#include "numos/lib/cloud_reference.slangh"
#endif

#ifndef CLOUD_GROUP_X
#define CLOUD_GROUP_X 8
#define CLOUD_GROUP_Y 8
#endif

struct CloudPixelResult {
  vec4 color;  // premultiplied cloud rgb, view-ray transmittance
  vec4 depth;  // entry km, transmittance-weighted mean km, surface km, density evaluations
};

// Slant distance to the primary surface this pixel resolved, or kNoSurfaceKm on a sky miss.
float cloudSurfaceDistanceKm(uint2 pixelCoord, CameraDirections cloudRay, AtmosphereArgs args) {
  const float linearViewZ = PrimaryLinearViewZ[pixelCoord];
  // Sky rays carry NRD's miss sentinel. It arrives through the CB rather than as a literal so it
  // cannot drift from the value NRD actually uses (this pass has no binding for NRD's own CB).
  if (linearViewZ >= args.cloudMissLinearViewZ) {
    return kNoSurfaceKm;
  }
  // Linear view Z is measured along the camera forward axis; the march needs slant range.
  const float cosToForward = max(abs(cloudRay.viewDirection.z), 1e-3f);
  return numosWorldToKm(abs(linearViewZ) / cosToForward, args);
}

// jitterIndex: the pixel's own sample count (the frame index at full rate), so a pixel marched every
// 4th or 16th frame still walks the whole blue-noise sequence.
CloudPixelResult marchCloudPixel(uint2 pixelCoord, vec3 viewDirYUp, float tSurfaceKm, uint jitterIndex,
                                 CloudShadeContext ctx, AtmosphereArgs args) {
#ifdef CLOUD_REFERENCE_PATH_TRACE
  {
    CloudPixelResult reference;
    reference.color = refTracePixel(viewDirYUp, tSurfaceKm, pixelCoord,
                                    cloudShadeContextBeginRay(ctx, viewDirYUp, args), args);
    reference.depth = vec4(-1.0f, 0.0f, tSurfaceKm, 0.0f);
    return reference;
  }
#endif
  float jitterStatic = fastJitter(pixelCoord, 0u).x;
  float jitter = fastJitter(pixelCoord, jitterIndex).x;
  // Light-tap seed: the same blue noise read at another offset and time slice, so it is
  // independent of the view jitter but keeps its spatial spectrum.
  g_cloudLightJitterSeed = fastJitter(pixelCoord + uint2(61u, 23u), jitterIndex + 29u).x;

  // ===== View-ray raymarch, both layers (cloud_march_common.slangh) =====
  // marchCloudLayers marches every crossing of layer 1 and the optional layer 2 in the order the
  // ray meets them, front to back.
  vec3 accumColor = vec3(0.0f);
  float viewTransmittance = 1.0f;
  float entryDistanceKm;
  float meanCloudDepthKm;
  // Clamped at the primary surface, so the pass integrates [0, tSurface] and reports exactly the
  // cloud in front of the surface. Scaling a full-ray result afterwards (C_full * alphaFront /
  // alphaFull) does not work: it assumes radiance per unit opacity is constant along the ray, but
  // inside the deck near samples are shadowed interior and far ones lit edges, so the far cloud's
  // colour and pattern would leak onto the surface.
  g_cloudMarchSamples = 0u;
  marchCloudLayers(viewDirYUp, jitter, jitterStatic,
                   /*tMinClamp=*/0.0f,
                   /*tMaxClamp=*/tSurfaceKm,
                   args.cloudViewSamples, ctx, args,
                   accumColor, viewTransmittance,
                   entryDistanceKm, meanCloudDepthKm);

  CloudPixelResult result;
  // Premultiplied: rgb = accumulated cloud radiance, a = view-ray cloud transmittance.
  result.color = vec4(accumColor, viewTransmittance);
  // Entry, mean and surface distance in km; the density evaluation count in .w.
  result.depth = vec4(entryDistanceKm, meanCloudDepthKm, tSurfaceKm, float(g_cloudMarchSamples));
  return result;
}

// ===== Temporal filter and sparse-march reconstruction (docs/cloud-temporal-filter.md) =====
//
// The march is noisy (jittered view steps and light taps), and the host's denoiser and upscaler never
// see this layer (the upscaler is told to trust the current frame on cloud pixels), so the pass
// averages its own noise and, with the quarter or sixteenth march, rebuilds the pixels it did not
// march this frame. The output must be clean on its own. Per pixel the pass keeps:
//   * the history (premultiplied radiance, transmittance): the running mean of the values the pixel was
//     given, a new value weighted c / (N + c);
//   * N, the history's length in samples (state .x). A pixel's own march counts 1; a value interpolated
//     from neighbouring marches counts less (it is a neighbour's sample, not this pixel's). N is capped
//     by how the history was reprojected: texel-exact (a still view, still clouds) up to
//     cloudTemporalStillSamples, so a still image converges to the mean of the pixel's own marches and
//     stops shimmering; resampled (camera or wind moving) to about 1 / (1 - cloudTemporalHistoryWeight)
//     frames, as every bilinear resample blurs it; inside the deck also by the motion ambiguity (lag cap);
//   * a short "fast" history (about 4 samples) of luminance and transmittance (state .yz), fed the same
//     values with the same weights. The long and the fast history average the same input, so they differ
//     only by noise, unless the content changed. Where they differ by more than their noise allows (the
//     spread of the fresh neighbourhood, scaled to the two averages' lengths), N shrinks by the square
//     of the excess and the long history is clipped toward the fresh neighbourhood in proportion
//     (cloudTemporalClip). In an unchanged view the test passes and nothing is clipped, so no
//     per-frame clamp to raw jittered samples re-injects their noise; a filter that clips to the
//     fresh 3x3 box every frame never converges.
// While the history is short (after a reset) the new value is a wide Gaussian of the fresh samples
// (blurred, not sparkling), handing over to the pixel's own march (or the bilinear interpolation of
// the sample lattice) as N reaches kCloudTemporalFillSamples.
//
// Rays: every march of a pixel goes through the pixel at its own sub-pixel offset (an R2 sequence over
// the pixel's own sample count, rotated per pixel by blue noise), along the unjittered camera, and
// the history is reprojected with the unjittered matrices. The host's camera jitter is a Halton
// sequence over its frame index; the cell march visits a pixel every 4th (16th) frame, and base-2
// Halton read at stride 4 (16) stays inside one quarter (sixteenth) of the pixel, a different one
// for each position in the cell. That fixed sub-pixel pattern shows as stairs along edges. The
// pixel's own offsets cover the footprint evenly whatever the host does, so the layer converges to
// each pixel's box-filtered value and carries no per-frame jitter for an upscaler that trusts it.
// The reference path tracer skips the filter; the viewer accumulates it exactly.
#ifndef CLOUD_REFERENCE_PATH_TRACE
#define CLOUD_TEMPORAL_FILTER 1
#endif
static const float kCloudTemporalClipSigma = 1.25f;
static const float kCloudTemporalRadianceClipSigma = 4.0f;  // safety box for lighting changes
static const float kCloudTemporalMinOpacity = 0.02f;
static const float kCloudTemporalStillTexels = 0.05f;      // reprojection offsets below this are "still"
static const float kCloudTemporalFastSamples = 4.0f;       // length of the fast history
static const float kCloudTemporalChangeSigmas = 4.0f;      // a disagreement beyond this many sigmas ...
static const float kCloudTemporalChangeRelative = 0.02f;   // ... and this relative luminance ...
static const float kCloudTemporalChangeOpacity = 0.01f;    // ... and this transmittance is a change
static const float kCloudTemporalFillSamples = 4.0f;       // below this length the wide fill blends in
static const float kCloudTemporalShortSamples = 8.0f;      // the spatial denoise works below this length
static const float kCloudTemporalMaxSamples = 1024.0f;
// An interpolated value's weight (against an own march's 1) while the history is being resampled. In a
// still view it is 0, so each pixel converges to its own marches; a reset history takes it at full
// weight until the pixel has a sample's worth.
static const float kCloudInterpolatedMovingWeight = 0.25f;
groupshared vec4 gs_cloudFresh[CLOUD_GROUP_Y][CLOUD_GROUP_X];
groupshared vec4 gs_cloudFreshDepth[CLOUD_GROUP_Y][CLOUD_GROUP_X];
groupshared float gs_cloudFreshFlash[CLOUD_GROUP_Y][CLOUD_GROUP_X];  // g_cloudFlashAccum of each march
groupshared float gs_cloudFreshLayer2[CLOUD_GROUP_Y][CLOUD_GROUP_X];  // g_cloudLayer2Share of each march

// History for the temporal filter, reprojected through the point the cloud actually sits at (the
// fresh transmittance-weighted mean depth), with the camera's translation, not just its rotation:
// rotation-only reprojection is exact for a distant deck but smears everything near the camera,
// worst while flying through cloud. A history texel whose own mean depth disagrees with that
// point's distance from the previous camera saw different cloud and is dropped, but only when the
// disagreement matters: with the camera's move d, a depth error from D1 to D2 shifts the
// reprojection by about d |1/D1 - 1/D2| radians, so a still camera keeps everything (the mean depth
// of a jittered march wobbles) and a moving one drops what would land over a pixel off.
static const float kCloudTemporalParallaxPixels = 1.0f;
static const float kCloudTemporalFarKm = 400.0f;  // stand-in depth for cloudless pixels

// Inside the deck no single reprojection is right. The cloud's texture moves with the
// wind and the camera at its own depth, but in-cloud the image is mostly smooth in-scatter whose
// shape (the moon's or sun's forward glow) is fixed to the view direction, and a pixel's radiance
// comes from every depth from the camera out. Reprojected at the mean depth, that glow drifts with
// the wind a pixel or two a frame, and the opacity clamp cannot stop it: the view ray is opaque
// everywhere in-cloud. So where the cloud fills the ray from close to the camera (entry well short
// of the mean depth), the two hypotheses' disagreement - mean depth with wind vs. direction only -
// caps the history's length so the filter's lag (about that many frames of that motion) stays small.
// What a lag costs is the image's gradient times the lag, so the allowed lag comes from the
// history's own local gradient (over 4 px, so its noise barely counts): about a pixel at a cloud
// edge, many pixels across smooth in-cloud glow, where the extra averaging is what keeps the noise
// down while flying through fog.
static const float kCloudTemporalLagErrorTolerance = 0.02f;  // relative radiance (or opacity) error
static const float kCloudTemporalMinLagPixels = 1.0f;
static const float kCloudTemporalMaxLagPixels = 16.0f;
static const float kCloudTemporalAmbiguityFloorPixels = 0.25f;  // below this, no cap

float cloudLuminance(vec3 rgb) {
  return dot(rgb, vec3(0.2126f, 0.7152f, 0.0722f));
}

// The k-th own march of `pixelCoord`: its offset from the pixel centre, in pixels (each component in
// [-0.5, 0.5)). R2 over k, so any run of a pixel's marches covers its footprint evenly; rotated per
// pixel (Cranley-Patterson, blue noise) so neighbours never share an offset.
vec2 cloudSubpixelOffset(uint2 pixelCoord, uint k) {
  // Texel offsets the march's own blue-noise reads (fastJitter) do not use.
  const vec2 rotation = vec2(BlueNoise.Load(int4(int2((pixelCoord + uint2(97u, 13u)) & 127u), 0, 0)),
                             BlueNoise.Load(int4(int2((pixelCoord + uint2(29u, 113u)) & 127u), 1, 0)));
  return frac(vec2(0.7548776662f, 0.5698402910f) * float(k & 4095u) + rotation) - 0.5f;
}

// The unjittered camera ray through `offset` pixels from the centre of `pixelCoord`.
CameraDirections cloudPixelRay(uint2 pixelCoord, uint2 outputDim, vec2 offset) {
  return cameraScreenUVToDirection(cloudCamera, (vec2(pixelCoord) + 0.5f + offset) / vec2(outputDim), false);
}

// Largest local rate of change, per pixel, of last frame's output around `texel`: relative for the
// radiance, absolute for the transmittance.
float cloudHistoryGradient(int2 texel, int2 maxTexel) {
  const vec4 xp = loadPrevCloudRender(clamp(texel + int2(2, 0), int2(0), maxTexel));
  const vec4 xm = loadPrevCloudRender(clamp(texel - int2(2, 0), int2(0), maxTexel));
  const vec4 yp = loadPrevCloudRender(clamp(texel + int2(0, 2), int2(0), maxTexel));
  const vec4 ym = loadPrevCloudRender(clamp(texel - int2(0, 2), int2(0), maxTexel));
  const float lum = 0.25f * (cloudLuminance(xp.rgb) + cloudLuminance(xm.rgb)
                             + cloudLuminance(yp.rgb) + cloudLuminance(ym.rgb));
  const vec2 gLum = vec2(cloudLuminance(xp.rgb) - cloudLuminance(xm.rgb),
                         cloudLuminance(yp.rgb) - cloudLuminance(ym.rgb)) * 0.25f;
  const vec2 gAlpha = vec2(xp.a - xm.a, yp.a - ym.a) * 0.25f;
  return max(length(gLum) / max(lum, 1e-6f), length(gAlpha));
}

struct CloudHistory {
  vec4  color;         // premultiplied radiance, transmittance
  vec4  state;         // .x length in samples, .y fast luminance, .z fast transmittance
  float stillness;     // 1: reprojected texel-exact (nothing moved); 0: resampled
  float lagCapFrames;  // in-cloud motion ambiguity: the longest history, in frames, worth keeping
};

// The reprojected history of `pixelCoord`, whose centre ray is `worldDir`, false when there is none.
// layer2Share: layer 2's share of this pixel's cloud (g_cloudLayer2Share), whose base may follow the
// camera (args.cloudLayer2FollowStepKm): that share moved with the camera, not with the world.
bool cloudTemporalHistory(uint2 pixelCoord, uint2 outputDim, vec3 worldDir, vec4 freshDepth,
                          float tSurfaceKm, vec4 freshColor, float freshFlash, float layer2Share,
                          AtmosphereArgs args, out CloudHistory h) {
  h.color = vec4(0.0f, 0.0f, 0.0f, 1.0f);
  h.state = vec4(0.0f);
  h.stillness = 0.0f;
  h.lagCapFrames = kCloudTemporalMaxSamples;
  const bool hasCloud = freshDepth.x >= 0.0f;
  // How much of the ray the cloud fills from near the camera: 0 for a cloud seen from outside (entry
  // at its mean depth), 1 from inside the deck.
  const float volumetric = hasCloud
    ? saturate((freshDepth.y - max(freshDepth.x, 0.0f)) / max(freshDepth.y, 1e-3f)) : 0.0f;
  const float distKm = hasCloud ? max(freshDepth.y, 1e-3f) : min(tSurfaceKm, kCloudTemporalFarKm);
  vec3 worldPoint = cameraGetWorldPosition(cloudCamera) + worldDir * numosKmToWorld(distKm, args);
  if (hasCloud) {
    // The wind carried that cloud here; last frame it sat one wind step back.
    worldPoint -= numosKmYUpToWorld(vec3(args.cloudWindStepXKm, 0.0f, args.cloudWindStepZKm), args);
    // A layer 2 following the camera climbed with it: last frame its share sat one step lower.
    if (args.cloudLayer2FollowStepKm != 0.0f) {
      worldPoint -= numosKmYUpToWorld(vec3(0.0f, args.cloudLayer2FollowStepKm * layer2Share, 0.0f), args);
    }
  }
  // Unjittered: the history holds pixel-centred values (see the rays above).
  const vec4 prevClip = mul(cloudCamera.prevWorldToProjection, vec4(worldPoint, 1.0f));
  if (!all(isfinite(prevClip)) || prevClip.w <= 1e-6f) {
    return false;
  }
  const vec2 prevUv = cameraNDCToScreenUV(cloudCamera, prevClip.xy / prevClip.w);
  if (any(prevUv < vec2(0.0f)) || any(prevUv >= vec2(1.0f))) {
    return false;
  }
  float ambiguityPx = 0.0f;
  if (hasCloud) {
    const vec3 farPoint = cameraGetWorldPosition(cloudCamera)
                        + worldDir * numosKmToWorld(kCloudTemporalFarKm, args);
    const vec4 farClip = mul(cloudCamera.prevWorldToProjection, vec4(farPoint, 1.0f));
    if (all(isfinite(farClip)) && farClip.w > 1e-6f) {
      const vec2 farUv = cameraNDCToScreenUV(cloudCamera, farClip.xy / farClip.w);
      ambiguityPx = max(length((prevUv - farUv) * vec2(outputDim)) * volumetric
                        - kCloudTemporalAmbiguityFloorPixels, 0.0f);
    }
  }
  const float prevDistKm = numosWorldToKm(length(worldPoint - cameraGetPreviousWorldPosition(cloudCamera)), args);
  const float moveKm = numosWorldToKm(length(cameraGetWorldPosition(cloudCamera)
                                             - cameraGetPreviousWorldPosition(cloudCamera)), args);
  const float pixelRad = 2.0f / max(abs(cloudCamera.viewToProjection[1][1]) * float(outputDim.y), 1e-6f);
  const bool currentIsSky = tSurfaceKm >= kNoSurfaceKm;

  vec2 texelF = prevUv * vec2(outputDim) - 0.5f;
  const vec2 nearest = round(texelF);
  const vec2 offTexel = abs(texelF - nearest);
  h.stillness = saturate(1.0f - max(offTexel.x, offTexel.y) / kCloudTemporalStillTexels);
  if (all(offTexel < 0.01f)) {
    texelF = nearest;
  }
  const int2 baseTexel = int2(floor(texelF));
  const vec2 f = texelF - vec2(baseTexel);
  const int2 maxTexel = int2(outputDim) - int2(1);
  const float weights[4] = {
    (1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y),
    (1.0f - f.x) * f.y,          f.x * f.y };
  const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
  vec4 sum = vec4(0.0f);
  vec4 stateSum = vec4(0.0f);
  float weightSum = 0.0f;
  [unroll]
  for (uint i = 0u; i < 4u; ++i) {
    const int2 texel = clamp(baseTexel + offsets[i], int2(0), maxTexel);
    const vec4 depth = PrevCloudDepth.Load(int3(texel, 0));
    // Same occluding surface (or sky) as before.
    if ((depth.z >= kNoSurfaceKm) != currentIsSky
        || (!currentIsSky && abs(depth.z - tSurfaceKm)
                             > args.cloudReprojectDepthTolerance * max(max(depth.z, tSurfaceKm), 1e-3f))) {
      continue;
    }
    // Same cloud, as far as parallax can tell (see above). Inside the deck there is no one depth to
    // test (the jittered mean wobbles by tens of percent), so the test relaxes as the ray turns
    // volumetric and the lag cap above bounds the motion error instead.
    const float tapDistKm = depth.x >= 0.0f ? max(depth.y, 1e-3f) : kCloudTemporalFarKm;
    if (moveKm * abs(1.0f / tapDistKm - 1.0f / max(prevDistKm, 1e-3f))
        > kCloudTemporalParallaxPixels * pixelRad / max(1.0f - volumetric, 1e-3f)) {
      continue;
    }
    sum += loadPrevCloudRender(texel) * weights[i];
    stateSum += PrevCloudState.Load(int3(texel, 0)) * weights[i];
    weightSum += weights[i];
  }
  if (weightSum < 0.5f) {
    return false;
  }
  h.color = sum / weightSum;
  h.state = stateSum / weightSum;
  // Taps were dropped: the history is partly from elsewhere, so it counts for less.
  h.state.x *= saturate(weightSum);
  // Lightning: the history holds last frame's flash, whose light scales with its envelope e, so
  // the history is brought to this frame's level instead of being dropped (raw noise for the whole
  // flash, day or night) or kept as is (a lagging, clamp-mangled flash). The fresh march gives the
  // flash's share here (freshFlash, g_cloudFlashAccum, of freshColor's luminance). A new flash adds
  // it; a changing one scales the history's flash share by r = e / e_prev, so that share's own error
  // scales with it and stays relative through the decay (adding the change instead would pile each
  // frame's fresh noise onto a fading signal). The flash's end (e = 0, cut at 1e-3 of its peak) is
  // left to the change test. The fast history follows by the same luminance change.
  if (args.lightningEnvelope > 0.0f) {
    const float before = cloudLuminance(h.color.rgb);
    if (args.lightningEnvelopePrevious > 0.0f) {
      const float r = args.lightningEnvelope / args.lightningEnvelopePrevious;
      const float flashThen = freshFlash / r;  // the flash's share as the history saw it
      const float restThen = max(cloudLuminance(freshColor.rgb) - freshFlash, 0.0f);
      const float share = flashThen / max(flashThen + restThen, 1e-30f);
      h.color.rgb *= 1.0f + share * (r - 1.0f);
    } else {
      h.color.rgb += args.lightningColor * freshFlash;
    }
    h.state.y = max(h.state.y + cloudLuminance(h.color.rgb) - before, 0.0f);
  }
  if (ambiguityPx > 0.0f) {
    const float lagPx = clamp(kCloudTemporalLagErrorTolerance
                                / max(cloudHistoryGradient(int2(round(texelF)), maxTexel), 1e-6f),
                              kCloudTemporalMinLagPixels, kCloudTemporalMaxLagPixels);
    // The exponential weight w = lagPx / (lagPx + ambiguityPx) as a length: 1 / (1 - w) frames.
    h.lagCapFrames = 1.0f + lagPx / ambiguityPx;
  }
  return true;
}

// The range of the fresh neighbourhood's radiance per unit opacity (the cloud's own brightness,
// premultiplied rgb / opacity), over the samples whose opacity is known (above
// kCloudTemporalMinOpacity). Empty (lo > hi) when none is.
struct CloudOpacityRadianceRange {
  vec3 lo;
  vec3 hi;
};

CloudOpacityRadianceRange cloudOpacityRadianceRangeEmpty() {
  CloudOpacityRadianceRange r;
  r.lo = vec3(3.0e38f);
  r.hi = vec3(-3.0e38f);
  return r;
}

void cloudOpacityRadianceRangeAdd(inout CloudOpacityRadianceRange r, vec4 v) {
  const float opacity = 1.0f - v.a;
  if (opacity > kCloudTemporalMinOpacity) {
    const vec3 perOpacity = v.rgb / opacity;
    r.lo = min(r.lo, perOpacity);
    r.hi = max(r.hi, perOpacity);
  }
}

// Split clip of a history value toward the fresh neighbourhood (its mean and standard deviation, and
// the range of its radiance per unit opacity), applied only where the change test found the history
// out of date. Cloud that moved or changed shows in the opacity, so the history's transmittance is
// clipped to the fresh box and its premultiplied radiance follows by the same opacity ratio. That
// rescale keeps the history's radiance per unit opacity, which is only as good as the opacity it was
// divided by: a thin edge texel (opacity a few percent) carries a noisy, often bright one, and scaled
// up to thick cloud's opacity it makes pixels brighter than both the cloud and the sky behind it (a
// speckled light rim around clouds under a low sun). So the rescaled history's radiance per unit
// opacity is kept inside the fresh samples' range: the rescale may thicken or thin the cloud, never
// invent a brightness none of them has. The premultiplied radiance itself only gets a wide box: under
// a grazing sun the light lottery is strongly right-skewed, a few samples mostly miss the bright ones,
// and a tight box would drag the history to the median, below the mean.
vec4 cloudTemporalClip(vec4 history, vec4 mean, vec4 sigma, CloudOpacityRadianceRange range) {
  vec4 clipped = history;
  clipped.a = clamp(history.a, mean.a - kCloudTemporalClipSigma * sigma.a,
                    mean.a + kCloudTemporalClipSigma * sigma.a);
  const float historyOpacity = 1.0f - history.a;
  const bool opacityKnown = historyOpacity > kCloudTemporalMinOpacity;
  if (opacityKnown) {
    clipped.rgb *= (1.0f - clipped.a) / historyOpacity;
  }
  // Without cloud in the history there is no radiance to rescale: clip it like the opacity.
  const float rgbSigmas = opacityKnown ? kCloudTemporalRadianceClipSigma : kCloudTemporalClipSigma;
  clipped.rgb = clamp(clipped.rgb, mean.rgb - rgbSigmas * sigma.rgb, mean.rgb + rgbSigmas * sigma.rgb);
  // Last, so neither the rescale nor the box above can leave it.
  if (opacityKnown && all(range.lo <= range.hi)) {
    const float clippedOpacity = 1.0f - clipped.a;
    clipped.rgb = clamp(clipped.rgb, range.lo * clippedOpacity, range.hi * clippedOpacity);
  }
  return clipped;
}

// Cell march. One pixel of every 2x2 (or 4x4) cell is marched per frame, cycling through the cell,
// so the march costs a quarter (or a sixteenth). The marched samples form a lattice of spacing 2 (4),
// shifted by the frame's phase. Each pixel gathers the samples of the 3x3 cells around its own that
// see its surface (sky, or a surface at about the same distance) into a CloudNeighbourhood: its own
// march when it is the cell's pixel this frame, the bilinear interpolation of the lattice at the
// pixel (the four samples around it, weighted by their actual distance; never the nearest copied),
// a wide Gaussian for a pixel without history, and the moments for the change test. The full-rate
// path is the same with a lattice of spacing 1 and every pixel its own sample.

uint2 cloudQuarterPhase(uint frameIdx) {
  const uint i = frameIdx & 3u;
  return uint2(i == 1u || i == 2u ? 1u : 0u, i == 1u || i == 3u ? 1u : 0u);
}

// The sixteenth march's pixel within its 4x4 cell on frame i: the 4x4 Bayer order, so any run of
// frames is spread over the cell rather than sweeping along it.
uint2 cloudSixteenthPhase(uint frameIdx) {
  static const uint kBayerInverse[16] = { 0u, 10u, 2u, 8u, 5u, 15u, 7u, 13u, 1u, 11u, 3u, 9u, 4u, 14u, 6u, 12u };
  const uint p = kBayerInverse[frameIdx & 15u];
  return uint2(p & 3u, p >> 2u);
}

// Whether a neighbour's fresh sample (`freshColor`, `freshDepth`, its surface at freshDepth.z) can
// stand in for a pixel whose surface is `tSurfaceKm`: the same surface (sky, or about the same
// distance), or two surfaces the cloud cannot tell apart - both in front of where the neighbour
// found cloud, or both behind cloud its ray had already made opaque. The last two keep far ground
// near the horizon, whose distance changes fast across pixels, from falling back to raw marches.
bool cloudSameSurface(float tSurfaceKm, vec4 freshColor, vec4 freshDepth, AtmosphereArgs args) {
  const float tFresh = freshDepth.z;
  const bool skyA = tSurfaceKm >= kNoSurfaceKm;
  const bool skyB = tFresh >= kNoSurfaceKm;
  if (skyA == skyB && (skyA || abs(tSurfaceKm - tFresh)
                                 <= args.cloudReprojectDepthTolerance * max(max(tSurfaceKm, tFresh), 1e-3f))) {
    return true;
  }
  const float nearKm = min(tSurfaceKm, tFresh);
  const float farKm = max(tSurfaceKm, tFresh);
  const bool cloudless = freshDepth.x < 0.0f;
  if (!skyA && !skyB && cloudless) {
    return true;  // neither ray met cloud before its surface
  }
  if (!cloudless && farKm < freshDepth.x) {
    return true;  // both surfaces in front of the cloud
  }
  // Both behind cloud the neighbour's ray made (nearly) opaque well before the nearer surface.
  return !cloudless && freshColor.a < 0.02f && nearKm > 2.0f * freshDepth.y;
}

struct CloudNeighbourhood {
  vec4  own;           // this pixel's own march this frame (hasOwn)
  float ownFlash;
  float ownLayer2;
  bool  hasOwn;
  vec4  interp;        // bilinear interpolation of the sample lattice
  vec2  interpDepth;   // cloud entry / mean depth, over its cloudy samples
  float interpWeight;
  float interpCloudy;
  float interpFlash;
  float interpLayer2;
  vec4  wide;          // Gaussian, sigma one lattice spacing: the fill for a short history
  vec2  wideDepth;
  float wideWeight;
  float wideCloudy;
  float wideFlash;
  float wideLayer2;
  vec4  m1;            // moments of the samples (unweighted), for the change test and the clip
  vec4  m2;
  float count;
  CloudOpacityRadianceRange range;
};

CloudNeighbourhood cloudNeighbourhoodEmpty() {
  CloudNeighbourhood nb;
  nb.own = vec4(0.0f, 0.0f, 0.0f, 1.0f);
  nb.ownFlash = 0.0f;
  nb.ownLayer2 = 0.0f;
  nb.hasOwn = false;
  nb.interp = vec4(0.0f);
  nb.interpDepth = vec2(0.0f);
  nb.interpWeight = 0.0f;
  nb.interpCloudy = 0.0f;
  nb.interpFlash = 0.0f;
  nb.interpLayer2 = 0.0f;
  nb.wide = vec4(0.0f);
  nb.wideDepth = vec2(0.0f);
  nb.wideWeight = 0.0f;
  nb.wideCloudy = 0.0f;
  nb.wideFlash = 0.0f;
  nb.wideLayer2 = 0.0f;
  nb.m1 = vec4(0.0f);
  nb.m2 = vec4(0.0f);
  nb.count = 0.0f;
  nb.range = cloudOpacityRadianceRangeEmpty();
  return nb;
}

// One fresh sample that sees the pixel's surface, `offset` pixels from the pixel (sample position -
// pixel position), on a lattice of spacing `spacing` (1, 2 or 4).
void cloudNeighbourhoodAdd(inout CloudNeighbourhood nb, vec4 v, vec4 d, float flash, float layer2, vec2 offset,
                           float spacing) {
  if (all(offset == vec2(0.0f))) {
    nb.own = v;
    nb.ownFlash = flash;
    nb.ownLayer2 = layer2;
    nb.hasOwn = true;
  }
  const vec2 tent = max(1.0f - abs(offset) / spacing, vec2(0.0f));
  const float wi = tent.x * tent.y;
  const float ww = exp(-dot(offset, offset) * (0.5f / (spacing * spacing)));
  nb.interp += wi * v;
  nb.interpWeight += wi;
  nb.interpFlash += wi * flash;
  nb.interpLayer2 += wi * layer2;
  nb.wide += ww * v;
  nb.wideWeight += ww;
  nb.wideFlash += ww * flash;
  nb.wideLayer2 += ww * layer2;
  if (d.x >= 0.0f) {
    nb.interpDepth += wi * d.xy;
    nb.interpCloudy += wi;
    nb.wideDepth += ww * d.xy;
    nb.wideCloudy += ww;
  }
  nb.m1 += v;
  nb.m2 += v * v;
  nb.count += 1.0f;
  cloudOpacityRadianceRangeAdd(nb.range, v);
}

struct CloudResolved {
  vec4 color;  // -> cloudRenderOutput
  vec4 depth;  // -> cloudDepthOutput
  vec4 state;  // -> cloudStateOutput
};

// A pixel's output from its neighbourhood and its history. `spacing`: the sample lattice's (1 at full
// rate). `marchSamples`: the density evaluation count for depth.w. A pixel no sample sees (a
// silhouette) is marched here.
CloudResolved cloudResolvePixel(uint2 pixelCoord, uint2 outputDim, float tSurfaceKm, CloudNeighbourhood nb,
                                float spacing, float marchSamples, CloudShadeContext ctx, AtmosphereArgs args) {
  if (nb.interpWeight <= 1e-4f && nb.wideWeight <= 1e-4f) {
    const CameraDirections ray = cloudPixelRay(pixelCoord, outputDim,
                                               cloudSubpixelOffset(pixelCoord, args.cloudRenderFrameIdx));
    const vec3 viewDirYUp = worldToAtmosphereYUp(ray.worldDirection, args.isZUp != 0u, args.flipUpAxis != 0);
    const CloudPixelResult marched = marchCloudPixel(pixelCoord, viewDirYUp, tSurfaceKm, args.cloudRenderFrameIdx,
                                                     ctx, args);
    cloudNeighbourhoodAdd(nb, marched.color, marched.depth, g_cloudFlashAccum, g_cloudLayer2Share, vec2(0.0f),
                          spacing);
    marchSamples = marched.depth.w;
  }
  const bool hasInterp = nb.interpWeight > 1e-4f;
  const vec4 wideColor = nb.wideWeight > 1e-4f ? nb.wide / nb.wideWeight : nb.interp / nb.interpWeight;
  const vec4 interpColor = hasInterp ? nb.interp / nb.interpWeight : wideColor;
  const float flash = hasInterp ? nb.interpFlash / nb.interpWeight : nb.wideFlash / nb.wideWeight;
  const float layer2 = hasInterp ? nb.interpLayer2 / nb.interpWeight : nb.wideLayer2 / nb.wideWeight;
  // A pixel's best single value this frame: its own march, else the lattice interpolated at it.
  const vec4 best = nb.hasOwn ? nb.own : interpColor;

  CloudResolved r;
  r.depth = vec4(-1.0f, 0.0f, tSurfaceKm, max(marchSamples, 0.0f));
  if (hasInterp ? nb.interpCloudy > 1e-6f : nb.wideCloudy > 1e-6f) {
    r.depth.xy = hasInterp ? nb.interpDepth / nb.interpCloudy : nb.wideDepth / nb.wideCloudy;
  }

  const bool filterOn = args.cloudTemporalHistoryWeight > 0.0f;
  const bool historyValid = filterOn && (args.cloudScreenInterleave & 0x20000u) != 0u;
  CloudHistory h;
  if (historyValid && cloudTemporalHistory(pixelCoord, outputDim, cloudPixelRay(pixelCoord, outputDim, vec2(0.0f)).worldDirection,
                                           r.depth, tSurfaceKm, interpColor, flash, layer2, args, h)) {
    // Effective samples a frame while resampling: the pixel's own march once per cell area, the
    // interpolated value on the other frames.
    const float cellArea = spacing * spacing;
    const float samplesPerFrame = (1.0f + (cellArea - 1.0f) * kCloudInterpolatedMovingWeight) / cellArea;
    const float movingCap = max(samplesPerFrame / max(1.0f - args.cloudTemporalHistoryWeight, 1e-3f), 1.0f);
    const float cap = min(lerp(movingCap, max(args.cloudTemporalStillSamples, movingCap), h.stillness),
                          max(h.lagCapFrames * samplesPerFrame, 1.0f));
    float n = min(max(h.state.x, 0.0f), cap);
    vec4 history = h.color;
    if (!all(isfinite(history)) || !all(isfinite(h.state.yz)) || !isfinite(n)) {
      n = 0.0f;
      history = vec4(0.0f, 0.0f, 0.0f, 1.0f);
      h.state = vec4(0.0f, cloudLuminance(wideColor.rgb), wideColor.a, 0.0f);
    }

    // Change test: the long and the fast history average the same values, so beyond their noise
    // they disagree only where the content changed.
    if (nb.count >= 2.0f && n >= 1.0f) {
      const vec4 mean = nb.m1 / nb.count;
      const vec4 sigma = sqrt(max(nb.m2 / nb.count - mean * mean, vec4(0.0f)));
      const float spread = sqrt(1.0f / min(n, kCloudTemporalFastSamples) + 1.0f / n);
      const float slowLum = cloudLuminance(history.rgb);
      const float tolLum = kCloudTemporalChangeSigmas * cloudLuminance(sigma.rgb) * spread
                         + kCloudTemporalChangeRelative * max(slowLum, h.state.y) + 1e-6f;
      const float tolAlpha = kCloudTemporalChangeSigmas * sigma.a * spread + kCloudTemporalChangeOpacity;
      const float excessLum = abs(slowLum - h.state.y) / tolLum;
      const float excessAlpha = abs(history.a - h.state.z) / tolAlpha;
      // A change of the cloud itself shows in its opacity: shorten the history and clip it toward the
      // fresh neighbourhood (cloudTemporalClip: the opacity, the radiance rescaled with it, kept inside
      // the fresh radiance-per-opacity range).
      if (excessAlpha > 1.0f) {
        n /= excessAlpha * excessAlpha;
        history = lerp(history, cloudTemporalClip(history, mean, sigma, nb.range), saturate(excessAlpha - 1.0f));
      }
      // A disagreement in brightness alone is a lighting change - or, where the light lottery is heavily
      // right-skewed (the forward-scattering lobe around the sun behind a thin sheet), one rare bright
      // sample the few fresh neighbours never saw. Clipping to those neighbours there drags the history to
      // their median, below the mean: a dark disc around the sun. So brightness only shortens the history
      // - at most halving it per frame, which a real lighting change still follows within a few frames -
      // and never clips it: unbiased either way.
      if (excessLum > 1.0f) {
        n = max(n / (excessLum * excessLum), 0.5f * n);
      }
    }

    // This frame's value and its weight. A short history takes the wide fill, handing over to the
    // pixel's own value as it grows; the interpolated value of a pixel not marched this frame counts
    // only while the history is resampled, or while it is shorter than one sample.
    const float fill = saturate(n / kCloudTemporalFillSamples);
    const vec4 fresh = lerp(wideColor, best, fill);
    const float c = nb.hasOwn ? 1.0f : max(kCloudInterpolatedMovingWeight * (1.0f - h.stillness), saturate(1.0f - n));
    const float total = n + c;
    r.color = total > 1e-6f ? (history * n + fresh * c) / total : fresh;
    const float fastN = min(n, kCloudTemporalFastSamples);
    const vec2 fastIn = vec2(cloudLuminance(fresh.rgb), fresh.a);
    const vec2 fast = fastN + c > 1e-6f ? (h.state.yz * fastN + fastIn * c) / (fastN + c) : fastIn;
    r.state = vec4(min(total, kCloudTemporalMaxSamples), fast, 0.0f);
  } else {
    // No history: the filter off gives the raw march; otherwise the wide fill (a blur, not sparkle),
    // which counts as one sample.
    r.color = filterOn ? wideColor : best;
    r.state = vec4(filterOn ? 1.0f : 0.0f, cloudLuminance(r.color.rgb), r.color.a, 0.0f);
  }
  // The spatial denoise (cloud_denoise.comp.slang) works only where the history is short: how short
  // rides in the fraction of depth.w (whose integer part is the density evaluation count), so a
  // still, converged view keeps its detail.
  const float shortHistory = saturate(1.0f - r.state.x / kCloudTemporalShortSamples);
  r.depth.w = floor(r.depth.w) + 0.99f * shortHistory;
  return r;
}

[shader("compute")]
[numthreads(CLOUD_GROUP_X, CLOUD_GROUP_Y, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID) {
  uint2 outputDim;
  cloudRenderOutput.GetDimensions(outputDim.x, outputDim.y);

  AtmosphereArgs args = atmosphereArgs;

  const uint period = 1u;
  const uint2 cellDim = uint2(1u, 1u);
  // Each lane owns a whole cell so skipped marches save work across the warp.
  const uint2 cellOrigin = dispatchId.xy * cellDim;
  const bool cellInBounds = all(cellOrigin < outputDim);

  // Clear sky: full transmittance, zero radiance. The effective coverage is the larger of the two
  // layers' means, so a sky with only layer 2 still marches.
  float effectiveCoverageMean = args.cloudCoverageMean;
  if (args.cloudLayer2Enable != 0u) {
    effectiveCoverageMean = max(effectiveCoverageMean, args.cloudLayer2CoverageMean);
  }
  if (args.cloudEnabled < 0.5f || effectiveCoverageMean < 0.01f || args.cloudViewSamples == 0u) {
    // Clear every pixel this lane owns: the cell marches dispatch one lane per cell, so clearing only
    // the cell's first pixel would leave the rest of the image stale.
    const bool quarterCells = period == 1u && (args.cloudScreenInterleave & 0x40000u) != 0u;
    const uint clearSize = (args.cloudScreenInterleave & 0x80000u) != 0u ? 4u : 2u;
    const uint2 clearCell = quarterCells ? uint2(clearSize, clearSize) : cellDim;
    for (uint sub = 0u; sub < clearCell.x * clearCell.y; ++sub) {
      const uint2 pixelCoord = dispatchId.xy * clearCell + uint2(sub % clearCell.x, sub / clearCell.x);
      if (any(pixelCoord >= outputDim)) {
        continue;
      }
      cloudRenderOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
      cloudDepthOutput[pixelCoord] = vec4(-1.0f, 0.0f, kNoSurfaceKm, 0.0f);
      cloudStateOutput[pixelCoord] = vec4(0.0f);
    }
    return;
  }

  // There is no below-horizon early-out. The deck's underside fills every direction down to the true
  // horizon, and a cut at y = 0 would draw a hard line along its far edge. cloudSlabSpan
  // (cloud_march_common.slangh) ends these rays on the planet instead, so the deck stops at the
  // horizon on its own.

  // The sky ring is sampled once per group (buildCloudShadeContextGroup); every lane gets here.
  CloudShadeContext ctx = buildCloudShadeContextGroup(args, groupThreadId.y * CLOUD_GROUP_X + groupThreadId.x);

#ifdef CLOUD_TEMPORAL_FILTER
  if (period == 1u && (args.cloudScreenInterleave & 0xC0000u) == 0xC0000u) {
    // Cell march: cells of 2x2 pixels (quarter march) or 4x4 (sixteenth march, bit 19), in two
    // dispatches. The march pass (one lane per cell) marches one pixel of every cell, cycling through
    // all of them, into the fresh-sample buffers; the rebuild pass (bit 20, one lane per pixel)
    // rebuilds every pixel from the fresh samples of the cells around it plus the temporal history.
    // Two passes let each pixel see cells outside its workgroup; in one pass every group would
    // rebuild its tile alone and the tiles would show as blocks.
    const bool sixteenth = (args.cloudScreenInterleave & 0x80000u) != 0u;
    const int cellSize = sixteenth ? 4 : 2;
    const int2 phase = int2(sixteenth ? cloudSixteenthPhase(args.cloudRenderFrameIdx)
                                      : cloudQuarterPhase(args.cloudRenderFrameIdx));
    // A pixel's own marches are its cell's every 4th (16th) frame: this is the pixel's own count.
    const uint ownIndex = args.cloudRenderFrameIdx >> (sixteenth ? 4u : 2u);
    const int2 cellsDim = (int2(outputDim) + int2(cellSize - 1)) / cellSize;
    if ((args.cloudScreenInterleave & 0x100000u) == 0u) {
      const int2 cell = int2(dispatchId.xy);
      if (any(cell >= cellsDim)) {
        return;
      }
      const int2 freshPixel = cell * cellSize + phase;
      CloudPixelResult fresh;
      fresh.color = vec4(0.0f, 0.0f, 0.0f, 1.0f);
      fresh.depth = vec4(-1.0f, 0.0f, kNoSurfaceKm, -1.0f);  // w < 0: no sample
      vec2 aux = vec2(0.0f);
      if (all(freshPixel < int2(outputDim))) {
        const uint2 pixelCoord = uint2(freshPixel);
        const CameraDirections cloudRay = cloudPixelRay(pixelCoord, outputDim, cloudSubpixelOffset(pixelCoord, ownIndex));
        const float tSurfaceKm = cloudSurfaceDistanceKm(pixelCoord, cloudRay, args);
        const vec3 viewDirYUp = worldToAtmosphereYUp(cloudRay.worldDirection, args.isZUp != 0u, args.flipUpAxis != 0);
        fresh = marchCloudPixel(pixelCoord, viewDirYUp, tSurfaceKm, ownIndex, ctx, args);
        aux = vec2(g_cloudFlashAccum, g_cloudLayer2Share);
      }
      cloudFreshColor[uint2(cell)] = fresh.color;
      cloudFreshDepth[uint2(cell)] = fresh.depth;
      cloudFreshAux[uint2(cell)] = aux;
      return;
    }

    const int2 q = int2(dispatchId.xy);
    if (any(q >= int2(outputDim))) {
      return;
    }
    const int2 cell = q / cellSize;
    const uint2 pixelCoord = uint2(q);
    const float tSurfaceKm = cloudSurfaceDistanceKm(pixelCoord, cloudPixelRay(pixelCoord, outputDim, vec2(0.0f)), args);
    CloudNeighbourhood nb = cloudNeighbourhoodEmpty();
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
      [unroll]
      for (int dx = -1; dx <= 1; ++dx) {
        const int2 c = cell + int2(dx, dy);
        if (any(c < int2(0)) || any(c >= cellsDim)) {
          continue;
        }
        const vec4 d = cloudFreshDepth[uint2(c)];
        const vec4 v = cloudFreshColor[uint2(c)];
        if (d.w < 0.0f || !cloudSameSurface(tSurfaceKm, v, d, args)) {
          continue;
        }
        const vec2 a = cloudFreshAux[uint2(c)];
        cloudNeighbourhoodAdd(nb, v, d, a.x, a.y, vec2(c * cellSize + phase - q), float(cellSize));
      }
    }
    // .w: the density evaluations of this cell's march, for the sample statistics and debug views.
    const CloudResolved r = cloudResolvePixel(pixelCoord, outputDim, tSurfaceKm, nb, float(cellSize),
                                              cloudFreshDepth[uint2(cell)].w, ctx, args);
    cloudRenderOutput[pixelCoord] = r.color;
    cloudDepthOutput[pixelCoord] = r.depth;
    cloudStateOutput[pixelCoord] = r.state;
    return;
  }

  if (period == 1u && (args.cloudScreenInterleave & 0x40000u) != 0u) {
    // Quarter march: one 2x2 cell per lane, its group grid shifted every frame like the full-rate
    // path below. Every lane reaches the barrier, in bounds or not.
    const int2 tileShift = int2((args.cloudRenderFrameIdx * 5u) % uint(CLOUD_GROUP_X),
                                (args.cloudRenderFrameIdx * 3u) % uint(CLOUD_GROUP_Y));
    const int2 cell = int2(dispatchId.xy) - tileShift;
    const int2 phase = int2(cloudQuarterPhase(args.cloudRenderFrameIdx));
    const uint ownIndex = args.cloudRenderFrameIdx >> 2u;
    const int2 freshPixel = cell * 2 + phase;
    const bool freshInBounds = all(cell >= int2(0)) && all(freshPixel < int2(outputDim));
    CloudPixelResult fresh;
    fresh.color = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    fresh.depth = vec4(-1.0f, 0.0f, kNoSurfaceKm, -1.0f);  // w < 0: no sample
    float freshFlash = 0.0f;
    float freshLayer2 = 0.0f;
    if (freshInBounds) {
      const uint2 pixelCoord = uint2(freshPixel);
      const CameraDirections cloudRay = cloudPixelRay(pixelCoord, outputDim, cloudSubpixelOffset(pixelCoord, ownIndex));
      const float tSurfaceKm = cloudSurfaceDistanceKm(pixelCoord, cloudRay, args);
      const vec3 viewDirYUp = worldToAtmosphereYUp(cloudRay.worldDirection, args.isZUp != 0u, args.flipUpAxis != 0);
      fresh = marchCloudPixel(pixelCoord, viewDirYUp, tSurfaceKm, ownIndex, ctx, args);
      freshFlash = g_cloudFlashAccum;
      freshLayer2 = g_cloudLayer2Share;
    }
    gs_cloudFresh[groupThreadId.y][groupThreadId.x] = fresh.color;
    gs_cloudFreshDepth[groupThreadId.y][groupThreadId.x] = fresh.depth;
    gs_cloudFreshFlash[groupThreadId.y][groupThreadId.x] = freshFlash;
    gs_cloudFreshLayer2[groupThreadId.y][groupThreadId.x] = freshLayer2;
    GroupMemoryBarrierWithGroupSync();
    if (any(cell < int2(0))) {
      return;
    }
    for (uint sub = 0u; sub < 4u; ++sub) {
      const int2 q = cell * 2 + int2(int(sub & 1u), int(sub >> 1u));
      if (any(q >= int2(outputDim))) {
        continue;
      }
      const uint2 pixelCoord = uint2(q);
      const float tSurfaceKm = cloudSurfaceDistanceKm(pixelCoord, cloudPixelRay(pixelCoord, outputDim, vec2(0.0f)), args);
      CloudNeighbourhood nb = cloudNeighbourhoodEmpty();
      [unroll]
      for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
          const int2 g = int2(groupThreadId.xy) + int2(dx, dy);
          if (any(g < int2(0)) || g.x >= CLOUD_GROUP_X || g.y >= CLOUD_GROUP_Y) {
            continue;
          }
          const vec4 d = gs_cloudFreshDepth[g.y][g.x];
          const vec4 v = gs_cloudFresh[g.y][g.x];
          if (d.w < 0.0f || !cloudSameSurface(tSurfaceKm, v, d, args)) {
            continue;
          }
          cloudNeighbourhoodAdd(nb, v, d, gs_cloudFreshFlash[g.y][g.x], gs_cloudFreshLayer2[g.y][g.x],
                                vec2((cell + int2(dx, dy)) * 2 + phase - q), 2.0f);
        }
      }
      // .w: the density evaluations of this cell's march (each pixel is marched one frame in four).
      const CloudResolved r = cloudResolvePixel(pixelCoord, outputDim, tSurfaceKm, nb, 2.0f,
                                                gs_cloudFreshDepth[groupThreadId.y][groupThreadId.x].w, ctx, args);
      cloudRenderOutput[pixelCoord] = r.color;
      cloudDepthOutput[pixelCoord] = r.depth;
      cloudStateOutput[pixelCoord] = r.state;
    }
    return;
  }

  // One pixel per lane. Every lane reaches the barrier, in bounds or not.
  if (period == 1u) {
    // The 3x3 neighbourhood stops at the group edge, so the group grid shifts every frame (the
    // host dispatches one extra group per axis) and its seams average out instead of showing.
    const int2 tileShift = int2((args.cloudRenderFrameIdx * 5u) % uint(CLOUD_GROUP_X),
                                (args.cloudRenderFrameIdx * 3u) % uint(CLOUD_GROUP_Y));
    const int2 shifted = int2(dispatchId.xy) - tileShift;
    const bool pixelInBounds = all(shifted >= int2(0)) && all(shifted < int2(outputDim));
    const uint2 pixelCoord = uint2(max(shifted, int2(0)));
    float tSurfaceKm = kNoSurfaceKm;
    CloudPixelResult fresh;
    fresh.color = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    fresh.depth = vec4(-1.0f, 0.0f, kNoSurfaceKm, -1.0f);  // w < 0: no sample
    float freshFlash = 0.0f;
    float freshLayer2 = 0.0f;
    if (pixelInBounds) {
      const CameraDirections cloudRay = cloudPixelRay(pixelCoord, outputDim,
                                                      cloudSubpixelOffset(pixelCoord, args.cloudRenderFrameIdx));
      tSurfaceKm = cloudSurfaceDistanceKm(pixelCoord, cloudRay, args);
      const vec3 viewDirYUp = worldToAtmosphereYUp(
        cloudRay.worldDirection, args.isZUp != 0u, args.flipUpAxis != 0);
      fresh = marchCloudPixel(pixelCoord, viewDirYUp, tSurfaceKm, args.cloudRenderFrameIdx, ctx, args);
      freshFlash = g_cloudFlashAccum;
      freshLayer2 = g_cloudLayer2Share;
    }
    gs_cloudFresh[groupThreadId.y][groupThreadId.x] = fresh.color;
    gs_cloudFreshDepth[groupThreadId.y][groupThreadId.x] = fresh.depth;
    gs_cloudFreshFlash[groupThreadId.y][groupThreadId.x] = freshFlash;
    gs_cloudFreshLayer2[groupThreadId.y][groupThreadId.x] = freshLayer2;
    GroupMemoryBarrierWithGroupSync();
    if (!pixelInBounds) {
      return;
    }
    CloudNeighbourhood nb = cloudNeighbourhoodEmpty();
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
      [unroll]
      for (int dx = -1; dx <= 1; ++dx) {
        const int2 g = int2(groupThreadId.xy) + int2(dx, dy);
        if (any(g < int2(0)) || g.x >= CLOUD_GROUP_X || g.y >= CLOUD_GROUP_Y) {
          continue;
        }
        const vec4 d = gs_cloudFreshDepth[g.y][g.x];
        const vec4 v = gs_cloudFresh[g.y][g.x];
        if (d.w < 0.0f || !cloudSameSurface(tSurfaceKm, v, d, args)) {
          continue;
        }
        cloudNeighbourhoodAdd(nb, v, d, gs_cloudFreshFlash[g.y][g.x], gs_cloudFreshLayer2[g.y][g.x],
                              vec2(dx, dy), 1.0f);
      }
    }
    const CloudResolved r = cloudResolvePixel(pixelCoord, outputDim, tSurfaceKm, nb, 1.0f, fresh.depth.w, ctx, args);
    cloudRenderOutput[pixelCoord] = r.color;
    cloudDepthOutput[pixelCoord] = r.depth;
    cloudStateOutput[pixelCoord] = r.state;
    return;
  }
#endif

  if (!cellInBounds) {
    return;
  }
  for (uint sub = 0u; sub < period; ++sub) {
    const uint2 subOffset = uint2(sub & 1u, sub >> 1u);
    const uint2 pixelCoord = cellOrigin + subOffset;
    if (any(pixelCoord >= outputDim)) {
      continue;
    }
    const vec2 uv = (vec2(pixelCoord) + 0.5f) / vec2(outputDim);
    const CameraDirections cloudRay = cameraScreenUVToDirection(cloudCamera, uv, true);
    const float tSurfaceKm = cloudSurfaceDistanceKm(pixelCoord, cloudRay, args);
    CloudPixelResult result;
    const vec3 viewDirYUp = worldToAtmosphereYUp(
      cloudRay.worldDirection, args.isZUp != 0u, args.flipUpAxis != 0);
    result = marchCloudPixel(pixelCoord, viewDirYUp, tSurfaceKm, args.cloudRenderFrameIdx, ctx, args);
    cloudRenderOutput[pixelCoord] = result.color;
    cloudDepthOutput[pixelCoord] = result.depth;
  }
}
