// Copyright 2026 Kim2091 (Sparkles)
// Portions Copyright (c) 2025-2026 CattaRappa (sambow23), Owen Krisch
// SPDX-License-Identifier: Apache-2.0 AND MIT
// The portions are under the MIT licence (licenses/LICENSE-MIT-contributors.txt);
// see THIRD_PARTY_NOTICES.md.

// Sky-view LUT (Hillaire 2020, section 5.3): the clear sky's radiance over every direction from the
// camera, without clouds.

#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/night_sky.slangh"
#include "numos/lib/sky_view_lut_binding_indices.h"

layout(binding = SKY_VIEW_LUT_ATMOSPHERE_ARGS)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = SKY_VIEW_LUT_TRANSMITTANCE_INPUT)
Texture2D<float4> transmittanceLutInput;

layout(binding = SKY_VIEW_LUT_MULTISCATTERING_INPUT)
Texture2D<float4> multiscatteringLutInput;

layout(binding = SKY_VIEW_LUT_SAMPLER)
SamplerState lutSampler;

layout(binding = SKY_VIEW_LUT_OUTPUT, rgba16f)
RWTexture2D<float4> skyViewLutOutput;

// The starlight blurred to ~3 deg (night_sky.slangh): the LUT carries it through the air so the
// clouds' and the ground's sky light include it.
layout(binding = SKY_VIEW_LUT_STARLIGHT_LOW)
Texture2D<float4> starlightLowInput;

// The sky's radiance toward the direction of LUT texel uv.
vec3 computeSkyView(vec2 uv, AtmosphereArgs args,
                    Texture2D<float4> transmittanceLut,
                    SamplerState lutSampler,
                    Texture2D<float4> multiscatteringLut) {
  // The origin is the eye: getPlanetCenter places the planet planetRadius + cameraAltitudeKm below
  // it, so the bake follows the camera's altitude without an offset here.
  vec3 cameraPos = vec3(0.0f, 0.0f, 0.0f);

  // Non-linear elevation l * l * sign(l) puts most texels near the horizon, where the colour changes
  // fastest; sampleSkyViewLutForRay inverts it.
  float azimuth = uv.x * 2.0f * kPi;
  float l = uv.y * 2.0f - 1.0f;
  float elevation = l * l * sign(l) * kPi * 0.5f;  // [-pi/2, pi/2]

  float cosElevation = cos(elevation);
  float sinElevation = sin(elevation);
  vec3 viewDir = vec3(
    cosElevation * cos(azimuth),
    sinElevation,
    cosElevation * sin(azimuth)
  );
  viewDir = normalize(viewDir);

  vec3 sunDir = args.sunDirection;
  vec3 radiance = evalAtmosphereRadiance(
    cameraPos,
    viewDir,
    sunDir,
    args,
    transmittanceLut,
    lutSampler,
    multiscatteringLut,
    32  // numSamples
  );

  // Once the sky is dark enough for them to register (nightSkyFlags bit 3): the airglow and the
  // zodiacal light, and the starlight from beyond the air (bits 0, 1), extinguished along the ray.
  // Rays that reach the ground see only the airglow in front of it, from an eye above the layer.
  if ((args.nightSkyFlags & 8u) != 0u) {
    const float eyeRadius = args.planetRadius + max(args.cameraAltitudeKm, 0.0f);
    const float horizonMu = -sqrt(max(1.0f - (args.planetRadius / eyeRadius) * (args.planetRadius / eyeRadius), 0.0f));
    const bool reachesSpace = viewDir.y > horizonMu;
    const float b = eyeRadius * viewDir.y;
    const float groundDistance = reachesSpace
      ? 1e30f : -b - sqrt(max(b * b - eyeRadius * eyeRadius + args.planetRadius * args.planetRadius, 0.0f));
    const vec3 transmittance = reachesSpace
      ? sampleTransmittanceLut(transmittanceLut, lutSampler, max(args.cameraAltitudeKm, 0.0f), viewDir.y, args)
      : vec3(1.0f);
    vec3 beyondAir = evalNightSkyGlow(viewDir, eyeRadius, groundDistance, args);
    if (reachesSpace && (args.nightSkyFlags & 3u) == 3u) {
      const vec3 e = nightSkyLocalToEquatorial(viewDir, args);
      beyondAir += sampleNightSkyStarlightLow(e, starlightLowInput, lutSampler)
                 * args.sunIlluminance * (args.nightSkyGain / kNightSkyAssetScale);  // night gain
    }
    radiance += beyondAir * transmittance;
  }

  return radiance;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;

  if (threadId.x >= args.skyViewLutWidth || threadId.y >= args.skyViewLutHeight) {
    return;
  }

  vec2 uv = (vec2(threadId) + 0.5f) / vec2(args.skyViewLutWidth, args.skyViewLutHeight);

  vec3 skyColor = computeSkyView(uv, args, transmittanceLutInput, lutSampler, multiscatteringLutInput);

  skyViewLutOutput[threadId] = float4(skyColor, 1.0f);
}
