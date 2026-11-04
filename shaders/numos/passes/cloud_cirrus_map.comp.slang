// Copyright 2026 Kim2091 (Sparkles)
// Copyright 2026 Veitaserum4
// SPDX-License-Identifier: Apache-2.0

// Cirrus map bake (lib/cloud_cirrus.slangh). Fills mip 0 of the tiling RG16F map:
//
//   R: filaments in [0, 1]. Ridged noise stretched along the map's x axis, its domain bent by a slow
//      warp so the streaks curve and comb together as wind-drawn ice trails do.
//   G: patches in [0, 1]. Smooth, wide noise; the coverage threshold opens the sky between them at
//      run time, so weather changes never re-bake.
//
// Every lattice wraps at an integer period, so the map tiles exactly. Baked when the layer is first
// used and when cloudCirrusNoiseSeed changes.

#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1, rg16f)
RWTexture2D<float2> cirrusMapOutput;

float cirrusHash(int2 cell, int2 period, uint seed) {
  const int2 wrapped = ((cell % period) + period) % period;
  uint h = uint(wrapped.x) * 73856093u ^ uint(wrapped.y) * 19349663u ^ seed * 83492791u;
  h = (h ^ (h >> 16u)) * 0x45d9f3bu;
  h = (h ^ (h >> 16u)) * 0x45d9f3bu;
  return float(h ^ (h >> 16u)) / 4294967295.0f;
}

// Gradient noise over `period` cells across the unit square, in [0, 1]; tiles exactly. Value noise
// would change every row at the same lattice lines, and the map's sharpening would draw those lines
// as straight edges across the streaks.
float cirrusGradient(int2 cell, vec2 offset, int2 period, uint seed) {
  const float angle = cirrusHash(cell, period, seed) * 6.2831853f;
  return dot(vec2(cos(angle), sin(angle)), offset);
}

float cirrusNoise(vec2 uv, int2 period, uint seed) {
  const vec2 p = uv * vec2(period);
  const vec2 base = floor(p);
  const int2 cell = int2(base);
  const vec2 r = p - base;
  const vec2 f = r * r * r * (r * (r * 6.0f - 15.0f) + 10.0f);
  const float bottom = lerp(cirrusGradient(cell, r, period, seed),
                            cirrusGradient(cell + int2(1, 0), r - vec2(1.0f, 0.0f), period, seed), f.x);
  const float top = lerp(cirrusGradient(cell + int2(0, 1), r - vec2(0.0f, 1.0f), period, seed),
                         cirrusGradient(cell + int2(1, 1), r - vec2(1.0f, 1.0f), period, seed), f.x);
  return saturate(0.5f + 0.7071f * lerp(bottom, top, f.y));
}

float cirrusFbm(vec2 uv, int2 period, uint octaves, uint seed) {
  float sum = 0.0f, weight = 0.5f, total = 0.0f;
  for (uint i = 0u; i < octaves; ++i) {
    sum += weight * cirrusNoise(uv, period, seed + i);
    total += weight;
    weight *= 0.5f;
    period *= 2;
  }
  return sum / total;
}

// Thin bright ridges where the noise crosses its mean, stretched across x by the period's aspect.
float cirrusFilaments(vec2 uv, int2 period, uint octaves, uint seed) {
  float sum = 0.0f, weight = 0.5f, total = 0.0f;
  for (uint i = 0u; i < octaves; ++i) {
    const float ridge = 1.0f - abs(2.0f * cirrusNoise(uv, period, seed + i) - 1.0f);
    const float ridge2 = ridge * ridge;
    sum += weight * ridge2 * ridge2 * ridge;
    total += weight;
    weight *= 0.55f;
    period *= 2;
  }
  return sum / total;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  uint2 size;
  cirrusMapOutput.GetDimensions(size.x, size.y);
  if (any(pixel >= size)) {
    return;
  }
  const uint seed = uint(atmosphereArgs.cloudCirrusNoiseSeed);
  const vec2 uv = (vec2(pixel) + 0.5f) / vec2(size);
  // The warp bends the streaks mostly across their length, so they curve rather than break.
  const vec2 warp = vec2(cirrusFbm(uv, int2(4, 4), 3u, seed + 100u), cirrusFbm(uv, int2(5, 5), 3u, seed + 200u)) - 0.5f;
  const vec2 warped = uv + warp * vec2(0.05f, 0.11f);
  const float filaments = cirrusFilaments(warped, int2(5, 44), 5u, seed + 300u);
  // Wide bands along the streaks vary their strength, so the trails fade in and out.
  const float strength = cirrusFbm(warped, int2(4, 14), 3u, seed + 400u);
  const float patches = cirrusFbm(uv + warp * 0.08f, int2(5, 7), 4u, seed + 500u);
  cirrusMapOutput[pixel] = vec2(saturate(filaments * (0.55f + 0.9f * strength)), saturate((patches - 0.2f) / 0.6f));
}
