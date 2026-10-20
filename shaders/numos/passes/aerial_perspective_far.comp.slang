// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Aerial perspective far field (docs/aerial-perspective-v2.md, section 4.3).
//
// Hillaire 2020's aerial perspective volume, re-indexed around the camera instead of fitted to its
// frustum: X/Y are the sky-view LUT's azimuth and latitude, Z is distance out to 400 km. Every
// direction is covered, so the clouds, reflections and anything else off-screen can read it, and
// the composite reads it for every surface past the froxel range. Atmosphere only: planet shadow,
// sun transmittance, Psi_ms and the cloud-shadow hook. The scene, the fog and local lights belong
// to the near field.
//
// One group per direction, its slices integrated front to back by a parallel scan
// (aerial_perspective_scan.slangh) so slice k+1 is exactly slice k plus the segment between them; the
// composite subtracts two slices of one column to get a segment, and that difference is only
// meaningful if they come from one integration.

#include "numos/lib/aerial_perspective.slangh"
#include "numos/lib/aerial_perspective_binding_indices.h"
// 48 slices: one per lane.
#define AP_SCAN_LANES 64u
#define AP_SCAN_MAX_CHUNK 1u
#include "numos/lib/aerial_perspective_scan.slangh"

layout(binding = AP_FAR_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = AP_FAR_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = AP_FAR_MULTISCATTERING_INPUT)
Texture2D<float4> multiscatteringLutInput;

layout(binding = AP_FAR_LUT_SAMPLER)
SamplerState lutSampler;

layout(binding = AP_FAR_CLOUD_SHADOW_MAP)
Texture3D<float2> cloudShadowMap;

layout(binding = AP_FAR_CLOUD_SAMPLER)
SamplerState cloudSampler;

layout(binding = AP_FAR_SCATTER_OUTPUT, rgba16f)
RWTexture3D<float4> farScatterOutput;

layout(binding = AP_FAR_TRANSMITTANCE_OUTPUT, rgba16f)
RWTexture3D<float4> farTransmittanceOutput;

// Sub-steps per slice. Slices are ~15 % deeper than their predecessor, so two midpoint steps keep
// every step within ~8 % of its own distance - finer than the scale heights it resolves past 10 km.
static const uint kStepsPerSlice = 2u;

[shader("compute")]
[numthreads(AP_SCAN_LANES, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThread : SV_GroupThreadID) {
  const AtmosphereArgs args = atmosphereArgs;
  const uint2 threadId = groupId.xy;  // the direction; uniform over the group
  const uint lane = groupThread.x;
  if (any(threadId >= uint2(args.apFarSizeX, args.apFarSizeY))) {
    return;
  }

  const vec2 uv = (vec2(threadId) + 0.5f) / vec2(args.apFarSizeX, args.apFarSizeY);
  const vec3 dirYUp = apFarUvToDirection(uv);
  const vec3 dirWorld = normalize(atmosphereYUpToWorld(dirYUp, args.isZUp != 0u, args.flipUpAxis != 0u));

  // Past the top of the atmosphere there is nothing left to integrate; below the datum the march
  // keeps going through ground-level air, standing in for terrain the atmosphere knows nothing about.
  float exitKm = 1e9f;
  {
    float t0, t1;
    if (intersectSphere(vec3(0.0f), dirYUp, getAirPlanetCenter(args), args.atmosphereRadius, t0, t1)) {
      exitKm = max(t1, 0.0f);
    }
  }

  // This lane's chunk of slices, from an empty segment; localI/T[c] is the chunk up to slice c.
  const uint slices = min(max(args.apFarSlices, 1u), AP_SCAN_LANES * AP_SCAN_MAX_CHUNK);
  const uint chunk = apScanChunk(slices);
  vec3 localI[AP_SCAN_MAX_CHUNK];
  vec3 localT[AP_SCAN_MAX_CHUNK];
  vec3 inScatter = vec3(0.0f);
  vec3 transmittance = vec3(1.0f);
  [unroll]
  for (uint c = 0u; c < AP_SCAN_MAX_CHUNK; ++c) {
    const uint slice = lane * chunk + c;
    if (c < chunk && slice < slices) {
      const float startKm = slice == 0u ? 0.0f : apFarSliceToKm(float(slice) / float(slices), args);
      const float endKm = apFarSliceToKm(float(slice + 1u) / float(slices), args);
      const float stepKm = (min(endKm, exitKm) - startKm) / float(kStepsPerSlice);
      if (stepKm > 0.0f) {
        for (uint i = 0u; i < kStepsPerSlice; ++i) {
          const float midKm = startKm + (float(i) + 0.5f) * stepKm;
          const vec3 worldPos = args.cameraPosition + dirWorld * numosAirKmToWorld(midKm, args);
          const float cloudT = apCloudSunTransmittance(worldPos, args, cloudShadowMap, cloudSampler);
          apIntegrateSegment(worldPos, dirYUp, numosAirKmToWorld(stepKm, args), /*includeNearMedia=*/false,
                             1.0f, 1.0f, cloudT, vec3(0.0f), args,
                             transmittanceLutInput, multiscatteringLutInput, lutSampler,
                             inScatter, transmittance);
        }
      }
    }
    localI[c] = inScatter;
    localT[c] = transmittance;
  }

  vec3 prefixI, prefixT;
  apScanExclusive(lane, inScatter, transmittance, prefixI, prefixT);
  [unroll]
  for (uint c = 0u; c < AP_SCAN_MAX_CHUNK; ++c) {
    const uint slice = lane * chunk + c;
    if (c < chunk && slice < slices) {
      const vec3 t = prefixT * localT[c];
      farScatterOutput[uint3(threadId, slice)] = vec4(prefixI + prefixT * localI[c], dot(t, vec3(1.0f / 3.0f)));
      farTransmittanceOutput[uint3(threadId, slice)] = vec4(t, 1.0f);
    }
  }
}
