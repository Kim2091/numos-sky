// Copyright 2026 Kim2091 (Sparkles)
// Copyright 2026 Veitaserum4
// SPDX-License-Identifier: Apache-2.0

// Layers the cirrus (lib/cloud_cirrus.slangh) with the frame's cloud render, into the texture the host
// composites. This comes after the temporal and spatial filters and outside their history: the
// streaks hold no march noise to filter, and the filters would only soften them.
//
// Scene pixels pass through, unless the eye is above the sheet and the surface is beyond it. Output RGBA16F, the cloud render's
// format (premultiplied radiance, view-ray transmittance).

#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"
#include "numos/util/camera.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1)
ConstantBuffer<Camera> camera;

layout(binding = 2)
SamplerState cirrusMapSampler;

layout(binding = 3)
SamplerState lutSampler;

layout(binding = 4)
Texture2D<float2> cirrusMap;

layout(binding = 5)
Texture2D<float4> transmittanceLut;

layout(binding = 6)
Texture2D<float4> skyViewLut;

layout(binding = 7)
Texture2D<float4> multiscatteringLut;

layout(binding = 8)
Texture2D<float4> cloudInput;

layout(binding = 9)
Texture2D<float4> cloudDepth;

layout(binding = 10, rgba16f)
RWTexture2D<float4> cloudOutput;

layout(binding = 11)
Texture2D<float> linearViewZ;

struct CirrusCompositePush {
  float missLinearViewZ;  // FrameInputs: texels at or beyond this are sky
};
layout(push_constant)
ConstantBuffer<CirrusCompositePush> push;

#include "numos/lib/aerial_perspective.slangh"
#include "numos/lib/cloud_cirrus.slangh"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID) {
  uint2 size;
  cloudOutput.GetDimensions(size.x, size.y);
  if (any(pixel >= size)) {
    return;
  }
  const vec4 cloud = cloudInput.Load(int3(pixel, 0));
  const vec4 depth = cloudDepth.Load(int3(pixel, 0));
  // From the host's depth, not cloudDepth.z: the march's clear-sky path writes that as sky everywhere.
  const AtmosphereArgs args = atmosphereArgs;
  const float viewZ = abs(linearViewZ.Load(int3(pixel, 0)));
  const bool miss = viewZ >= push.missLinearViewZ;
  if (!miss && !cirrusEyeAbove(args)) {
    cloudOutput[pixel] = cloud;
    return;
  }
  // The ray through this pixel, jittered as the host's sample was.
  const vec2 screenUV = cameraPixelCoordinateToScreenUV(camera, ivec2(pixel));
  const CameraDirections ray = cameraScreenUVToDirection(camera, screenUV, true);
  const vec3 viewDirYUp = normalize(worldToAtmosphereYUp(ray.worldDirection, args.isZUp != 0u, args.flipUpAxis != 0u));
  const CameraDirections nextRay = cameraScreenUVToDirection(camera, cameraPixelCoordinateToScreenUV(camera, ivec2(pixel) + ivec2(1, 0)), true);
  const float pixelAngle = length(nextRay.worldDirection - ray.worldDirection);

  float distanceKm;
  vec4 cirrus = cirrusEvaluate(viewDirYUp, args, pixelAngle, cirrusMap, cirrusMapSampler, transmittanceLut, skyViewLut,
                               lutSampler, distanceKm);
  if (!miss) {
    const float forwardPerDistance = max(abs(cameraScreenUVToDirection(camera, screenUV, false).viewDirection.z), 1e-6f);
    if (distanceKm >= numosWorldToKm(viewZ / forwardPerDistance, args)) {
      cloudOutput[pixel] = cloud;
      return;
    }
  }
  // numosApplyCloudComposite lays the height fog over a cloud it has a depth for. Where only cirrus
  // shows it has none, so the fog in front is applied here, from the near field's edge likewise.
  if (args.fogExtinctionPerKm > 0.0f && depth.y <= 0.0f && distanceKm > 0.0f) {
    const float distanceWorld = numosKmToWorld(distanceKm, args);
    const float nearEdgeWorld = min(args.apFroxelRange / max(abs(ray.viewDirection.z), 1e-6f), distanceWorld);
    const float fogT = exp(-apFogOpticalDepth(viewDirYUp, nearEdgeWorld, distanceWorld, args));
    const vec3 fogLight = apFarFogRadiance(viewDirYUp, args, transmittanceLut, multiscatteringLut, lutSampler);
    cirrus.rgb = cirrus.rgb * fogT + fogLight * ((1.0f - fogT) * cirrus.a);
  }
  cloudOutput[pixel] = cirrusLayerWithCloud(cloud, cirrus, args);
}
