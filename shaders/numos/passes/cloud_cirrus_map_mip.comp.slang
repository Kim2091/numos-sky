// Copyright 2026 Veitaserum4
// SPDX-License-Identifier: Apache-2.0

// Cirrus map mip chain. Fills mip N from mip N-1 with a 2x2 box: one linear tap at the destination
// texel's centre lands between four source texels, and the repeat sampler wraps the tiling edges
// (generate_mipmap clamps, which would show the tile's seam at a distance).

#include "numos/util/shader_types.h"

layout(binding = 0)
Texture2D<float2> cirrusMapSrc;

layout(binding = 1)
SamplerState cirrusMapBoxSampler;

layout(binding = 2, rg16f)
RWTexture2D<float2> cirrusMapDst;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  uint2 size;
  cirrusMapDst.GetDimensions(size.x, size.y);
  if (any(pixel >= size)) {
    return;
  }
  cirrusMapDst[pixel] = cirrusMapSrc.SampleLevel(cirrusMapBoxSampler, (vec2(pixel) + 0.5f) / vec2(size), 0);
}
