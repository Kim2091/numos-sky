// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

/*
 * Cloud NVDF occupancy voxelize.
 *
 * Rasterizes the procedural cloud body (placement map plus per-column model) into a tile-periodic
 * 256x64x256 R8 grid. The JFA distance transform runs over this grid, and the resulting SDF drives
 * the Nubis3 dimensional profile (the outside-to-inside density gradient) and the sphere-traced
 * march stepping. The texel holds the encoded body field rather than a plain 0/1 (see
 * cloudNvdfEncodeFieldKm).
 *
 * What the bake leaves out:
 *  - Hex de-tiling. The NVDF is baked plainly tile-periodic and the sampler applies the hex offsets,
 *    as for the placement map. The hex lattice is world-space and breaks the tile period, so a
 *    de-tiled field could not be baked anyway.
 *  - Detail noise. As in Nubis3, the SDF describes the cloud body; detail noise is erosion applied
 *    at sample time and must not move the baked iso-surface.
 *  - Live coverage. The bake uses a nominal coverage (nvdfNominalCoverage) and the sampler turns the
 *    difference to the live coverage into a level-set offset, so clouds grow and shrink between
 *    bakes. In auto mode (option 0) the nominal follows the live coverage and a change triggers an
 *    amortized re-bake published front/back; a nonzero option pins it.
 *
 * Dispatch: 8x4x8 threads, (32, 16, 32) groups -> 256x64x256 voxels.
 * Axes: see cloud_nvdf.h (u = world X, v = vertical, w = world Z).
 */

#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/cloud_nvdf_common.slangh"

layout(binding = CLOUD_NVDF_OCCUPANCY_BINDING_CONSTANTS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = CLOUD_NVDF_OCCUPANCY_BINDING_OUTPUT, r8)
RWTexture3D<float> occupancyOutput;

// The same placement map the density samplers read. The body model here must use the same inputs as
// the samplers so the SDF describes the bodies they shade.
layout(binding = CLOUD_NVDF_OCCUPANCY_BINDING_PLACEMENT_INPUT)
Texture2D<float4> cloudPlacementMapSrv;

layout(binding = CLOUD_NVDF_OCCUPANCY_BINDING_SAMPLER)
SamplerState placementSamplerState;

[shader("compute")]
[numthreads(8, 4, 8)]
void main(uint3 threadId : SV_DispatchThreadID) {
  if (any(threadId >= uint3(CLOUD_NVDF_SIZE_XZ, CLOUD_NVDF_SIZE_Y, CLOUD_NVDF_SIZE_XZ))) {
    return;
  }

  AtmosphereArgs args = atmosphereArgs;

  // One linear placement tap at the voxel-centre tile UV, without hex offsets. The map tiles at
  // cloudNoiseTileKm, the same period as this grid's horizontal domain.
  vec2 tileUV = cloudNvdfVoxelToTileUV(threadId);
  vec4 placement = cloudPlacementMapSrv.SampleLevel(placementSamplerState, tileUV, 0);
  float hf = cloudNvdfVoxelToHeightFraction(threadId);

  occupancyOutput[threadId] = cloudNvdfEncodeFieldKm(
      cloudCumulusBodyFieldKm(tileUV, hf, placement, saturate(args.nvdfNominalCoverage), args));
}
