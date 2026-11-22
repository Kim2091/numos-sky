// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

//!variant aerial_perspective_tile_depth.comp
//!variant aerial_perspective_tile_depth_dilate.comp
//!>       AP_TILE_DEPTH_DILATE
//!end-variants

// Aerial perspective near field, tile depth (docs/aerial-perspective-v2.md, section 4.2.2).
//
// One thread per froxel column. Writes the deepest visible surface over the column's screen tile
// (apTileSurfaceForward: kApNoSurface if any of its pixels sees the sky, or without depth), so the
// integration, its sky filter, the lighting pass and the composite read one texel per column instead
// of the tile's pixels.
//
// With AP_TILE_DEPTH_DILATE it instead writes the deepest of those values over the square of columns
// within ApSurfaceArgs.params.z of each column: a froxel past it is hidden from every pixel whose
// filtered reads can reach it, so the lighting pass may skip it.

#include "numos/lib/aerial_perspective.slangh"
#include "numos/lib/aerial_perspective_binding_indices.h"

layout(binding = AP_TILE_DEPTH_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = AP_TILE_DEPTH_SURFACE_ARGS)
ConstantBuffer<ApSurfaceArgs> surfaceArgs;

#if defined(AP_TILE_DEPTH_DILATE)
layout(binding = AP_TILE_DEPTH_INPUT)
Texture2D<float> tileDepthInput;
#else
layout(binding = AP_TILE_DEPTH_LINEAR_VIEW_Z)
Texture2D<float> sceneLinearViewZ;
#endif

layout(binding = AP_TILE_DEPTH_OUTPUT, r32f)
RWTexture2D<float> tileDepthOutput;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 column : SV_DispatchThreadID) {
  const AtmosphereArgs args = atmosphereArgs;
  if (any(column >= uint2(args.apFroxelSizeX, args.apFroxelSizeY))) {
    return;
  }
#if defined(AP_TILE_DEPTH_DILATE)
  const int radius = int(min(surfaceArgs.params.z, 3.0f));
  const int2 grid = int2(args.apFroxelSizeX, args.apFroxelSizeY);
  float deepest = 0.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int2 neighbor = clamp(int2(column) + int2(dx, dy), int2(0), grid - 1);
      deepest = max(deepest, tileDepthInput.Load(int3(neighbor, 0)));
    }
  }
  tileDepthOutput[column] = deepest;
#else
  tileDepthOutput[column] = apTileSurfaceForward(sceneLinearViewZ, surfaceArgs, column, args);
#endif
}
