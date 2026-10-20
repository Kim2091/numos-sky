// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "numos/util/shader_types.h"

layout(binding = 0)
Texture2D<float4> cloudDepth;
layout(binding = 1)
RWStructuredBuffer<uint4> statistics;

groupshared uint4 partial[64];

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
          uint groupIndex : SV_GroupIndex) {
  uint width, height;
  cloudDepth.GetDimensions(width, height);
  const bool valid = dispatchId.x < width && dispatchId.y < height;
  uint samples = 0u;
  if (valid) {
    samples = uint(max(cloudDepth.Load(int3(dispatchId.xy, 0)).w, 0.0f));
  }
  partial[groupIndex] = uint4(samples, samples, samples > 0u ? 1u : 0u, valid ? 1u : 0u);
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = 32u; stride > 0u; stride >>= 1u) {
    if (groupIndex < stride) {
      uint4 other = partial[groupIndex + stride];
      partial[groupIndex].x += other.x;
      partial[groupIndex].y = max(partial[groupIndex].y, other.y);
      partial[groupIndex].z += other.z;
      partial[groupIndex].w += other.w;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (groupIndex == 0u) {
    statistics[groupId.y * ((width + 7u) / 8u) + groupId.x] = partial[0];
  }
}
