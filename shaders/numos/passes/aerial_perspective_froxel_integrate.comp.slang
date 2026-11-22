// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Aerial perspective near field, integration (docs/aerial-perspective-v2.md, section 4.2).
//
// One thread per froxel column, front to back, so every slice is exactly its predecessor plus the
// segment between them and the composite can interpolate between neighbors without overshoot.
// Each slice is one segment lit by what the lighting pass accumulated (scene visibility of sun and
// sky) and by what is analytic and therefore fresh every frame (sun transmittance, planet shadow,
// the cloud-shadow hook, Psi_ms, phase, density). Local lights are not integrated here: their glow
// falls off far faster than a froxel is wide, so the composite evaluates it per pixel
// (apLocalLightAirlight) and only takes its shadowing from the grid.
//
// Writes the cumulative in-scatter to the far edge of every slice in RGB and the column's mean
// transmittance in A. RGB transmittance is not stored: it has a closed form (apNearTransmittance).
//
// Past the deepest visible surface of the column's screen tile no pixel of the tile looks, and the
// froxels there only hold samples moved in front of a surface, so those slices carry on with the
// last froxel in front of it (docs/aerial-perspective-v2.md 4.2.2). The cumulative values stay
// continuous for the filtered reads of the neighboring columns.

#include "numos/lib/aerial_perspective.slangh"
#include "numos/lib/aerial_perspective_binding_indices.h"

layout(binding = AP_INTEGRATE_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = AP_INTEGRATE_CAMERA)
ConstantBuffer<Camera> atmosphereCamera;

layout(binding = AP_INTEGRATE_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = AP_INTEGRATE_MULTISCATTERING_INPUT)
Texture2D<float4> multiscatteringLutInput;

layout(binding = AP_INTEGRATE_LUT_SAMPLER)
SamplerState lutSampler;

layout(binding = AP_INTEGRATE_LIGHTING)
Texture3D<float4> froxelLighting;

layout(binding = AP_INTEGRATE_LIGHTING_AUX)
Texture3D<float4> froxelAux;

layout(binding = AP_INTEGRATE_CLOUD_SHADOW_MAP)
Texture3D<float2> cloudShadowMap;

layout(binding = AP_INTEGRATE_CLOUD_SAMPLER)
SamplerState cloudSampler;

layout(binding = AP_INTEGRATE_OUTPUT, rgba16f)
RWTexture3D<float4> nearScatterOutput;

// The deepest visible surface of each column's screen tile (aerial_perspective_tile_depth).
layout(binding = AP_INTEGRATE_TILE_DEPTH)
Texture2D<float> tileDepth;

// [0]: the number of columns whose screen tile shows the sky or a surface past the froxel range, the
// pixels the composite takes the far field for. The renderer reads it back to skip far-field updates
// nothing reads.
layout(binding = AP_INTEGRATE_FAR_USAGE)
RWStructuredBuffer<uint> farUsage;

// The sky visibility filter's largest radius in froxels, and the taps of its square at that radius.
static const int kApSkyFilterMaxRadius = 2;
static const int kApSkyFilterWidth = 2 * kApSkyFilterMaxRadius + 1;
static const int kApSkyFilterTaps = kApSkyFilterWidth * kApSkyFilterWidth;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 column : SV_DispatchThreadID) {
  const AtmosphereArgs args = atmosphereArgs;
  if (any(column >= uint2(args.apFroxelSizeX, args.apFroxelSizeY))) {
    return;
  }

  const vec2 uv = (vec2(column) + 0.5f) / vec2(args.apFroxelSizeX, args.apFroxelSizeY);
  const vec3 rayVector = apFroxelRayVector(uv, atmosphereCamera);
  const float forwardToRay = length(rayVector);
  const vec3 dirWorld = rayVector / forwardToRay;
  const vec3 dirYUp = normalize(worldToAtmosphereYUp(dirWorld, args.isZUp != 0u, args.flipUpAxis != 0u));

  vec3 inScatter = vec3(0.0f);
  vec3 transmittance = vec3(1.0f);
  const uint slices = max(args.apFroxelSlices, 1u);
  float startWorld = 0.0f;
  const float debugVisibility = apDebugZeroesSunAndSky(args) ? 0.0f : 1.0f;
  const float tileForward = tileDepth.Load(int3(column, 0));
  if (tileForward > args.apFroxelRange) {
    InterlockedAdd(farUsage[0], 1u);
  }
  const float tileLimit = apSurfaceSampleLimit(tileForward, args);

  // The sky visibility is box-filtered over the neighboring columns (rtx.atmosphere.aerialPerspectiveSkyFilterRadius).
  // Sky light in the air varies slowly across the screen, while its per-froxel estimate in shaded air
  // is a small share of rare hits, so the filter removes blotches there. A neighbor's froxel counts
  // only in front of its own tile's surface, the same rule this column follows, so the air in front
  // of a wall beside a doorway does not darken the air seen through it. Sun visibility is not
  // filtered: that would widen every shaft and shadow edge.
  const int skyRadius = int(min(args.apSkyFilterRadius, uint(kApSkyFilterMaxRadius)));
  const int2 grid = int2(args.apFroxelSizeX, args.apFroxelSizeY);
  float neighborLimit[kApSkyFilterTaps];
  [unroll]
  for (int tap = 0; tap < kApSkyFilterTaps; ++tap) {
    const int2 offset = int2(tap % kApSkyFilterWidth, tap / kApSkyFilterWidth) - kApSkyFilterMaxRadius;
    const int2 neighbor = int2(column) + offset;
    neighborLimit[tap] = -1.0f;
    if (all(abs(offset) <= skyRadius) && all(neighbor >= 0) && all(neighbor < grid)) {
      neighborLimit[tap] = all(offset == 0) ? tileLimit
        : apSurfaceSampleLimit(tileDepth.Load(int3(neighbor, 0)), args);
    }
  }

  float sunVisibility = 1.0f;
  float skyVisibility = 1.0f;
  for (uint slice = 0u; slice < slices; ++slice) {
    const float endWorld = apFroxelSliceToDepth(float(slice + 1u) / float(slices), args) * forwardToRay;
    const float sliceStart = apFroxelSliceToDepth(float(slice) / float(slices), args);
    if (slice == 0u || sliceStart < tileLimit) {
      sunVisibility = froxelLighting.Load(int4(column, slice, 0)).a;
      float skySum = 0.0f;
      float skyCount = 0.0f;
      [unroll]
      for (int tap = 0; tap < kApSkyFilterTaps; ++tap) {
        if (neighborLimit[tap] >= 0.0f && (slice == 0u || sliceStart < neighborLimit[tap])) {
          const int2 neighbor = int2(column) + int2(tap % kApSkyFilterWidth, tap / kApSkyFilterWidth) - kApSkyFilterMaxRadius;
          skySum += froxelAux.Load(int4(neighbor, slice, 0)).r;
          skyCount += 1.0f;
        }
      }
      skyVisibility = skyCount > 0.0f ? skySum / skyCount : froxelAux.Load(int4(column, slice, 0)).r;
    }
    // The host's own volumetrics own everything nearer than the handoff.
    const float segmentStart = max(startWorld, args.apStartDistance);
    if (endWorld > segmentStart) {
      const float midWorld = 0.5f * (segmentStart + endWorld);
      const vec3 worldPos = args.cameraPosition + dirWorld * midWorld;
      const float cloudT = apCloudSunTransmittance(worldPos, args, cloudShadowMap, cloudSampler);
      apIntegrateSegment(worldPos, dirYUp, endWorld - segmentStart, /*includeNearMedia=*/true,
                         sunVisibility * debugVisibility, skyVisibility * debugVisibility, cloudT, vec3(0.0f), args,
                         transmittanceLutInput, multiscatteringLutInput, lutSampler,
                         inScatter, transmittance);
    }
    nearScatterOutput[uint3(column, slice)] = vec4(inScatter, dot(transmittance, vec3(1.0f / 3.0f)));
    startWorld = endWorld;
  }
}
