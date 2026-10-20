// Copyright 2026 Kim2091 (Sparkles)
// Portions Copyright (c) 2025-2026 CattaRappa (sambow23), softsoundd
// SPDX-License-Identifier: Apache-2.0 AND MIT
// The portions are under the MIT licence (licenses/LICENSE-MIT-contributors.txt);
// see THIRD_PARTY_NOTICES.md.

// Transmittance LUT (Hillaire 2020, section 5.2): transmittance from any altitude along any view
// zenith angle to the top of the atmosphere.

#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/transmittance_lut_binding_indices.h"

layout(binding = TRANSMITTANCE_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = TRANSMITTANCE_LUT_OUTPUT, rgba16f)
RWTexture2D<float4> transmittanceLutOutput;

// Transmittance from (altitude, viewZenithCosAngle) to the top of the atmosphere.
//
// The planet is not tested. A hard zero for rays through the ground would be averaged into the
// neighbouring texels by the linear sampler and dim and redden the sun too early at sunset. Those
// rays are integrated with the density clamped at ground level instead, and the planet's shadow is
// a separate visibility term where the LUT is used (evalPlanetShadow, evalSunShadowing).
vec3 computeTransmittance(float altitude, float viewZenithCosAngle, AtmosphereArgs args) {
  altitude = clamp(altitude, 0.0f, args.atmosphereThickness);
  viewZenithCosAngle = clamp(viewZenithCosAngle, -1.0f, 1.0f);

  // The LUT depends on the altitude above the planet alone, so the planet is centred directly below
  // the origin. getPlanetCenter would add cameraAltitudeKm, which this bake must not depend on.
  const vec3 planetCenter = vec3(0.0f, -args.planetRadius, 0.0f);
  const vec3 rayOrigin = vec3(0.0f, altitude, 0.0f);

  const float sinZenith = sqrt(max(0.0f, 1.0f - viewZenithCosAngle * viewZenithCosAngle));
  const vec3 rayDir = normalize(vec3(sinZenith, viewZenithCosAngle, 0.0f));

  float t0, t1;
  if (!intersectSphere(rayOrigin, rayDir, planetCenter, args.atmosphereRadius, t0, t1)) {
    return vec3(1.0f);
  }

  const float tStart = max(0.0f, t0);
  const float tEnd = t1;
  if (tEnd <= tStart) {
    return vec3(1.0f);
  }

  // Power-distributed segments: short near the ray origin where the medium is densest.
  const int numSamples = 40;
  const float span = tEnd - tStart;
  const float invSamples = 1.0f / float(numSamples);

  vec3 opticalDepth = vec3(0.0f);
  float segmentStart = 0.0f;

  for (int i = 0; i < numSamples; ++i) {
    const float segmentEnd = atmosphereStepFraction(float(i + 1) * invSamples);
    const float dt = (segmentEnd - segmentStart) * span;
    const float sampleT = tStart + (segmentStart + (segmentEnd - segmentStart) * 0.5f) * span;
    segmentStart = segmentEnd;

    if (dt <= 0.0f) {
      continue;
    }

    const vec3 samplePos = rayOrigin + rayDir * sampleT;
    const float sampleAltitude = clamp(
      length(samplePos - planetCenter) - args.planetRadius, 0.0f, args.atmosphereThickness);

    opticalDepth += sampleAtmosphereMedium(sampleAltitude, args).extinction * dt;
  }

  opticalDepth = min(opticalDepth, vec3(1e3f));

  return clamp(exp(-opticalDepth), vec3(0.0f), vec3(1.0f));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;

  if (threadId.x >= args.transmittanceLutWidth || threadId.y >= args.transmittanceLutHeight) {
    return;
  }

  vec2 uv = (vec2(threadId) + 0.5f) / vec2(args.transmittanceLutWidth, args.transmittanceLutHeight);

  float viewZenithCosAngle, altitude;
  uvToTransmittanceLutParams(uv, args, viewZenithCosAngle, altitude);

  vec3 transmittance = computeTransmittance(altitude, viewZenithCosAngle, args);

  transmittanceLutOutput[threadId] = float4(transmittance, 1.0f);
}
