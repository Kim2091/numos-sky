// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Clustered light cull for the froxel grid's local-light sampling.
//
// One thread per cluster: AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE^2 froxels by one depth slice, i.e. the
// froxel grid coarsened to the lighting pass's thread group, so a group reads one list. The test is
// sphere against a bounding sphere of the cluster's frustum cell: one dot product, conservative
// (over-inclusion costs the RIS a zero-weight candidate; under-inclusion would pop a light out of
// the fog), and the lighting pass re-checks each light's true reach per froxel anyway.
//
// Thread layer z == apFroxelSlices writes the per-tile list the composite's per-pixel glow reads:
// every light whose sphere touches the tile's frustum column (four side planes) at any depth.

#include "numos/lib/aerial_perspective_lights.slangh"
#include "numos/lib/aerial_perspective_binding_indices.h"

layout(binding = AP_CULL_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = AP_CULL_CAMERA)
ConstantBuffer<Camera> atmosphereCamera;

layout(binding = AP_CULL_LIGHTS)
StructuredBuffer<AerialPerspectiveLight> aerialPerspectiveLights;

layout(binding = AP_CULL_CLUSTERS)
RWStructuredBuffer<uint> aerialPerspectiveLightClusters;

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 threadId : SV_DispatchThreadID) {
  const AtmosphereArgs args = atmosphereArgs;

  if (any(threadId.xy >= uint2(args.apLocalLightTilesX, args.apLocalLightTilesY)) || threadId.z > args.apFroxelSlices) {
    return;
  }

  const bool columnList = threadId.z == args.apFroxelSlices;
  const uint clusterOffset = columnList
    ? apLightTileOffset(threadId.xy, args)
    : aerialPerspectiveLightClusterOffset(threadId.xy, threadId.z, args);

  const uint lightCount = args.apLocalLightCount;
  if (lightCount == 0u) {
    aerialPerspectiveLightClusters[clusterOffset] = 0u;
    return;
  }

  // Screen UV span of the tile, clamped at the grid edge so a partial tile does not pull in lights
  // no froxel inside it can reach.
  const vec2 gridSize = vec2(args.apFroxelSizeX, args.apFroxelSizeY);
  const float tile = float(AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE);
  const vec2 uvMin = vec2(threadId.xy) * tile / gridSize;
  const vec2 uvMax = min((vec2(threadId.xy) + 1.0f) * tile, gridSize) / gridSize;

  if (columnList) {
    // Side planes through the camera, oriented to contain the tile's centre ray.
    vec3 rays[4];
    rays[0] = apFroxelRayVector(uvMin, atmosphereCamera);
    rays[1] = apFroxelRayVector(vec2(uvMax.x, uvMin.y), atmosphereCamera);
    rays[2] = apFroxelRayVector(uvMax, atmosphereCamera);
    rays[3] = apFroxelRayVector(vec2(uvMin.x, uvMax.y), atmosphereCamera);
    const vec3 centerRay = rays[0] + rays[1] + rays[2] + rays[3];
    vec3 planes[4];
    [unroll]
    for (uint k = 0u; k < 4u; ++k) {
      vec3 n = normalize(cross(rays[k], rays[(k + 1u) & 3u]));
      planes[k] = dot(n, centerRay) < 0.0f ? -n : n;
    }
    uint count = 0u;
    for (uint lightIndex = 0u; lightIndex < lightCount && count < AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER; ++lightIndex) {
      const AerialPerspectiveLight light = aerialPerspectiveLights[lightIndex];
      const vec3 rel = light.position - args.cameraPosition;
      bool outside = false;
      [unroll]
      for (uint k = 0u; k < 4u; ++k) {
        outside = outside || dot(rel, planes[k]) < -light.influenceRadius;
      }
      if (!outside) {
        aerialPerspectiveLightClusters[clusterOffset + 1u + count] = lightIndex;
        ++count;
      }
    }
    aerialPerspectiveLightClusters[clusterOffset] = count;
    return;
  }

  const float slices = float(max(args.apFroxelSlices, 1u));
  const float nearForward = apFroxelSliceToDepth(float(threadId.z) / slices, args);
  const float farForward = apFroxelSliceToDepth(float(threadId.z + 1u) / slices, args);

  // Eight corners of the cell relative to the camera; the ray vector's forward component is one,
  // so scaling it by a forward distance lands on that depth plane.
  vec3 corners[8];
  [unroll]
  for (uint corner = 0u; corner < 8u; ++corner) {
    const vec2 uv = vec2((corner & 1u) != 0u ? uvMax.x : uvMin.x, (corner & 2u) != 0u ? uvMax.y : uvMin.y);
    corners[corner] = apFroxelRayVector(uv, atmosphereCamera) * ((corner & 4u) != 0u ? farForward : nearForward);
  }

  vec3 center = vec3(0.0f);
  [unroll]
  for (uint i = 0u; i < 8u; ++i) {
    center += corners[i];
  }
  center *= 0.125f;

  float radiusSq = 0.0f;
  [unroll]
  for (uint j = 0u; j < 8u; ++j) {
    radiusSq = max(radiusSq, dot(corners[j] - center, corners[j] - center));
  }
  const float radius = sqrt(radiusSq);
  const vec3 centerWorld = args.cameraPosition + center;

  uint written = 0u;
  for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex) {
    const AerialPerspectiveLight light = aerialPerspectiveLights[lightIndex];
    const vec3 offset = light.position - centerWorld;
    const float reach = radius + light.influenceRadius;
    if (dot(offset, offset) > reach * reach) {
      continue;
    }

    aerialPerspectiveLightClusters[clusterOffset + 1u + written] = lightIndex;

    // The CPU sorts the list by importance, so stopping at the cap keeps the brightest lights.
    ++written;
    if (written >= AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER) {
      break;
    }
  }

  aerialPerspectiveLightClusters[clusterOffset] = written;
}
