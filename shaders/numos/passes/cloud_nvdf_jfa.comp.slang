// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

/*
 * Cloud NVDF jump-flooding pass. CloudNvdfJfaArgs.mode selects one of two modes.
 *
 * Mode 0, seed init: an occupied voxel with at least one empty 6-neighbour is a boundary voxel and
 * seeds itself; every other voxel starts with no seed. Seeding only the boundary, instead of running
 * separate inside and outside JFAs, halves the pass count; the resolve pass takes the sign from the
 * occupancy grid. XZ neighbours wrap (the field is tile-periodic). Y neighbours outside the slab
 * count as empty, so a body touching the slab floor or ceiling has its surface there, matching the
 * density samplers' hard base and top cut-offs.
 *
 * Mode 1, jump pass at jumpSizeVoxels: the standard 3^3 JFA step. Each voxel keeps the closest seed
 * among its own and those of its 26 neighbours at +-jumpSizeVoxels per axis. Distances are compared
 * in km (wrap- and anisotropy-aware, see cloudNvdfVoxelDistanceKm); voxel units would favour seeds
 * along the cheaper axis whenever the voxels are not cubic.
 *
 * Schedule (set in src/renderer.cpp): 128, 64, 32, 16, 8, 4, 2, 1, then one extra pass at 1
 * (JFA+1), which removes most of the rare far-seed misses plain JFA leaves. The two R32_UINT seed
 * buffers ping-pong; the CPU swaps the bindings each pass.
 *
 * Dispatch: 8x4x8 threads, (32, 16, 32) groups -> 256x64x256 voxels.
 */

#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/cloud_nvdf_common.slangh"

layout(binding = CLOUD_NVDF_JFA_BINDING_CONSTANTS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

// Occupancy (read in mode 0 only; bound in both modes for a stable layout).
layout(binding = CLOUD_NVDF_JFA_BINDING_OCCUPANCY_INPUT)
Texture3D<float> occupancySrv;

// Seed ping-pong (read in mode 1 only).
layout(binding = CLOUD_NVDF_JFA_BINDING_SEEDS_INPUT)
Texture3D<uint> seedsInput;

layout(binding = CLOUD_NVDF_JFA_BINDING_SEEDS_OUTPUT, r32ui)
RWTexture3D<uint> seedsOutput;

layout(push_constant)
ConstantBuffer<CloudNvdfJfaArgs> pushArgs;

bool isOccupied(int3 vox) {
  // Y out of range = empty (see header). XZ are pre-wrapped by callers.
  if (vox.y < 0 || vox.y >= int(CLOUD_NVDF_SIZE_Y)) {
    return false;
  }
  return occupancySrv.Load(int4(vox, 0)).r > 0.5f;
}

[shader("compute")]
[numthreads(8, 4, 8)]
void main(uint3 threadId : SV_DispatchThreadID) {
  if (any(threadId >= uint3(CLOUD_NVDF_SIZE_XZ, CLOUD_NVDF_SIZE_Y, CLOUD_NVDF_SIZE_XZ))) {
    return;
  }

  if (pushArgs.mode == 0u) {
    // --- Seed init -------------------------------------------------------
    uint seed = kCloudNvdfSeedInvalid;
    if (isOccupied(int3(threadId))) {
      // Face neighbours only: a voxel whose six face neighbours are occupied is interior even if
      // a diagonal is empty.
      const int3 offsets[6] = {
        int3( 1, 0, 0), int3(-1, 0, 0),
        int3( 0, 1, 0), int3( 0,-1, 0),
        int3( 0, 0, 1), int3( 0, 0,-1),
      };
      [unroll]
      for (int i = 0; i < 6; ++i) {
        int3 n = int3(threadId) + offsets[i];
        n.x = cloudNvdfWrapXZ(n.x);
        n.z = cloudNvdfWrapXZ(n.z);
        if (!isOccupied(n)) {
          seed = cloudNvdfPackSeed(threadId);
          break;
        }
      }
    }
    seedsOutput[threadId] = seed;
    return;
  }

  // --- Jump pass ---------------------------------------------------------
  AtmosphereArgs args = atmosphereArgs;
  const float tileKm      = args.cloudNoiseTileKm;
  const float thicknessKm = args.cloudThickness;
  const int   jump        = int(pushArgs.jumpSizeVoxels);

  uint  bestSeed = seedsInput.Load(int4(int3(threadId), 0)).r;
  float bestDist = (bestSeed == kCloudNvdfSeedInvalid)
                 ? 1e30f
                 : cloudNvdfVoxelDistanceKm(threadId, cloudNvdfUnpackSeed(bestSeed),
                                            tileKm, thicknessKm);

  for (int dz = -1; dz <= 1; ++dz) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0 && dz == 0) {
          continue;
        }
        int3 n = int3(threadId) + int3(dx, dy, dz) * jump;
        // Y clamps out (no vertical wrap); XZ wrap onto the tile torus.
        if (n.y < 0 || n.y >= int(CLOUD_NVDF_SIZE_Y)) {
          continue;
        }
        n.x = cloudNvdfWrapXZ(n.x);
        n.z = cloudNvdfWrapXZ(n.z);

        uint cand = seedsInput.Load(int4(n, 0)).r;
        if (cand == kCloudNvdfSeedInvalid) {
          continue;
        }
        float d = cloudNvdfVoxelDistanceKm(threadId, cloudNvdfUnpackSeed(cand),
                                           tileKm, thicknessKm);
        if (d < bestDist) {
          bestDist = d;
          bestSeed = cand;
        }
      }
    }
  }

  seedsOutput[threadId] = bestSeed;
}
