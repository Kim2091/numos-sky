// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Aerial perspective near field, integration (docs/aerial-perspective-v2.md, section 4.2).
//
// One thread per froxel column, front to back, so every slice is exactly its predecessor plus the
// segment between them and the composite can interpolate between neighbours without overshoot.
// Each slice is one segment lit by what the lighting pass accumulated (scene visibility of sun and
// sky) and by what is analytic and therefore fresh every frame (sun transmittance, planet shadow,
// the cloud-shadow hook, Psi_ms, phase, density). Local lights are not integrated here: their glow
// falls off far faster than a froxel is wide, so the composite evaluates it per pixel
// (apLocalLightAirlight) and only takes its shadowing from the grid.
//
// Writes the cumulative in-scatter to the far edge of every slice in RGB and the column's mean
// transmittance in A. RGB transmittance is not stored: it has a closed form (apNearTransmittance).

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
  for (uint slice = 0u; slice < slices; ++slice) {
    const float endWorld = apFroxelSliceToDepth(float(slice + 1u) / float(slices), args) * forwardToRay;
    // The host's own volumetrics own everything nearer than the handoff.
    const float segmentStart = max(startWorld, args.apStartDistance);
    if (endWorld > segmentStart) {
      const float midWorld = 0.5f * (segmentStart + endWorld);
      const vec3 worldPos = args.cameraPosition + dirWorld * midWorld;
      const float sunVisibility = froxelLighting.Load(int4(column, slice, 0)).a;
      const float skyVisibility = froxelAux.Load(int4(column, slice, 0)).r;
      const float cloudT = apCloudSunTransmittance(worldPos, args, cloudShadowMap, cloudSampler);
      apIntegrateSegment(worldPos, dirYUp, endWorld - segmentStart, /*includeNearMedia=*/true,
                         sunVisibility, skyVisibility, cloudT, vec3(0.0f), args,
                         transmittanceLutInput, multiscatteringLutInput, lutSampler,
                         inScatter, transmittance);
    }
    nearScatterOutput[uint3(column, slice)] = vec4(inScatter, dot(transmittance, vec3(1.0f / 3.0f)));
    startWorld = endWorld;
  }
}
