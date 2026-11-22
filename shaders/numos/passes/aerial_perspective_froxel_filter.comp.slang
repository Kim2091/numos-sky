// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Aerial perspective near field, filter (docs/aerial-perspective-v2.md, section 4.2).
//
// One thread per froxel. Copies this frame's aux volume into the spare one with the local-light
// visibility (B) averaged over the neighboring froxels of the same slice, under a tent of
// rtx.atmosphere.aerialPerspectiveLocalLightFilterRadius froxels. The per-pixel lamp glow reads its
// shadowing from that copy. Each froxel's visibility is a running average of one 0/1 shadow ray a
// frame, so in a dark scene lit by lamps it stays grainy at froxel scale; the glow itself is smooth
// and analytic, so it only shows that grain. The lighting pass keeps the unfiltered volume as its
// history, so the filter never feeds back into itself. The other channels are copied unchanged.

#include "numos/lib/aerial_perspective_binding_indices.h"

struct ApFilterArgs {
  uint radius;
  uint gridX;
  uint gridY;
  uint gridZ;
};

layout(binding = AP_FILTER_ARGS)
ConstantBuffer<ApFilterArgs> filterArgs;

layout(binding = AP_FILTER_INPUT)
Texture3D<float4> auxInput;

layout(binding = AP_FILTER_OUTPUT, rgba16f)
RWTexture3D<float4> auxOutput;

static const int kApLocalFilterMaxRadius = 2;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 froxel : SV_DispatchThreadID) {
  const ApFilterArgs a = filterArgs;
  if (any(froxel >= uint3(a.gridX, a.gridY, a.gridZ))) {
    return;
  }
  float4 center = auxInput.Load(int4(froxel, 0));
  const int radius = int(min(a.radius, uint(kApLocalFilterMaxRadius)));
  float sum = 0.0f;
  float weightSum = 0.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int2 neighbor = int2(froxel.xy) + int2(dx, dy);
      if (any(neighbor < 0) || neighbor.x >= int(a.gridX) || neighbor.y >= int(a.gridY)) {
        continue;
      }
      const float w = float((radius + 1 - abs(dx)) * (radius + 1 - abs(dy)));
      sum += auxInput.Load(int4(neighbor, froxel.z, 0)).b * w;
      weightSum += w;
    }
  }
  center.b = weightSum > 0.0f ? sum / weightSum : center.b;
  auxOutput[froxel] = center;
}
