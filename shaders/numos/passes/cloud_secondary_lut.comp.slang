// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Secondary-ray cloud LUT.
//
// Bakes the full cloud view march into a 256x256 RGBA16F full-sphere dome keyed by (azimuth, signed
// elevation = l*l*sign(l) * (pi/2), l = uv.y*2 - 1; see cloudDomeUvToDir in
// atmosphere_common.slangh). rgb = premultiplied cloud radiance, a = view transmittance, the same
// convention as the screen-space cloud render target.
//
// Indirect, PSR and reflection rays that miss to the sky would otherwise each run a full cloud
// march, a per-ray cost comparable to the visible cloud pass. They only need what the clouds look
// like in a given direction: the parallax from a scene-scale origin offset (~100 m) against clouds
// kilometres away is far below a texel. One 64k-texel bake per frame replaces millions of per-ray
// marches.
//
// The bake runs marchCloudLayers, the same march as the screen pass, so reflections and indirect
// bounces see the clouds the primary rays see. With rtx.atmosphere.cloudSecondaryLutEnable = 0,
// secondary sky-miss rays see no clouds.
//
// The march is coarse and averaged over frames: steps are kCloudDomeStepScale times longer than the
// screen pass's, the jitter changes every frame, and each update blends into the texel's previous
// value, so the coarse steps' error averages out instead of banding. At 1080p the 4x steps save
// 0.05-0.15 ms with no visible change (mean |diff| <= 1.2/255). Blending lags a texel by about
// 1/kCloudDomeBlend updates, ~16 frames with the default row interleave of 4, which is slow next to
// the clouds' drift. A camera cut, a first bake (cloudDomeInterleave bit 16) or a lightning flash
// writes the texel fresh.

#define ATMOSPHERE_AVAILABLE

#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"

// The binding layout matches cloud_render.comp.slang (slot 6 is this pass's own output). The global
// names are the ones atmosphere_common.slangh's helpers expect, so they are declared before the
// include.
layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

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
#define CLOUD_SHADOW_MAP_AVAILABLE
// Layer 2 patch-field map (cloud_layer2_map.comp), for layer 2's light paths.
layout(binding = 23)
Texture3D<float2> AtmosphereCloudLayer2Map;
#define CLOUD_LAYER2_MAP_AVAILABLE

layout(binding = 5)
Texture2DArray<float> BlueNoise;

layout(binding = 6, rgba16f)
RWTexture2D<float4> cloudSecondaryLutOutput;

layout(binding = 7)
Texture2D<float4> AtmosphereSkyViewLut;


layout(binding = 9)
SamplerState AtmosphereSkyViewSampler;

// NVDF SDF and detail volume, at the same slots as cloud_render.comp.slang so the dome march sees the
// same field.
layout(binding = 13)
Texture3D<float> AtmosphereCloudNvdfSdf;

layout(binding = 14)
Texture3D<float4> AtmosphereCloudDetailNoise3D;

#include "numos/lib/atmosphere_common.slangh"
// Per-sample sun transmittance at the cloud's own altitude.
layout(binding = 10)
Texture2D<float4> AtmosphereTransmittanceLut;
#define ATMOSPHERE_TRANSMITTANCE_LUT_AVAILABLE


#include "numos/lib/cloud_march_common.slangh"

[shader("compute")]
[numthreads(4, 4, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
  AtmosphereArgs args = atmosphereArgs;

  // Row interleave. With period P the dispatch covers 1/P of the rows and this thread marches row
  // P * y + phase; the other rows keep the previous frames' values. Whole rows are skipped rather
  // than a checkerboard because a per-lane skip would idle lanes while the warp still ran its
  // longest march. The renderer forces P = 1 on any frame where the cloud inputs change or the
  // camera cuts.
  const uint period = max(args.cloudDomeInterleave & 0xFFu, 1u);
  const uint phase = (args.cloudDomeInterleave >> 8u) & 0xFFu;
  uint2 pixelCoord = uint2(dispatchId.x, dispatchId.y * period + phase);
  uint2 lutDim;
  cloudSecondaryLutOutput.GetDimensions(lutDim.x, lutDim.y);
  const bool inBounds = all(pixelCoord < lutDim);

  // Clear-sky early-out under the same conditions as cloud_render, so both write "no cloud, full
  // transmittance" together. Coverage on either layer keeps the march on.
  float effectiveCoverageMean = args.cloudCoverageMean;
  if (args.cloudLayer2Enable != 0u) {
    effectiveCoverageMean = max(effectiveCoverageMean, args.cloudLayer2CoverageMean);
  }
  if (args.cloudEnabled < 0.5f || effectiveCoverageMean < 0.01f || args.cloudViewSamples == 0u) {
    if (inBounds) {
      cloudSecondaryLutOutput[pixelCoord] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return;
  }

  // The sky ring is sampled once per group (buildCloudShadeContextGroup), so every lane, in bounds
  // or not, gets here before any returns.
  CloudShadeContext ctx = buildCloudShadeContextGroup(args, groupIndex);
  if (!inBounds) {
    return;
  }

  // Dome direction for this texel. The mapping covers the full sphere; v = 0.5 is the horizon.
  vec2 uv = (vec2(pixelCoord) + 0.5f) / vec2(lutDim);
  vec3 viewDirYUp = cloudDomeUvToDir(uv);

  // Coarse steps (see the header comment).
  const float kCloudDomeStepScale = 4.0f;
  const float kCloudDomeBlend = 0.25f;
  args.cloudViewStepKm *= kCloudDomeStepScale;
  args.nubis3AdaptiveStepKm *= kCloudDomeStepScale;
  args.cloudViewSamplesMax = max(args.cloudViewSamplesMax / kCloudDomeStepScale, 2.0f);
  args.cloudViewSamples = max(uint(float(args.cloudViewSamples) / kCloudDomeStepScale), 2u);
  // A lightning flash lasts a few frames, which blending would dim; it writes fresh too.
  const bool domeReset = (args.cloudDomeInterleave & 0x10000u) != 0u
                      || args.lightningEnvelope > 0.0f || args.lightningEnvelopePrevious > 0.0f;
  // A fresh jitter every frame, for the view march and its light samples.
  const float jitter = fastJitter(pixelCoord, args.cloudRenderFrameIdx).x;
  g_cloudLightJitterSeed = fastJitter(pixelCoord + uint2(61u, 23u), args.cloudRenderFrameIdx + 29u).x;

  vec3 accumColor = vec3(0.0f);
  float viewTransmittance = 1.0f;
  // The depth outputs are unused here; only the screen pass writes a depth companion target.
  // marchCloudLayers includes layer 2 as in the screen pass, so reflections and indirect light see
  // the same layers.
  float unusedEntryDistanceKm, unusedMeanCloudDepthKm;
  // The dome is averaged over frames anyway, so the adaptive march's near (animated) and far
  // (static) hashes are the same value here.
  marchCloudLayers(viewDirYUp, jitter, jitter,
                   /*tMinClamp=*/0.0f,
                   /*tMaxClamp=*/0.0f,
                   args.cloudViewSamples, ctx, args,
                   accumColor, viewTransmittance,
                   unusedEntryDistanceKm, unusedMeanCloudDepthKm);

  vec4 domeValue = vec4(accumColor, viewTransmittance);
  if (!domeReset) {
    domeValue = lerp(cloudSecondaryLutOutput[pixelCoord], domeValue, kCloudDomeBlend);
  }
  cloudSecondaryLutOutput[pixelCoord] = domeValue;
}
