// Copyright 2026 Kim2091 (Sparkles)
// Portions Copyright (c) 2025-2026 CattaRappa (sambow23), softsoundd
// SPDX-License-Identifier: Apache-2.0 AND MIT
// The portions are under the MIT licence (licenses/LICENSE-MIT-contributors.txt);
// see THIRD_PARTY_NOTICES.md.

/*
 * atmosphere_common.slangh's computeGroundReflectionAnalytical and its analytical transmittance are
 * adapted from Blender's intern/sky/source/sky_multiple_scattering.cpp, under this notice:
 *
 *   SPDX-License-Identifier: MIT
 *   SPDX-FileCopyrightText: 2022 Fernando García Liñán
 *   SPDX-FileCopyrightText: 2011-2025 Blender Authors
 *
 * Upstream:
 *   https://projects.blender.org/blender/blender/src/branch/main/intern/sky/source/sky_multiple_scattering.cpp
 *
 * MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// Multiple scattering LUT (Hillaire 2020, section 5.5).
//
// For each (sun zenith angle, altitude) entry this integrates, over 64 uniformly distributed
// directions around the sample point:
//   - L_2ndOrder (Eqs. 5-6): second-order in-scattered luminance for a unit illuminance light,
//     including the diffuse response of the planet's ground;
//   - f_ms (Eqs. 7-8): a unitless [0, 1] factor for how much energy the surrounding medium returns
//     toward the sample point.
// The series of higher orders then collapses to F_ms = 1 / (1 - f_ms) (Eq. 9), giving
// Psi_ms = L_2ndOrder * F_ms (Eq. 10).
//
// Psi_ms is in sr^-1: times a directional light's illuminance and the local sigma_s it is that
// light's complete multiple scattering (Eq. 11). Neither is baked in; callers apply each once.
// All orders are included, so dense atmospheres turn milky rather than dark as density rises.
//
// The bake re-runs only when the atmosphere parameters change.

#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/multiscattering_lut_binding_indices.h"

layout(binding = MULTISCATTERING_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = MULTISCATTERING_LUT_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = MULTISCATTERING_LUT_SAMPLER)
SamplerState transmittanceSampler;

layout(binding = MULTISCATTERING_LUT_OUTPUT, rgba16f)
RWTexture2D<float4> multiscatteringLutOutput;

// 8x8 directions over the sphere, matching the paper's 64 direction budget.
static const uint kSqrtDirectionCount = 8;
// Ray march step count per direction. The paper reports 20 as sufficient.
static const int kMarchStepCount = 20;
// The twilight half (see multiscatteringTwilightLutCoord) resolves a sunlit shell that is thin and
// far away, so it takes 4x the directions and twice the steps; it is the same 32x32 texels.
static const uint kTwilightSqrtDirectionCount = 16;
static const int kTwilightMarchStepCount = 40;

struct MultiscatterIntegral {
  vec3 secondOrderLuminance;  // L_2ndOrder for E_I = 1
  vec3 transferFactor;        // f_ms
};

// Integrates Equations 6 and 8 along a single direction leaving the sample point.
MultiscatterIntegral integrateDirection<let STEPS : int>(
  vec3 samplePos, vec3 rayDir, vec3 sunDir, AtmosphereArgs args)
{
  MultiscatterIntegral result;
  result.secondOrderLuminance = vec3(0.0f);
  result.transferFactor = vec3(0.0f);

  const vec3 planetCenter = getPlanetCenter(args);

  float tTop0, tTop1;
  if (!intersectSphere(samplePos, rayDir, planetCenter, args.atmosphereRadius, tTop0, tTop1)) {
    return result;
  }

  // The sample point is inside the atmosphere, so the far root is the exit distance.
  float tEnd = max(tTop1, 0.0f);

  // Stop at the ground when the direction points into the planet. Solved in (r, mu) form: the LUT's
  // bottom row sits exactly on the ground, where the generic test's near root of every downward
  // direction is 0 and would drop the direction, ground bounce included. A sample on the ground
  // looking down sees the ground itself: tGround = 0, no air, the ground term at full throughput.
  const vec3 toSample = samplePos - planetCenter;
  const float sampleRadius = length(toSample);
  const float rayMu = dot(toSample, rayDir) / max(sampleRadius, kAtmosphereEpsilon);
  const float groundDiscriminant = sampleRadius * sampleRadius * (rayMu * rayMu - 1.0f)
                                 + args.planetRadius * args.planetRadius;
  const bool hitsGround = rayMu < 0.0f && groundDiscriminant >= 0.0f;
  if (hitsGround) {
    tEnd = max(-sampleRadius * rayMu - sqrt(groundDiscriminant), 0.0f);
  }

  if (tEnd <= 0.0f && !hitsGround) {
    return result;
  }

  const float invSteps = 1.0f / float(STEPS);
  vec3 throughput = vec3(1.0f);
  float segmentStart = 0.0f;

  for (int i = 0; i < STEPS; ++i) {
    const float segmentEnd = atmosphereStepFraction(float(i + 1) * invSteps);
    const float dt = (segmentEnd - segmentStart) * tEnd;
    const float t = (segmentStart + (segmentEnd - segmentStart) * 0.5f) * tEnd;
    segmentStart = segmentEnd;

    if (dt <= 0.0f) {
      continue;
    }

    const vec3 marchPos = samplePos + rayDir * t;
    const float altitude = clamp(
      length(marchPos - planetCenter) - args.planetRadius, 0.0f, args.atmosphereThickness);

    const AtmosphereMedium medium = sampleAtmosphereMedium(altitude, args);
    const vec3 sampleTransmittance = clamp(exp(-medium.extinction * dt), vec3(0.0f), vec3(1.0f));
    const vec3 clampedExtinction = max(medium.extinction, vec3(1e-7f));

    // Equation 6: single scattering with the isotropic phase and a unit illuminance.
    const vec3 sunShadowing = evalSunShadowing(
      marchPos, sunDir, transmittanceLutInput, transmittanceSampler, args);
    const vec3 inScatter = medium.scattering * kIsotropicPhase * sunShadowing;

    // Equation 8: the same integral without shadowing or phase, which normalises f_ms into [0, 1].
    const vec3 transferIntegrand = medium.scattering;

    // Energy conserving analytical integration (Equation 17).
    result.secondOrderLuminance += throughput * ((inScatter - inScatter * sampleTransmittance) / clampedExtinction);
    result.transferFactor += throughput * ((transferIntegrand - transferIntegrand * sampleTransmittance) / clampedExtinction);

    throughput *= sampleTransmittance;
  }

  // Diffuse response of the ground, the L_o term of Equation 6, at the ground point the direction
  // reaches, with that point's own sun shadowing.
  if (hitsGround) {
    const vec3 groundPos = samplePos + rayDir * tEnd;
    const vec3 groundNormal = normalize(groundPos - planetCenter);
    const float NdotL = dot(groundNormal, sunDir);

    if (NdotL > 0.0f) {
      const vec3 sunShadowing = evalSunShadowing(
        groundPos, sunDir, transmittanceLutInput, transmittanceSampler, args);
      result.secondOrderLuminance += throughput * sunShadowing * NdotL * (args.groundAlbedo / kPi);
    }
  }

  return result;
}

vec3 computeMultiscattering<let SQRT_DIRECTIONS : int, let STEPS : int>(
  float altitude, float sunZenithCosAngle, AtmosphereArgs args) {
  const vec3 samplePos = vec3(0.0f, clamp(altitude, 0.0f, args.atmosphereThickness), 0.0f);

  const float sunZenithSinAngle = sqrt(max(0.0f, 1.0f - sunZenithCosAngle * sunZenithCosAngle));
  const vec3 sunDir = normalize(vec3(sunZenithSinAngle, sunZenithCosAngle, 0.0f));

  vec3 secondOrderLuminance = vec3(0.0f);
  vec3 transferFactor = vec3(0.0f);

  // Uniform directions over the sphere, so the Monte Carlo estimator of Equations 5 and 7 carries a
  // 4pi/N weight. Those equations also apply p_u to the whole per-direction integral, on top of the
  // p_u that Equation 6 already applies to its own single scattering term. Uniform sphere sampling
  // needs no Jacobian, so the weight below is exact.
  for (uint i = 0; i < uint(SQRT_DIRECTIONS); ++i) {
    for (uint j = 0; j < uint(SQRT_DIRECTIONS); ++j) {
      const float u = (float(i) + 0.5f) / float(SQRT_DIRECTIONS);
      const float v = (float(j) + 0.5f) / float(SQRT_DIRECTIONS);

      const float theta = 2.0f * kPi * u;
      const float cosPhi = 1.0f - 2.0f * v;
      const float sinPhi = sqrt(max(0.0f, 1.0f - cosPhi * cosPhi));
      const vec3 rayDir = vec3(sinPhi * cos(theta), cosPhi, sinPhi * sin(theta));

      const MultiscatterIntegral integral = integrateDirection<STEPS>(samplePos, rayDir, sunDir, args);

      secondOrderLuminance += integral.secondOrderLuminance;
      transferFactor += integral.transferFactor;
    }
  }

  // The sphere solid angle and the outer p_u cancel exactly, leaving the mean over directions.
  const float directionWeight = (4.0f * kPi) * kIsotropicPhase / float(SQRT_DIRECTIONS * SQRT_DIRECTIONS);
  secondOrderLuminance *= directionWeight;
  transferFactor *= directionWeight;

  // Equations 9 and 10. f_ms is in [0, 1) by construction; the clamp guards against extreme
  // atmosphere settings.
  const vec3 clampedTransfer = clamp(transferFactor, vec3(0.0f), vec3(0.999f));
  const vec3 multiScatteringSum = vec3(1.0f) / (vec3(1.0f) - clampedTransfer);

  vec3 psiMs = secondOrderLuminance * multiScatteringSum;
  psiMs = max(psiMs, vec3(0.0f));
  if (any(isnan(psiMs)) || any(isinf(psiMs))) {
    psiMs = vec3(0.0f);
  }

  return psiMs;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;
  // The LUT is indexed by altitude above the planet alone, so the planet centre must sit exactly
  // planetRadius below a sample at (0, altitude, 0); getPlanetCenter would add cameraAltitudeKm.
  args.cameraAltitudeKm = 0.0f;

  // Two size x size halves stacked vertically.
  if (threadId.x >= args.multiscatteringLutSize || threadId.y >= 2u * args.multiscatteringLutSize) {
    return;
  }

  // Bottom half: the twilight table, log2 Psi_ms for a sun below the local horizon.
  if (threadId.y >= args.multiscatteringLutSize) {
    const uint2 texel = uint2(threadId.x, threadId.y - args.multiscatteringLutSize);
    const vec2 twilightUv = (vec2(texel) + 0.5f) / float(args.multiscatteringLutSize);
    const float twilightMu = lerp(kMultiscatteringTwilightMuMin, kMultiscatteringTwilightMuMax,
                                  textureCoordToUnitRange(twilightUv.x, args.multiscatteringLutSize));
    const float twilightAltitude = textureCoordToUnitRange(twilightUv.y, args.multiscatteringLutSize)
                                 * args.atmosphereThickness;
    const vec3 psi = computeMultiscattering<kTwilightSqrtDirectionCount, kTwilightMarchStepCount>(
      twilightAltitude, twilightMu, args);
    multiscatteringLutOutput[threadId] = float4(log2(max(psi, vec3(kMultiscatteringTwilightFloor))), 1.0f);
    return;
  }

  // Top half: x = sun zenith cosine, y = altitude (section 5.5.2). The half-texel remap matches
  // sampleMultiscatteringLut.
  vec2 uv = (vec2(threadId) + 0.5f) / float(args.multiscatteringLutSize);

  float sunZenithCosAngle = textureCoordToUnitRange(uv.x, args.multiscatteringLutSize) * 2.0f - 1.0f;  // [-1, 1]
  float altitude = textureCoordToUnitRange(uv.y, args.multiscatteringLutSize) * args.atmosphereThickness;

  vec3 multiScatter = computeMultiscattering<kSqrtDirectionCount, kMarchStepCount>(altitude, sunZenithCosAngle, args);

  multiscatteringLutOutput[threadId] = float4(multiScatter, 1.0f);
}
