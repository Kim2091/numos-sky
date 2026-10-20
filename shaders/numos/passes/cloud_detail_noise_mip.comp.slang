// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Cloud detail-noise mip chain.
//
// Fills mip N of the 128^3 detail volume from mip N-1 with a 2x2x2 box: one linear tap at the
// destination texel's centre lands exactly between eight source texels, and the repeat sampler wraps
// the periodic volume's edges. Dispatched once per level at init, after cloud_detail_noise_baker.
// The view march samples the level its step can integrate (cloudDetailMip in
// cloud_nubis3_common.slangh) instead of aliasing the finest content.

#include "numos/util/shader_types.h"

layout(binding = 0)
Texture3D<float4> cloudDetailNoiseSrc;

layout(binding = 1)
SamplerState cloudDetailNoiseBoxSampler;

layout(binding = 2, rgba8)
RWTexture3D<float4> cloudDetailNoiseDst;

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 threadId : SV_DispatchThreadID) {
  uint3 dstDim;
  cloudDetailNoiseDst.GetDimensions(dstDim.x, dstDim.y, dstDim.z);
  if (any(threadId >= dstDim)) {
    return;
  }
  const vec3 uvw = (vec3(threadId) + 0.5f) / vec3(dstDim);
  cloudDetailNoiseDst[threadId] = cloudDetailNoiseSrc.SampleLevel(cloudDetailNoiseBoxSampler, uvw, 0);
}
