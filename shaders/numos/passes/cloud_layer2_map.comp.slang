// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Layer 2 patch-field map (lib/cloud_nubis3_common.slangh, kCloudLayer2Map*).
//
// Bakes cloudStratiformPatchFields (the warped Worley F1 of the patch edges and the billow channel of
// the top) on two nested grids centred at (cloudLayer2MapCenterX, cloudLayer2MapCenterZ) in the
// cloud frame. Layer 2's light paths (cloudStratiformFieldsForLight, cloud_march_common.slangh) read
// this map instead of evaluating the noise per tap, which would cost ~44 taps per lit sample and
// dominate a fog or scud view. The view march evaluates the fields live.
//
// Output RG16F, kCloudLayer2MapSize^2 x 2: depth slice = cascade. One thread per texel.

#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"
#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/cloud_nubis3_common.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1)
Texture3D<float4> cloudDetailNoise;

layout(binding = 2)
SamplerState cloudSampler;

layout(binding = 3, rg16f)
RWTexture3D<float2> cloudLayer2MapOutput;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  if (any(threadId.xy >= uint2(kCloudLayer2MapSize)) || threadId.z >= 2u) {
    return;
  }
  const AtmosphereArgs args = atmosphereArgs;
  const uint cascade = threadId.z;
  const float texelKm = cloudLayer2MapTexelKm(cascade);
  const vec2 xz = vec2(args.cloudLayer2MapCenterX, args.cloudLayer2MapCenterZ)
                + (vec2(threadId.xy) + 0.5f - 0.5f * float(kCloudLayer2MapSize)) * texelKm;
  // Prefilter each noise channel to the texel: curl texels are 6 km / 128 = 47 m, billow 1.6 km / 128
  // = 12.5 m.
  const float curlMip = cascade == 0u ? 0.0f : 2.0f;
  const float billowMip = cascade == 0u ? 1.0f : 4.0f;
  cloudLayer2MapOutput[threadId] = cloudStratiformPatchFields(xz, args.cloudLayer2NoiseSeed, curlMip, billowMip,
                                                             cloudDetailNoise, cloudSampler);
}
