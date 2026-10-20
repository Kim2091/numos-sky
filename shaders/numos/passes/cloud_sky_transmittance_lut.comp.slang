// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Cloud sky-light LUT.
//
// A host's volumes and particles need the sky radiance arriving from each direction with clouds
// included: the clear sky dimmed by the clouds in front of it plus the light the clouds scatter
// toward the viewer. Clouds are a light source as well as a blocker; an overcast deck is usually
// brighter than the clear zenith it hides. Each texel stores
//   rgb: premultiplied cloud radiance (what the clouds add),
//   a:   cloud view transmittance (the fraction of the clear sky behind them that gets through),
// so a reader composites sky * a + rgb, as evalSkyRadiance does for secondary rays.
//
// The source is the secondary cloud dome LUT (cloud_secondary_lut.comp.slang), which marches the
// rendered cloud field with its lighting per direction from the camera. This pass reduces it onto
// the coarse cells here with solid-angle weights, 8x8 sub-directions per cell at mip 0, giving a
// prefiltered, ambient-scale version of what non-primary sky rays see. Cost: 256 above-horizon cells
// x 64 taps, once per frame after the dome bake.
//
// Parameterization matches sky_view_lut.comp.slang and sampleSkyAmbientForVolume:
//   uv.x: azimuth atan2(z, x) in [0, 2pi);  uv.y: l = 2v - 1, elevation = l*l*sign(l) * pi/2.
// Below-horizon cells hold (0, 1); readers only use the upper hemisphere.

#include "numos/lib/atmosphere_common.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1, rgba16f)
RWTexture2D<float4> cloudSkyTransmittanceLutOutput;

layout(binding = 2)
Texture2D<float4> AtmosphereCloudSecondaryLut;

layout(binding = 3)
SamplerState cloudDomeSampler;  // linear, clamp; sampleCloudDomeLut wraps azimuth itself

// Must match Renderer::Impl::kCloudSkyTransmittanceLutWidth/Height in src/renderer_impl.h.
static const uint kLutWidth = 32u;
static const uint kLutHeight = 16u;
static const uint kSubSamples = 8u;  // per axis, per cell

// Sky-view LUT encoding: uv -> Y-up direction (azimuth measured atan2(z, x)).
vec3 skyViewUvToDir(vec2 uv, out float elevation, out float l) {
  const float azimuth = uv.x * 2.0f * kPi;
  l = uv.y * 2.0f - 1.0f;
  elevation = l * l * sign(l) * (0.5f * kPi);
  const float cosEl = cos(elevation);
  return vec3(cosEl * cos(azimuth), sin(elevation), cosEl * sin(azimuth));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 threadId : SV_DispatchThreadID) {
  if (threadId.x >= kLutWidth || threadId.y >= kLutHeight) {
    return;
  }

  AtmosphereArgs args = atmosphereArgs;

  // Below the horizon, or with no cloud source, nothing sits in front of the clear sky. Without the
  // dome LUT (cloudSecondaryLutEnable off) there is no cloud field to reduce, and evalSkyRadiance's
  // non-primary path shows no clouds either, so the two agree. There is no clear-sky gate here: the
  // dome bake writes (0, 1) whenever it has nothing to march, so the reduction is exactly (0, 1)
  // too. A gate on cloudCoverageMean alone would drop a deck that exists only on layer 2.
  const bool upperHemisphere = (threadId.y + 1u) * 2u > kLutHeight;  // cell centre above v = 0.5
  if (!upperHemisphere || args.cloudSecondaryLutEnable == 0u) {
    cloudSkyTransmittanceLutOutput[threadId] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return;
  }

  // Solid-angle-weighted mean over the cell. dOmega = cos(el) d(el) d(az), and
  // d(el)/dv = 2 * pi * |l| under the quadratic encoding; constant factors cancel.
  // Alpha is reduced as opacity (1 - transmittance) and converted back at the end, so a clear cell
  // sums exact zeros and stores exactly 1. Averaging transmittance directly can leave the mean one
  // float ULP under 1, which the RGBA16F store may round toward zero to 1 - 2^-11.
  vec4 sum = vec4(0.0f);
  float weightSum = 0.0f;
  for (uint j = 0u; j < kSubSamples; ++j) {
    for (uint i = 0u; i < kSubSamples; ++i) {
      const vec2 uv = (vec2(threadId) + (vec2(float(i), float(j)) + 0.5f) / float(kSubSamples))
                    / vec2(kLutWidth, kLutHeight);
      float elevation, l;
      const vec3 dirYUp = skyViewUvToDir(uv, elevation, l);
      const float weight = cos(elevation) * abs(l);
      // Dome LUT: (premultiplied cloud rgb, view transmittance).
      const vec4 dome = sampleCloudDomeLut(AtmosphereCloudSecondaryLut, cloudDomeSampler,
                                           cloudDomeDirToUv(dirYUp), 0.0f);
      sum += weight * vec4(dome.rgb, 1.0f - dome.a);
      weightSum += weight;
    }
  }
  const vec4 cell = sum / max(weightSum, 1e-8f);

  cloudSkyTransmittanceLutOutput[threadId] = vec4(max(cell.rgb, vec3(0.0f)), saturate(1.0f - cell.a));
}
