// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#ifndef CLOUD_NVDF_H
#define CLOUD_NVDF_H

#include "numos/util/shader_types.h"

NUMOS_SHARED_BEGIN

// Cloud NVDF (Nubis voxel data field): shared CPU/GPU constants for the cloud-body SDF bake chain
// (occupancy voxelize -> JFA seed init -> JFA jump passes -> signed resolve).
//
// The NVDF voxelizes the procedural cloud body (placement map plus column model) into a
// tile-periodic grid, and a wrap-aware jump-flooding chain turns it into a signed distance field.
// Detail noise stays out of the SDF and is applied as erosion at sample time, as in Nubis3
// (Schneider, SIGGRAPH 2023).
//
// Texture axes:
//   x (u) = world X, CLOUD_NVDF_SIZE_XZ texels, tile-wrapped
//   y (v) = vertical slab height fraction, CLOUD_NVDF_SIZE_Y texels, clamped
//   z (w) = world Z, CLOUD_NVDF_SIZE_XZ texels, tile-wrapped
// The horizontal domain is one cloudNoiseTileKm period, world-anchored, with wind applied at sample
// time. The vertical domain is [cloudAltitude, cloudAltitude + cloudThickness]. At the 12 km / 3 km
// defaults a voxel is ~47 m on every axis, which is plenty for a body model whose placement cells
// are 1 km or larger; erosion supplies the finer detail.
//
// Must match Renderer::Impl::kCloudNvdfSizeXZ / kCloudNvdfSizeY in src/renderer_impl.h.
#define CLOUD_NVDF_SIZE_XZ 256
#define CLOUD_NVDF_SIZE_Y  64

// Pass-local binding slots. Each NVDF pass has its own small descriptor set, outside the common
// atmosphere binding range. Only the cloud passes sample the NVDF; surface shadows read the cloud shadow map.
// Each block must match the bindings of the matching dispatch in src/renderer.cpp.
#define CLOUD_NVDF_OCCUPANCY_BINDING_CONSTANTS        0
#define CLOUD_NVDF_OCCUPANCY_BINDING_OUTPUT           1
#define CLOUD_NVDF_OCCUPANCY_BINDING_PLACEMENT_INPUT  2
#define CLOUD_NVDF_OCCUPANCY_BINDING_SAMPLER          3

#define CLOUD_NVDF_JFA_BINDING_CONSTANTS        0
#define CLOUD_NVDF_JFA_BINDING_OCCUPANCY_INPUT  1
#define CLOUD_NVDF_JFA_BINDING_SEEDS_INPUT      2
#define CLOUD_NVDF_JFA_BINDING_SEEDS_OUTPUT     3

#define CLOUD_NVDF_RESOLVE_BINDING_CONSTANTS        0
#define CLOUD_NVDF_RESOLVE_BINDING_OCCUPANCY_INPUT  1
#define CLOUD_NVDF_RESOLVE_BINDING_SEEDS_INPUT      2
#define CLOUD_NVDF_RESOLVE_BINDING_SDF_OUTPUT       3

// JFA push constants. mode 0 = seed init (reads occupancy, writes boundary-voxel seeds), mode 1 =
// jump pass at jumpSizeVoxels (reads seeds, writes refined seeds). They are push constants rather
// than AtmosphereArgs fields so the 9-pass chain does not rewrite the shared constant buffer
// mid-frame.
struct CloudNvdfJfaArgs {
  uint jumpSizeVoxels;
  uint mode;
  uint pad0;
  uint pad1;
};

NUMOS_SHARED_END

#endif  // CLOUD_NVDF_H
