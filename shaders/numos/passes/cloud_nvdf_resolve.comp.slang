// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

/*
 * Cloud NVDF signed resolve.
 *
 * Turns the final JFA seed buffer into the SDF: per voxel, the distance in km to the closest
 * boundary seed, negated inside the body. The sign comes from the occupancy grid, which is why a
 * single boundary-seeded JFA is enough. Near the surface the encoded body field takes over (see
 * cloudNvdfEncodeFieldKm).
 *
 * Precision:
 *  - Output is signed km in R16F. The R16F step is ~4 m at 6 km and ~6 cm at 0.1 km, always below
 *    the ~47 m voxel size, so no normalization is needed (Nubis3 stores [0, 1] only for BC1
 *    compression, which is not used here).
 *  - JFA distances are voxel centre to voxel centre. The true zero crossing lies between a boundary
 *    voxel and its empty neighbour, so that part of the field is biased outward by half a voxel
 *    (~23 m). The profile remap and the safety-scaled sphere-trace stepping absorb it.
 *  - Voxels with no seed at all (no cloud bodies at the baked nominal coverage) resolve to +100 km,
 *    which reads as empty air for both the profile early-out and the stepping.
 *
 * Writes the back SDF buffer; the renderer swaps front and back after this pass so readers never
 * see a half-baked field.
 *
 * Dispatch: 8x4x8 threads, (32, 16, 32) groups -> 256x64x256 voxels.
 */

#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/cloud_nvdf_common.slangh"

layout(binding = CLOUD_NVDF_RESOLVE_BINDING_CONSTANTS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = CLOUD_NVDF_RESOLVE_BINDING_OCCUPANCY_INPUT)
Texture3D<float> occupancySrv;

layout(binding = CLOUD_NVDF_RESOLVE_BINDING_SEEDS_INPUT)
Texture3D<uint> seedsInput;

layout(binding = CLOUD_NVDF_RESOLVE_BINDING_SDF_OUTPUT, r16f)
RWTexture3D<float> sdfOutput;

// Distance for "no cloud anywhere": far above any real in-slab distance (the wrapped tile caps XZ
// distances at tileKm/sqrt(2), ~8.5 km) and far below the R16F maximum of 65504, so later math
// cannot overflow.
static const float kCloudNvdfEmptyKm = 100.0f;

[shader("compute")]
[numthreads(8, 4, 8)]
void main(uint3 threadId : SV_DispatchThreadID) {
  if (any(threadId >= uint3(CLOUD_NVDF_SIZE_XZ, CLOUD_NVDF_SIZE_Y, CLOUD_NVDF_SIZE_XZ))) {
    return;
  }

  AtmosphereArgs args = atmosphereArgs;

  uint seed = seedsInput.Load(int4(int3(threadId), 0)).r;

  float sdfKm;
  if (seed == kCloudNvdfSeedInvalid) {
    sdfKm = kCloudNvdfEmptyKm;
  } else {
    float d = cloudNvdfVoxelDistanceKm(threadId, cloudNvdfUnpackSeed(seed),
                                       args.cloudNoiseTileKm, args.cloudThickness);
    const float occupancy = occupancySrv.Load(int4(int3(threadId), 0)).r;
    bool inside = occupancy > 0.5f;
    sdfKm = inside ? -d : d;
    // Near the surface, the body field's own continuous distance (see cloudNvdfEncodeFieldKm);
    // the JFA distance takes over toward the edge of the encoded band.
    const float fieldSdfKm = -cloudNvdfDecodeFieldKm(occupancy);
    const float w = smoothstep(0.5f * kCloudNvdfFieldBandKm, 0.9f * kCloudNvdfFieldBandKm, d);
    sdfKm = lerp(fieldSdfKm, sdfKm, w);
  }

  sdfOutput[threadId] = sdfKm;
}
