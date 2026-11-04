// Copyright 2026 Kim2091 (Sparkles)
// Copyright 2026 Veitaserum4
// SPDX-License-Identifier: Apache-2.0

// The cirrus's shadow: fills the cloud shadow map's last layer (cirrusShadowMapSunOpticalDepth in
// lib/cloud_shadow_map.slangh) with the optical depth the sun's beam crosses at each point of a window
// of the sheet. Hosts already read this texture for the clouds' shadow, so they need no new binding.
// Runs every frame the sheet casts a shadow: the wind moves it, and it is a single layer.

#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1, rg16f)
RWTexture3D<float2> cloudShadowMapOutput;

layout(binding = 2)
SamplerState cirrusMapSampler;

layout(binding = 3)
Texture2D<float2> cirrusMap;

#include "numos/lib/cloud_cirrus.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 texel : SV_DispatchThreadID) {
  if (any(texel >= uint2(kCloudShadowMapSize))) {
    return;
  }
  const AtmosphereArgs args = atmosphereArgs;
  const vec2 d = (vec2(texel) + 0.5f) / float(kCloudShadowMapSize) * 2.0f - 1.0f;
  const vec2 world = vec2(args.cloudCirrusShadowCenterX, args.cloudCirrusShadowCenterZ) + d * args.cloudCirrusShadowHalfKm;
  // The sheet's up here. The planet's centre lies under the camera (getCloudPlanetCenter).
  const vec2 outward = (world - args.cameraWorldPosYUpKm.xz) / (args.planetRadius + args.cloudCirrusAltitude);
  const vec3 up = vec3(outward.x, sqrt(max(1.0f - dot(outward, outward), 0.0f)), outward.y);
  const float sunCos = abs(dot(vec3(args.cloudCirrusShadowDirX, args.cloudCirrusShadowDirY, args.cloudCirrusShadowDirZ), up));
  // One texel of the window is one of the map's at mip 0 (Renderer::Impl::kCloudCirrusShadowHalfKm).
  const float column = cirrusColumnShare(world, 0.0f, args, cirrusMap, cirrusMapSampler);
  const float depth = kCirrusBeamDepthScale * args.cloudCirrusOpticalDepth * column / max(sunCos, kCirrusMinSlantCos);
  cloudShadowMapOutput[uint3(texel, kCloudShadowMapCirrusLayer)] = vec2(depth, 0.0f);
}
