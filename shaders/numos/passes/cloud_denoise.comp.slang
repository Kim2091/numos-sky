// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Cloud spatial denoise. An edge-aware 5x5 filter over the screen cloud pass's output, after its
// temporal filter, into a separate texture the host composites. The temporal history keeps the
// unfiltered result, so the blur never compounds from frame to frame.
//
// The temporal filter leaves a residue of the march's jitter noise wherever the history is short
// (motion, in-cloud views, the quarter march's interpolated pixels). Cloud noise is a small relative
// wobble in brightness on a smooth field, while what must stay sharp is where the brightness or the
// opacity steps: billow rims, silhouettes, the sky/surface boundary. So each neighbour's weight falls
// with its relative brightness difference (scale: cloudSpatialDenoise), its opacity difference and
// any change of the surface behind, and a step larger than the noise keeps its edge. The brightness
// scale is further multiplied by how much of its history the pixel lost (the fraction of depth.w, see
// cloudDepthWithShortHistory): a pixel with its full history is left alone, its detail intact.
//
// Output RGBA16F, the cloud render's format (premultiplied radiance, view-ray transmittance).

#include "numos/util/shader_types.h"

// x: relative brightness scale (cloudSpatialDenoise, > 0), y: opacity scale, z: surface distance
// tolerance (relative), w: unused.
struct CloudDenoiseArgs {
  vec4 params;
};

layout(binding = 0)
ConstantBuffer<CloudDenoiseArgs> denoiseArgs;

layout(binding = 1)
Texture2D<float4> cloudRenderInput;

layout(binding = 2)
Texture2D<float4> cloudDepthInput;

layout(binding = 3, rgba16f)
RWTexture2D<float4> cloudDenoiseOutput;

static const float kCloudDenoiseNoSurfaceKm = 1e8f;  // depth.z at or beyond this = sky
static const float kCloudDenoiseSpatialSigmaPx = 1.5f;

// The edge weights compare a 3x3-smoothed guide, not the pixels themselves: the march's noise is
// right-skewed (rare bright light-tap samples), and weights on raw brightness reject exactly those,
// pulling the result toward the median - darker. Tile: 8x8 outputs, a 2 px filter apron, and a 1 px
// guide apron beyond it.
#define DN_GROUP 8
#define DN_TILE (DN_GROUP + 6)
groupshared vec4 gs_color[DN_TILE][DN_TILE];
groupshared vec2 gs_depth[DN_TILE][DN_TILE];  // surface km (or sky), x < 0: outside the image
groupshared vec2 gs_guide[DN_TILE - 2][DN_TILE - 2];  // smoothed luminance, opacity
groupshared float gs_short[DN_TILE][DN_TILE];  // share of the temporal history the pixel lost
groupshared uint gs_anyShort;  // any pixel of the group or its 1 px apron lost history

float denoiseLuminance(vec3 rgb) {
  return dot(rgb, vec3(0.2126f, 0.7152f, 0.0722f));
}

bool denoiseSameSurface(vec2 a, vec2 b, float tolerance) {
  const bool skyA = a.y >= kCloudDenoiseNoSurfaceKm;
  const bool skyB = b.y >= kCloudDenoiseNoSurfaceKm;
  return skyA == skyB && (skyA || abs(a.y - b.y) <= tolerance * max(max(a.y, b.y), 1e-3f));
}

[shader("compute")]
[numthreads(DN_GROUP, DN_GROUP, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThread : SV_GroupThreadID) {
  uint2 dim;
  cloudRenderInput.GetDimensions(dim.x, dim.y);
  const int2 tileOrigin = int2(groupId.xy) * DN_GROUP - 3;
  const uint lane = groupThread.y * DN_GROUP + groupThread.x;
  const int2 p = int2(groupId.xy) * DN_GROUP + int2(groupThread.xy);

  // Group-level early-out. A pixel is filtered only if some pixel of its 3x3 neighbourhood lost
  // history, so a group where no pixel of the group or its 1 px apron did copies its input, exactly
  // what the per-pixel test below would write, without loading the 14x14 tile or building the guide.
  // In a still or slowly moving view that is nearly every group.
  if (lane == 0u) {
    gs_anyShort = 0u;
  }
  GroupMemoryBarrierWithGroupSync();
  for (uint i = lane; i < (DN_GROUP + 2) * (DN_GROUP + 2); i += DN_GROUP * DN_GROUP) {
    const int2 q = int2(groupId.xy) * DN_GROUP - 1 + int2(i % (DN_GROUP + 2), i / (DN_GROUP + 2));
    if (all(q >= int2(0)) && all(q < int2(dim)) && frac(max(cloudDepthInput.Load(int3(q, 0)).w, 0.0f)) > 0.0f) {
      gs_anyShort = 1u;
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (gs_anyShort == 0u) {
    if (all(p < int2(dim))) {
      cloudDenoiseOutput[uint2(p)] = cloudRenderInput.Load(int3(p, 0));
    }
    return;
  }

  for (uint i = lane; i < DN_TILE * DN_TILE; i += DN_GROUP * DN_GROUP) {
    const int2 t = int2(i % DN_TILE, i / DN_TILE);
    const int2 q = tileOrigin + t;
    const bool inside = all(q >= int2(0)) && all(q < int2(dim));
    gs_color[t.y][t.x] = inside ? cloudRenderInput.Load(int3(q, 0)) : vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const vec4 depth = inside ? cloudDepthInput.Load(int3(q, 0)) : vec4(0.0f);
    gs_depth[t.y][t.x] = inside ? vec2(1.0f, depth.z) : vec2(-1.0f, 0.0f);
    gs_short[t.y][t.x] = inside ? frac(max(depth.w, 0.0f)) / 0.99f : 0.0f;
  }
  GroupMemoryBarrierWithGroupSync();
  const float surfaceTolerance = denoiseArgs.params.z;
  for (uint i = lane; i < (DN_TILE - 2) * (DN_TILE - 2); i += DN_GROUP * DN_GROUP) {
    const int2 t = int2(i % (DN_TILE - 2), i / (DN_TILE - 2)) + 1;
    const vec2 dc = gs_depth[t.y][t.x];
    vec2 sum = vec2(0.0f);
    float n = 0.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
      [unroll]
      for (int dx = -1; dx <= 1; ++dx) {
        const vec2 dn = gs_depth[t.y + dy][t.x + dx];
        if (dn.x < 0.0f || !denoiseSameSurface(dc, dn, surfaceTolerance)) {
          continue;
        }
        const vec4 c = gs_color[t.y + dy][t.x + dx];
        sum += vec2(denoiseLuminance(c.rgb), c.a);
        n += 1.0f;
      }
    }
    gs_guide[t.y - 1][t.x - 1] = n > 0.0f ? sum / n : vec2(0.0f, 1.0f);
  }
  GroupMemoryBarrierWithGroupSync();

  if (any(p >= int2(dim))) {
    return;
  }
  const int2 tc = int2(groupThread.xy) + 3;
  const vec4 c = gs_color[tc.y][tc.x];
  const vec2 dc = gs_depth[tc.y][tc.x];
  const vec2 gc = gs_guide[tc.y - 1][tc.x - 1];
  // The neighbourhood's shortest-history pixel decides: the quarter march's history loss is patchy.
  float shortHistory = 0.0f;
  [unroll]
  for (int dy = -1; dy <= 1; ++dy) {
    [unroll]
    for (int dx = -1; dx <= 1; ++dx) {
      shortHistory = max(shortHistory, gs_short[tc.y + dy][tc.x + dx]);
    }
  }
  if (shortHistory <= 0.0f) {
    cloudDenoiseOutput[uint2(p)] = c;
    return;
  }
  const float brightnessScale = max(denoiseArgs.params.x * shortHistory, 1e-4f);
  const float opacityScale = max(denoiseArgs.params.y, 1e-4f);

  vec4 sum = vec4(0.0f);
  float weightSum = 0.0f;
  [unroll]
  for (int dy = -2; dy <= 2; ++dy) {
    [unroll]
    for (int dx = -2; dx <= 2; ++dx) {
      const int2 t = tc + int2(dx, dy);
      const vec2 dn = gs_depth[t.y][t.x];
      if (dn.x < 0.0f || !denoiseSameSurface(dc, dn, surfaceTolerance)) {
        continue;  // outside the image, or another surface behind
      }
      const vec2 gn = gs_guide[t.y - 1][t.x - 1];
      const float relative = abs(gn.x - gc.x) / max(max(gn.x, gc.x), 1e-6f);
      const float w = exp(-float(dx * dx + dy * dy) * (0.5f / (kCloudDenoiseSpatialSigmaPx * kCloudDenoiseSpatialSigmaPx)))
                    * exp(-relative / brightnessScale)
                    * exp(-abs(gn.y - gc.y) / opacityScale);
      sum += gs_color[t.y][t.x] * w;
      weightSum += w;
    }
  }
  cloudDenoiseOutput[uint2(p)] = weightSum > 0.0f ? sum / weightSum : c;
}
