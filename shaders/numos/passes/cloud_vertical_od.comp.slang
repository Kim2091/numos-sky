// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Cloud vertical optical depth (lib/cloud_shadow_map.slangh, sampleCloudVerticalOpticalDepth).
//
// The cumulus body is one tile-periodic distance field whose texture y axis is the slab height
// fraction, i.e. the local vertical. Summing the body density up each texture column gives, for every
// voxel, the optical depth (km x body density) of the cloud below it, and the column's total less
// that is the cloud above it: the water the light from below and the sky light cross, read with two
// taps instead of two live marches. The four channels hold the tile's clouds shrunk by each of
// kCloudVerticalShrinkLevelsKm, for the hex blend zone's shrink (cloudUnionShrinkKm); the lookup
// interpolates between them.
//
// Output RGBA16F, same size and layout as the NVDF: the optical depth below the voxel's centre at
// each shrink level, and in the top voxel the column total. One thread per column.
// Re-run when the field, the live coverage offset or the slab change.

#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"
#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/cloud_nubis3_common.slangh"
#include "numos/lib/cloud_shadow_map.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1)
Texture3D<float> cloudNvdfSdf;

layout(binding = 2, rgba16f)
RWTexture3D<float4> cloudVerticalOutput;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  if (any(threadId.xy >= uint2(CLOUD_NVDF_SIZE_XZ))) {
    return;
  }
  AtmosphereArgs args = atmosphereArgs;
  const float offsetKm = (args.cloudCoverageMean - args.nvdfNominalCoverage) * args.nvdfCoverageOffsetKm;
  const float profileKm = max(args.nvdfProfileDepthKm, 1e-3f);
  const float dzKm = args.cloudThickness / float(CLOUD_NVDF_SIZE_Y);

  // Per voxel: everything below it plus half its own density, so values sit at voxel centres. The top
  // voxel holds the whole column, which the lookup reads as the total.
  vec4 belowKm = vec4(0.0f);
  for (uint y = 0u; y < CLOUD_NVDF_SIZE_Y; ++y) {
    const float sdfKm = cloudNvdfSdf.Load(int4(int(threadId.x), int(y), int(threadId.y), 0)).r - offsetKm;
    const float heightDensity = cloudCumulusHeightDensity((float(y) + 0.5f) / float(CLOUD_NVDF_SIZE_Y), args.cloudThickness);
    const vec4 density = saturate(-(sdfKm + kCloudVerticalShrinkLevelsKm) / profileKm) * (heightDensity * dzKm);
    const bool top = y + 1u == CLOUD_NVDF_SIZE_Y;
    cloudVerticalOutput[uint3(threadId.x, y, threadId.y)] = belowKm + (top ? 1.0f : 0.5f) * density;
    belowKm += density;
  }
}
