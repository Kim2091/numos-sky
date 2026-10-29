// Copyright 2026 Kim2091 (Sparkles)
// Portions Copyright (c) 2025-2026 CattaRappa (sambow23), softsoundd
// SPDX-License-Identifier: Apache-2.0 AND MIT
// The portions are under the MIT licence (licenses/LICENSE-MIT-contributors.txt);
// see THIRD_PARTY_NOTICES.md.

// NUMOS renderer. GPU work goes through PassRecorder/IGpu, options come from m_settings, and the
// host's scene, camera and lights arrive as FrameInputs.
#include "renderer_impl.h"
#include "blue_noise_128x128x64.h"
#include "celestial.h"
#include "night_gain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace numos {

Renderer::Impl::Impl() {
  const char* trace = std::getenv("RTX_NUMOS_TRACE_PLACEMENT");
  m_traceCloudPlacement = trace != nullptr && std::string(trace) == "1";
}

namespace {
  float computeCloudNvdfNominalCoverage(const Settings& settings, float cloudCoverageMean) {
    const float pinned = settings.nvdfNominalCoverage;
    const float autoNominal = std::min(std::max(cloudCoverageMean, 0.0f), 1.0f);
    return pinned > 0.0f ? pinned : autoNominal;
  }

  // The cloud type the body bake shapes clouds with, in steps of 0.1 so a weather transition
  // re-bakes only a handful of times. It drives a closed deck's cells and lobes
  // (cloudDeckBodyFieldKm) and a storm cumulus's lumpy sides and base (cloudCumulusBodyFieldKm).
  float cloudNvdfDeckType(float nominalCoverage, float cloudTypeMean) {
    (void)nominalCoverage;
    return std::round(std::min(std::max(cloudTypeMean, 0.0f), 1.0f) * 10.0f) / 10.0f;
  }

  // The body billows the bake shapes with, quantized like the type: weather presets set them, and a
  // transition moving them every frame would otherwise re-bake every frame.
  float cloudNvdfBillowSizeKm(float sizeKm) { return std::round(sizeKm / 0.1f) * 0.1f; }
  float cloudNvdfBillowGain(float gain) { return std::round(gain / 0.05f) * 0.05f; }
}

void Renderer::Impl::initialize(PassRecorder& rec) {
  if (m_initialized) {
    return;
  }

  createLutResources(rec);
  dispatchCloudPlacementMapBake(rec);
  cacheCloudPlacementBakeInputs();
  // One-shot wispy/billowy detail volume (fixed pattern).
  dispatchCloudDetailNoiseBake(rec);
  // Full synchronous NVDF bake at init; runtime re-bakes use the amortized state machine in computeLuts.
  runCloudNvdfBakeFull(rec);
  cacheCloudNvdfBakeInputs();
  m_initialized = true;
  m_lutsNeedRecompute = true;
}

namespace {
  // Zero per-frame animated fields that never feed any LUT bake, so the memcmp gate only fires on real changes.
  void normalizeForSkyLutCache(AtmosphereArgs& args) {
    args.timeSeconds                 = 0.0f;
    for (uint32_t m = 0; m < 2; ++m) {
      args.cloudShadowDirX[m] = args.cloudShadowDirY[m] = args.cloudShadowDirZ[m] = vec4(0.0f);
      args.cloudShadowCenterU[m] = args.cloudShadowHalfU[m] = vec4(0.0f);
      args.cloudShadowCenterV[m] = args.cloudShadowHalfV[m] = vec4(0.0f);
    }
    args.cloudShadowSlots            = 0u;
    args.cloudShadowLive             = 0u;
    args.cloudShadowBakeTarget       = 0u;
    args.cloudShadowBakeRow0         = 0u;
    args.cloudWindOffset             = vec2(0.0f, 0.0f);
    args.cloudWindStepXKm            = 0.0f;
    args.cloudWindStepZKm            = 0.0f;
    args.cloudLayer2FollowStepKm     = 0.0f;
    // The layer 2 patch-field map re-centres as the camera drifts over the clouds; re-centring does
    // not change the field it holds.
    args.cloudLayer2MapCenterX       = 0.0f;
    args.cloudLayer2MapCenterZ       = 0.0f;
    args.cloudLayer2MapValid         = 0.0f;
    args.cloudEvolutionOffsetX       = 0.0f;
    args.cloudEvolutionOffsetY       = 0.0f;
    // The detail drift follows the wind direction, so a preset that rotates the wind would
    // otherwise re-bake the whole sky LUT cascade every frame.
    args.cloudWindDirUnitX           = 0.0f;
    args.cloudWindDirUnitZ           = 0.0f;
    args.cloudRenderFrameIdx         = 0u;
    args.cameraWorldPosYUpKm         = vec3(0.0f, 0.0f, 0.0f);
    // The transmittance and multiscattering LUTs are parameterized by altitude internally
    // (uvToTransmittanceLutParams, sampleMultiscatteringLut), so they do not depend on the camera's
    // altitude. The sky-view key re-injects it quantized, since getEyeRadius uses it.
    args.cameraAltitudeKm            = 0.0f;
    // The aerial perspective block (froxel grid, far field, fog, local lights) is camera-fitted or
    // read only by its own passes and the composite; none of it feeds the transmittance,
    // multiscattering or sky-view bakes. Zeroing it here means a camera move or a lamp flicker
    // cannot re-bake the LUT cascade. isZUp is the one field of the block the rest of the frame
    // reads, so it is kept.
    {
      const uint32_t isZUp = args.isZUp;
      std::memset(&args.apFroxelSizeX, 0,
                  offsetof(AtmosphereArgs, apLocalLightTilesY) + sizeof(args.apLocalLightTilesY)
                  - offsetof(AtmosphereArgs, apFroxelSizeX));
      args.isZUp = isZUp;
    }
    // The celestial frame: only the sky-view bake reads it, and its key re-injects it quantized.
    args.celestialToLocalX           = vec3(0.0f, 0.0f, 0.0f);
    args.celestialToLocalY           = vec3(0.0f, 0.0f, 0.0f);
    args.celestialToLocalZ           = vec3(0.0f, 0.0f, 0.0f);
    args.eclipticPoleLocal           = vec3(0.0f, 0.0f, 0.0f);
    args.skyIndirectRadianceScale    = 0.0f;
    args.radianceScaleHistoryRatio   = 0.0f;  // one-frame transient; R itself is in sunIlluminance
    args.lightningStrikePosKm        = vec3(0.0f, 0.0f, 0.0f);
    args.lightningFlashIntensity     = 0.0f;
    args.lightningEnvelope           = 0.0f;
    args.lightningEnvelopePrevious   = 0.0f;
    // Interleave words carry this frame's phase; no bake reads them.
    args.cloudScreenInterleave         = 0u;
    args.cloudReprojectDepthTolerance  = 0.0f;
    args.cloudDomeInterleave           = 0u;
    // Detail LOD applies to the view march only; the bakes pass lod 0 themselves.
    args.cloudDetailLodBias            = 0.0f;
    args.cloudDetailLodEnable          = 0u;
    // The night sky's per-frame inputs: no bake reads them (the celestial frame is re-injected,
    // quantized, by the sky-view key when the starlight is baked).
    args.skyPixelAngle               = 0.0f;
    args.starScintillation           = 0.0f;
    // The debug view words sit outside the aerial perspective block; two move with the pre-exposure.
    args.apDebugNitsPerUnit          = 0.0f;
    args.apDebugView                 = 0u;
    args.apDebugRadianceScale        = 0.0f;
  }

  float quantizeDirComponent(float v, float stepRad) {
    return std::floor(v / stepRad + 0.5f) * stepRad;
  }

  // Quantizes sun/moon directions to skyViewRebakeGranularityDeg so continuous time-of-day motion
  // re-bakes only at granularity steps instead of every frame.
  void normalizeForSkyViewLutKey(AtmosphereArgs& args, const Settings& settings) {
    // Captured before normalizeForSkyLutCache zeroes them.
    const float camAltitudeKm = args.cameraAltitudeKm;
    const vec3 celestial[3] = { args.celestialToLocalX, args.celestialToLocalY, args.celestialToLocalZ };
    const vec3 eclipticPole = args.eclipticPoleLocal;
    normalizeForSkyLutCache(args);

    // Re-inject camera altitude, quantized. The sky-view bake places the eye at
    // planetRadius + cameraAltitudeKm (getEyeRadius, atmosphere_common.slangh), so climbing must
    // re-bake it, but at full precision any vertical motion would re-bake every frame. Quantizing to
    // skyViewAltitudeRebakeGranularityKm gives one re-bake per step of climb.
    const float altStepKm = settings.skyViewAltitudeRebakeGranularityKm;
    args.cameraAltitudeKm = altStepKm > 0.0f
      ? quantizeDirComponent(camAltitudeKm, altStepKm)
      : 0.0f;

    const float granularityDeg = settings.skyViewRebakeGranularityDeg;
    if (granularityDeg > 0.0f) {
      const float stepRad = granularityDeg * kDegreesToRadians;
      args.sunDirection.x = quantizeDirComponent(args.sunDirection.x, stepRad);
      args.sunDirection.y = quantizeDirComponent(args.sunDirection.y, stepRad);
      args.sunDirection.z = quantizeDirComponent(args.sunDirection.z, stepRad);
      for (uint32_t i = 0; i < MAX_MOONS; ++i) {
        args.moons[i].direction.x = quantizeDirComponent(args.moons[i].direction.x, stepRad);
        args.moons[i].direction.y = quantizeDirComponent(args.moons[i].direction.y, stepRad);
        args.moons[i].direction.z = quantizeDirComponent(args.moons[i].direction.z, stepRad);
      }
    }
    // The zodiacal light (nightSkyFlags bit 3) turns with the ecliptic, the baked starlight (bits 0
    // and 1) with the whole celestial frame; both key in the same angular steps as the sun.
    if ((args.nightSkyFlags & 8u) != 0u) {
      const float stepRad = std::max(granularityDeg, 0.01f) * kDegreesToRadians;
      args.eclipticPoleLocal = vec3(quantizeDirComponent(eclipticPole.x, stepRad),
                                    quantizeDirComponent(eclipticPole.y, stepRad),
                                    quantizeDirComponent(eclipticPole.z, stepRad));
    }
    if ((args.nightSkyFlags & 3u) == 3u) {
      const float stepRad = std::max(granularityDeg, 0.01f) * kDegreesToRadians;
      vec3* axes[3] = { &args.celestialToLocalX, &args.celestialToLocalY, &args.celestialToLocalZ };
      for (int k = 0; k < 3; ++k) {
        *axes[k] = vec3(quantizeDirComponent(celestial[k].x, stepRad), quantizeDirComponent(celestial[k].y, stepRad),
                        quantizeDirComponent(celestial[k].z, stepRad));
      }
    }
    // The sky bake reads a moon's direction, light and mask bit only; the light drifts with the
    // phase every frame, so it keys in ~2% steps.
    for (uint32_t i = 0; i < MAX_MOONS; ++i) {
      MoonParams& m = args.moons[i];
      const auto q = [](float v) { return v > 0.0f ? std::exp2(std::round(std::log2(v) * 32.0f) / 32.0f) : 0.0f; };
      m.illuminance = vec3(q(m.illuminance.x), q(m.illuminance.y), q(m.illuminance.z));
      m.northPole = vec3(0.0f, 0.0f, 0.0f);
      m.phaseAngle = 0.0f;
      m.albedoTint = vec3(0.0f, 0.0f, 0.0f);
      m.earthshine = 0.0f;
      m.angularRadius = 0.0f;
      m.primeMeridian = vec3(0.0f, 0.0f, 0.0f);
      m.sunlightDirection = vec3(0.0f, 0.0f, 0.0f);
      m.textureLayer = 0.0f;
    }
  }

  // Re-injects wind scroll + camera position (km-quantized) back into the sky-view key
  // so continuous motion re-bakes once per step, not every frame.
  void normalizeForVoxelGridKey(AtmosphereArgs& args, const Settings& settings) {
    const vec2 windKm = args.cloudWindOffset;
    const vec3 camKm  = args.cameraWorldPosYUpKm;
    const vec2  evoKm  = vec2(args.cloudEvolutionOffsetX, args.cloudEvolutionOffsetY);
    normalizeForSkyViewLutKey(args, settings);

    // Weather coverage drifts every frame. Bound cache-key error to half a 1/1024 step
    // instead of treating sub-per-mille density changes as a full-grid invalidation.
    args.cloudCoverageMean = quantizeDirComponent(args.cloudCoverageMean, 1.0f / 1024.0f);

    const float stepKm = std::max(settings.cloudVoxelGridRebakeGranularityKm, 1e-5f);
    args.cloudWindOffset.x     = quantizeDirComponent(windKm.x, stepKm);
    args.cloudWindOffset.y     = quantizeDirComponent(windKm.y, stepKm);
    // Horizontal: snap to a whole voxel with exactly the expression cloudVoxelGridOriginKm() uses
    // on the GPU (atmosphere_common.slangh): floor(), not round(), and the same step. It cannot
    // reuse stepKm: that defaults to 0.1 km while a voxel is cloudNoiseTileKm/256 (~47 m at 12 km),
    // and keying on the coarser step would let the GPU origin move to a new voxel without changing
    // the key, so the grid would be sampled at an origin the bake never produced. Keying on the
    // snapped origin changes the key exactly when the origin moves.
    //
    // kCloudVoxelGridResolutionXZ must match Renderer::Impl::kCloudVoxelGridX (private, hence the
    // local copy) and the shader-side constant of the same name.
    constexpr float kCloudVoxelGridResolutionXZ = 256.0f;
    const float voxelSizeXZ =
      std::max(settings.cloudNoiseTileKm / kCloudVoxelGridResolutionXZ, 1e-6f);
    args.cameraWorldPosYUpKm.x = std::floor(camKm.x / voxelSizeXZ) * voxelSizeXZ;
    args.cameraWorldPosYUpKm.z = std::floor(camKm.z / voxelSizeXZ) * voxelSizeXZ;
    // Vertical stays on the coarse granularity: the grid origin has no y term (the box's
    // vertical span is the slab, addressed by altitude), so .y only needs to re-key the
    // spherical vertical mapping as the eye climbs.
    args.cameraWorldPosYUpKm.y = quantizeDirComponent(camKm.y, stepKm);
    // A layer 2 that follows the camera climbs with it: same granularity.
    if (settings.cloudLayer2FollowCamera) {
      args.cloudLayer2Altitude = quantizeDirComponent(args.cloudLayer2Altitude, stepKm);
    }

    // The cloud animation is part of this key. The base normalizer zeroes the evolution offset
    // because the sky LUTs do not read it, but moving clouds must invalidate interleaved cloud work.
    // It is quantized on the same km step as wind and camera, so staleness stays bounded by one step.
    args.cloudEvolutionOffsetX = quantizeDirComponent(evoKm.x, stepKm);
    args.cloudEvolutionOffsetY = quantizeDirComponent(evoKm.y, stepKm);

  }

  // Interleave option value to period in frames.
  uint32_t cloudInterleavePeriod(int mode) {
    return mode <= 0 ? 1u : (mode == 1 ? 2u : 4u);
  }

  // Packs a period and this frame's phase the way the cloud shaders unpack it: bits 0-7 period,
  // bits 8-15 phase.
  uint32_t packCloudInterleave(uint32_t period, uint32_t frameIdx) {
    return (period & 0xFFu) | ((frameIdx % std::max(period, 1u)) << 8u);
  }

  // The sky LUT bakes read the clouds only through the analytical multiscattering fit's ground
  // term (getTransmittanceToSun -> evalCloudGroundShadow), which the physical-LUT blend at
  // multiScatterPhysicalStrength >= 1 (the default) multiplies by zero. In that case the cloud
  // fields a weather transition moves every frame must not key the T + MS + sky-view cascade.
  void zeroCloudFieldsForSkyKeys(AtmosphereArgs& args, float multiScatterPhysicalStrength) {
    if (multiScatterPhysicalStrength < 1.0f) {
      return;
    }
    args.cloudEnabled = 0.0f;
    args.cloudCoverageMean = 0.0f;
    args.cloudCoverageSpread = 0.0f;
    args.cloudCoverageNoiseScale = 0.0f;
    args.cloudTypeMean = 0.0f;
    args.cloudTypeSpread = 0.0f;
    args.cloudTypeNoiseScale = 0.0f;
    args.cloudDensity = 0.0f;
    args.cloudAltitude = 0.0f;
    args.cloudThickness = 0.0f;
    args.cloudShadowStrength = 0.0f;
    args.nvdfNominalCoverage = 0.0f;
  }

  // Air and aerosol densities drift every frame of a weather transition; key them in ~1% steps
  // (2^(1/64)) so the T + MS + sky-view cascade re-bakes per step, not per frame. Between re-bakes
  // the LUTs lag the live medium by under 1%.
  void quantizeMediumForSkyKeys(AtmosphereArgs& args) {
    const auto q = [](float v) { return v > 0.0f ? std::exp2(std::round(std::log2(v) * 64.0f) / 64.0f) : 0.0f; };
    const auto q3 = [&](vec3 v) { return vec3(q(v.x), q(v.y), q(v.z)); };
    args.rayleighScattering = q3(args.rayleighScattering);
    args.mieScattering = q3(args.mieScattering);
    args.mieAbsorption = q3(args.mieAbsorption);
  }

  // Neither transmittance nor multiscatter reads the sun direction or any moon field, so a moving
  // sun re-bakes only the sky-view LUT, not the heavy multiscatter dispatch.
  void normalizeForTransmittanceMsKey(AtmosphereArgs& args, const Settings& settings) {
    normalizeForSkyViewLutKey(args, settings);

    args.sunDirection                 = vec3(0.0f, 0.0f, 0.0f);
    args.sunIlluminance               = vec3(0.0f, 0.0f, 0.0f);
    args.sunAngularRadius             = 0.0f;
    args.mieAnisotropy                = 0.0f;
    args.multiScatterPhysicalStrength = 0.0f;
    // Both bakes are functions of altitude alone (neither reads the camera's), so climbing must not
    // re-bake them; normalizeForSkyViewLutKey re-injected it for the sky-view bake only.
    args.cameraAltitudeKm             = 0.0f;

    args.moonSkyLightMask = 0u;
    memset(&args.moons[0], 0, sizeof(args.moons));
    // The night sky lives in the sky-view bake only.
    args.nightSkyFlags = 0u;
    args.airglowEmission = vec3(0.0f, 0.0f, 0.0f);
    args.airglowAltitudeKm = 0.0f;
    args.airglowThicknessKm = 0.0f;
    args.zodiacalLightScale = 0.0f;
    args.nightGlowInScatter = vec3(0.0f, 0.0f, 0.0f);
    args.eclipticPoleLocal = vec3(0.0f, 0.0f, 0.0f);
    args.celestialToLocalX = vec3(0.0f, 0.0f, 0.0f);
    args.celestialToLocalY = vec3(0.0f, 0.0f, 0.0f);
    args.celestialToLocalZ = vec3(0.0f, 0.0f, 0.0f);
  }

  // NUMOS's illuminance unit in lux (FrameOutputs::luxPerUnit): sunIlluminance x sunIntensity is the
  // real sun's 1.27e5 lux, the calibration lightningIntensityKm2 and the night sky use.
  float luxPerUnit(const Settings& settings) {
    constexpr float kSunLux = 1.27e5f;
    const Vector3 sun = settings.sunIlluminance * settings.sunIntensity;
    const float sunLuminance = 0.2126f * sun.x + 0.7152f * sun.y + 0.0722f * sun.z;
    return sunLuminance > 0.0f ? kSunLux / sunLuminance : 1.0f;
  }
} // anonymous namespace

void Renderer::Impl::advanceTimeCycle(float dt) {
  // Re-seed whenever the authored option moves, so scrubbing the slider (or loading a config) sets
  // the clock rather than the clock immediately overwriting the edit. The sentinel initial value
  // guarantees a seed on the first frame.
  const float authored = m_settings.timeOfDayHours;
  if (authored != m_lastAuthoredTimeOfDayHours) {
    m_timeOfDayHours = authored;
    m_lastAuthoredTimeOfDayHours = authored;
  }

  if (!m_settings.timeCycleEnable) {
    // Frozen: the authored value is the time of day, so the option doubles as a manual control.
    m_timeOfDayHours = authored;
    return;
  }

  const float dayLengthSeconds = std::max(m_settings.dayLengthMinutes, 0.01f) * 60.0f;
  m_timeOfDayHours += (std::max(dt, 0.0f) / dayLengthSeconds) * 24.0f;

  // Wrap into [0, 24). fmod alone can return a negative for a negative input, hence the double fold.
  m_timeOfDayHours = std::fmod(std::fmod(m_timeOfDayHours, 24.0f) + 24.0f, 24.0f);
}

void Renderer::Impl::computeTimeCycleSunAngles(float timeOfDayHours, float& outElevationDeg, float& outAzimuthDeg) const {
  // Standard solar position model. Declination from the day of year (Cooper's approximation), then
  // elevation and azimuth from the hour angle and observer latitude.
  const float latitudeRad = m_settings.latitudeDegrees * kDegreesToRadians;
  const float declinationRad = 23.44f * kDegreesToRadians
    * std::sin(2.0f * kPi * (284.0f + float(m_settings.dayOfYear)) / 365.0f);

  // Hour angle: 0 at solar noon, 15 degrees per hour, negative before noon.
  const float hourAngleRad = (timeOfDayHours - 12.0f) * 15.0f * kDegreesToRadians;

  const float sinLat = std::sin(latitudeRad);
  const float cosLat = std::cos(latitudeRad);
  const float sinDec = std::sin(declinationRad);
  const float cosDec = std::cos(declinationRad);

  const float sinElevation = std::min(std::max(
    sinLat * sinDec + cosLat * cosDec * std::cos(hourAngleRad), -1.0f), 1.0f);
  const float elevationRad = std::asin(sinElevation);

  // Azimuth measured from north, increasing clockwise (east = 90). The denominator collapses at the
  // poles and at the exact zenith, where azimuth is undefined and any value renders identically.
  const float cosElevation = std::cos(elevationRad);
  const float denom = cosElevation * cosLat;
  float azimuthRad;
  if (std::abs(denom) < 1e-6f) {
    azimuthRad = 0.0f;
  } else {
    const float cosAzimuth = std::min(std::max((sinDec - sinElevation * sinLat) / denom, -1.0f), 1.0f);
    azimuthRad = std::acos(cosAzimuth);
    // acos only resolves [0, 180]; afternoon (positive hour angle) is the western mirror.
    if (hourAngleRad > 0.0f) {
      azimuthRad = 2.0f * kPi - azimuthRad;
    }
  }

  outElevationDeg = elevationRad / kDegreesToRadians;
  outAzimuthDeg = azimuthRad / kDegreesToRadians + m_settings.northOffsetDegrees;
}

void Renderer::Impl::resolveSunAngles(float& outElevationDeg, float& outAzimuthDeg) const {
  outElevationDeg = m_settings.sunElevation;
  outAzimuthDeg   = m_settings.sunRotation;
  if (m_settings.timeCycleEnable) {
    computeTimeCycleSunAngles(m_timeOfDayHours, outElevationDeg, outAzimuthDeg);
  }
}

namespace {
  // Zenith sky luminance relative to daylight's, for sun elevation h (deg): a fit to measured
  // twilight curves (about 2e-3 at -6, 4e-6 at -12, 1e-8 at -18). Only steers the pre-exposure.
  float relativeSkyLuminance(float elevationDeg) {
    return elevationDeg >= 0.0f ? 1.0f : std::pow(10.0f, 0.45f * std::max(elevationDeg, -30.0f));
  }
}

// The night sky's geometry for this frame (celestial.h): the frame the stars turn with and every
// moon's direction, phase and light. The frame puts the sun's catalogue position for the configured
// date exactly on the sun as drawn (time cycle or placed by hand), so the stars always agree with the
// sun on screen (celestial::alignedFrame).
void Renderer::Impl::updateCelestial() {
  float sunElevationDeg, sunAzimuthDeg;
  resolveSunAngles(sunElevationDeg, sunAzimuthDeg);
  const float se = sunElevationDeg * kDegreesToRadians, sa = sunAzimuthDeg * kDegreesToRadians;
  const Vector3 sunDir(std::cos(se) * std::sin(sa), std::sin(se), std::cos(se) * std::cos(sa));
  const float latitude = m_settings.latitudeDegrees * kDegreesToRadians;
  const float northOffset = m_settings.northOffsetDegrees * kDegreesToRadians;

  const double days = celestial::daysSinceJ2000(m_settings.calendarYear, m_settings.dayOfYear, m_timeOfDayHours);
  double sunLongitude, obliquity;
  celestial::sunEcliptic(days, sunLongitude, obliquity);
  const Vector3 sunEq = celestial::eclipticToEquatorial(sunLongitude, 0.0, obliquity);
  m_celestialFrame = celestial::alignedFrame(sunEq, sunDir, celestial::celestialPoleLocal(latitude, northOffset));
  m_eclipticPoleLocal = m_celestialFrame.toLocal(
    Vector3(0.0f, -float(std::sin(obliquity)), float(std::cos(obliquity))));

  // A moon's light from its direction and the sun's. The phase angle (sun-moon-observer) is pi minus
  // the elongation, to the 0.15 deg the moon's finite distance shifts it. `sunlight` is the direction
  // of the sun as the moon sees it: the true sun's, or one a game moon's phase setting places (then
  // there is no real sun-earth-moon alignment, so no eclipse).
  const Vector3 lunar = celestial::lunarColour();
  auto light = [&](uint32_t slot, const Vector3& dir, const Vector3& geocentricDir, float distance,
                   float drawnDiameterDeg, const Vector3& tint, const Vector3& pole, const Vector3& primeMeridian,
                   int32_t textureLayer, const Vector3& sunlight, bool canEclipse) {
    MoonParams& m = m_moons[slot];
    const float phaseAngle = kPi - std::acos(std::min(std::max(dot(sunlight, dir), -1.0f), 1.0f));
    const float geocentricPhase = kPi - std::acos(std::min(std::max(dot(sunlight, geocentricDir), -1.0f), 1.0f));
    const float phaseLaw = celestial::moonIlluminanceFraction(phaseAngle);
    const float sunVisible = canEclipse ? celestial::sunVisibleFromMoon(geocentricPhase, distance) : 1.0f;
    const float fraction = phaseLaw * sunVisible;
    m.direction = dir;
    m.angularRadius = 0.5f * std::max(drawnDiameterDeg, 0.01f) * kDegreesToRadians;
    m.illuminance = Vector3(lunar.x * tint.x, lunar.y * tint.y, lunar.z * tint.z) * fraction;
    m.enabled = 1.0f;
    m.northPole = pole;
    m.phaseAngle = phaseAngle;
    m.albedoTint = tint;
    m.earthshine = fraction > 0.0f ? celestial::earthshineFraction(phaseAngle, distance) / fraction : 0.0f;
    m.primeMeridian = primeMeridian;
    m.textureLayer = textureLayer >= 0 && nightSkyMoonReady() ? float(textureLayer) : -1.0f;
    m.sunlightDirection = sunlight;
    // The sunlight the Earth hides in an eclipse; getAtmosphereArgs adds back what its air refracts in.
    m_moonUmbra[slot] = Vector3(lunar.x * tint.x, lunar.y * tint.y, lunar.z * tint.z) * (phaseLaw * (1.0f - sunVisible));
  };

  memset(m_moons, 0, sizeof(m_moons));
  for (Vector3& u : m_moonUmbra) {
    u = Vector3(0.0f, 0.0f, 0.0f);
  }
  uint32_t count = 0;
  if (m_settings.realMoonEnabled) {
    double lon, lat, distance;
    celestial::moonEcliptic(days, lon, lat, distance);
    const Vector3 geocentric = m_celestialFrame.toLocal(celestial::eclipticToEquatorial(lon, lat, obliquity));
    // Topocentric: the observer stands one Earth radius above the centre, along the local up.
    const Vector3 topocentric = normalize(geocentric * float(distance) - Vector3(0.0f, 1.0f, 0.0f));
    Vector3 pole, primeMeridian;
    celestial::moonAxes(days, obliquity, pole, primeMeridian);
    light(count++, topocentric, geocentric, float(distance), m_settings.realMoonAngularDiameter,
          Vector3(1.0f, 1.0f, 1.0f), m_celestialFrame.toLocal(pole), m_celestialFrame.toLocal(primeMeridian),
          m_nightSky.moonLayers[0], sunDir, true);
  }
  // Local east (azimuth northOffset + 90): a game moon's set phase lights its western limb while
  // waxing (the sun lies west of it) and its eastern one while waning, as the Moon's do.
  const Vector3 east(std::cos(northOffset), 0.0f, -std::sin(northOffset));
  for (uint32_t i = 0; i < kMaxMoons && count < MAX_MOONS; ++i) {
    const MoonSettings& ms = m_settings.moons[i];
    if (!ms.enabled) {
      continue;
    }
    const float e = ms.elevation * kDegreesToRadians, a = ms.rotation * kDegreesToRadians;
    const Vector3 dir(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
    // A game moon shows the centre of its map (longitude and latitude 0) to the observer, its north
    // toward the ecliptic pole's side of the sky.
    const Vector3 primeMeridian = dir * -1.0f;
    Vector3 pole = m_eclipticPoleLocal - dir * dot(m_eclipticPoleLocal, dir);
    if (dot(pole, pole) < 1e-8f) {
      pole = Vector3(0.0f, 1.0f, 0.0f) - dir * dir.y;
    }
    pole = dot(pole, pole) > 1e-8f ? normalize(pole) : Vector3(0.0f, 0.0f, 1.0f);
    // Its phase setting (0 new, 0.5 full) places the sun it sees at elongation 2 pi x phase from it,
    // to the west of it while waxing; negative = the true sun.
    Vector3 sunlight = sunDir;
    bool canEclipse = true;
    if (ms.phase >= 0.0f) {
      const float p = ms.phase - std::floor(ms.phase);
      const float elongation = 2.0f * kPi * std::min(p, 1.0f - p);
      const Vector3 side = p < 0.5f ? east * -1.0f : east;
      Vector3 t = side - dir * dot(side, dir);
      if (dot(t, t) < 1e-6f) {
        t = Vector3(0.0f, 1.0f, 0.0f) - dir * dir.y;
      }
      t = dot(t, t) > 1e-8f ? normalize(t) : Vector3(0.0f, 1.0f, 0.0f);
      sunlight = normalize(dir * std::cos(elongation) + t * std::sin(elongation));
      canEclipse = false;
    }
    light(count++, dir, dir, float(celestial::kMeanMoonDistanceEarthRadii), ms.angularDiameter, ms.color,
          pole, primeMeridian, m_nightSky.moonLayers[1 + i], sunlight, canEclipse);
  }

  // Moons whose light the sky bake integrates: those not lost under the sun's sky. Daylight is ~1e6
  // brighter than the full moon's sky, below fp16's resolution of it, so it stays bit-identical.
  m_moonSkyLightMask = 0u;
  for (uint32_t i = 0; i < count; ++i) {
    const Vector3& f = m_moons[i].illuminance;
    if (0.2126f * f.x + 0.7152f * f.y + 0.0722f * f.z > 1e-4f * relativeSkyLuminance(sunElevationDeg)) {
      m_moonSkyLightMask |= 1u << i;
    }
  }

  // The clouds' key light (getCloudArgs): the sun while it can still reach a visible cloud, then the
  // brightest moon that lights the sky. A cloud top at height h, seen out to its own horizon distance
  // sqrt(2 R h), is sunlit until the sun sinks 2 sqrt(2 h / R) below the observer's horizon (~4 deg
  // for a 3.8 km top). Past that the planet shadows every visible cloud, so switching to the moon
  // loses nothing. The only light left out is the moon's during the sun's last minutes, well under
  // 1% of the twilight sky on the clouds.
  {
    const float topKm = std::max(m_settings.cloudBaseHeightMeters * 0.001f + m_settings.cloudDepthMeters * 0.001f, 0.01f);
    const float reachDeg = 2.0f * std::sqrt(2.0f * topKm / std::max(m_settings.planetRadius, 1.0f)) / kDegreesToRadians;
    m_cloudKeyLight = 0u;
    if (sunElevationDeg < -reachDeg) {
      float best = 0.0f;
      for (uint32_t i = 0; i < count; ++i) {
        const MoonParams& m = m_moons[i];
        const float elevationDeg = std::asin(std::min(std::max(m.direction.y, -1.0f), 1.0f)) / kDegreesToRadians;
        const float lum = 0.2126f * m.illuminance.x + 0.7152f * m.illuminance.y + 0.0722f * m.illuminance.z;
        if (((m_moonSkyLightMask >> i) & 1u) != 0u && elevationDeg > -reachDeg && lum > best) {
          best = lum;
          m_cloudKeyLight = i + 1u;
        }
      }
    }
  }
}

// The args the cloud passes (march, dome LUT, shadow maps) and the aerial perspective see: the key
// light (updateCelestial) in the sun's slot, so they light with the moon exactly as with the sun:
// phase octaves, shadow map, planet shadow, transmittance. Everything else (the sky, the ground, the
// host) reads getAtmosphereArgs() with the real sun.
AtmosphereArgs Renderer::Impl::getCloudArgs() const {
  AtmosphereArgs args = getAtmosphereArgs();
  if (m_cloudKeyLight != 0u) {
    const MoonParams& m = args.moons[m_cloudKeyLight - 1u];
    args.sunDirection = m.direction;
    args.sunIlluminance = m.illuminance;
    args.sunAngularRadius = 0.2605f * kDegreesToRadians;
  }
  return args;
}

namespace {
  // A night sky asset's header: a 4-character magic, a version, then format-specific words.
  bool nightSkyHeader(const void* data, size_t size, const char* magic, uint32_t words, uint32_t* out) {
    if (data == nullptr || size < 8 + 4 * words || std::memcmp(data, magic, 4) != 0) {
      return false;
    }
    uint32_t version = 0;
    std::memcpy(&version, static_cast<const uint8_t*>(data) + 4, 4);
    std::memcpy(out, static_cast<const uint8_t*>(data) + 8, 4 * words);
    return version == 1u;
  }
  std::vector<uint8_t> bytesOf(const void* data, size_t offset, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data) + offset;
    return std::vector<uint8_t>(p, p + size);
  }
}

namespace {
  struct MoonMap { uint32_t width, height; const uint8_t* texels; };

  // Must match kMoonAlbedoRange in atmosphere_sky.slangh and tools/build_nightsky_assets.py.
  constexpr float kMoonAlbedoRange = 3.5f;

  // The moon atlas: every map resampled (bilinear) to one power-of-two 2:1 band, the first map's
  // size or below, stacked top to bottom. Each band's colour is scaled so its near side (longitude
  // -90..90, weighted as a full disk shows it) averages 1 / kMoonAlbedoRange: the moon's measured
  // brightness and colour stay in its light, and a map, the real one or a game's, only says where
  // its surface is brighter or darker. The alpha (height) is kept. Mips are box-filtered within each
  // band so no band bleeds into the next.
  void buildMoonAtlas(Renderer::Impl::NightSkyData& d, const MoonMap* maps, size_t count) {
    uint32_t bandHeight = 1;
    while (bandHeight * 4 <= maps[0].width) bandHeight *= 2;
    const uint32_t width = bandHeight * 2;
    std::vector<float> level(size_t(width) * bandHeight * count * 4);
    for (size_t l = 0; l < count; ++l) {
      const MoonMap& m = maps[l];
      float* band = &level[l * width * bandHeight * 4];
      auto texel = [&](int x, int y, int c) {
        x = ((x % int(m.width)) + int(m.width)) % int(m.width);
        y = std::min(std::max(y, 0), int(m.height) - 1);
        return float(m.texels[(size_t(y) * m.width + x) * 4 + c]) * (1.0f / 255.0f);
      };
      double sum[3] = {}, weightSum = 0.0;
      for (uint32_t y = 0; y < bandHeight; ++y) {
        const float sy = (float(y) + 0.5f) * float(m.height) / float(bandHeight) - 0.5f;
        const int y0 = int(std::floor(sy));
        const float fy = sy - float(y0);
        const double lat = (0.5 - (y + 0.5) / bandHeight) * kPi;
        for (uint32_t x = 0; x < width; ++x) {
          const float sx = (float(x) + 0.5f) * float(m.width) / float(width) - 0.5f;
          const int x0 = int(std::floor(sx));
          const float fx = sx - float(x0);
          float* out = &band[(size_t(y) * width + x) * 4];
          for (int c = 0; c < 4; ++c) {
            out[c] = (texel(x0, y0, c) * (1.0f - fx) + texel(x0 + 1, y0, c) * fx) * (1.0f - fy)
                   + (texel(x0, y0 + 1, c) * (1.0f - fx) + texel(x0 + 1, y0 + 1, c) * fx) * fy;
          }
          const double lon = ((x + 0.5) / width - 0.5) * 2.0 * kPi;
          if (std::abs(lon) < 0.5 * kPi) {
            const double weight = std::cos(lat) * std::cos(lat) * std::cos(lon);
            for (int c = 0; c < 3; ++c) sum[c] += out[c] * weight;
            weightSum += weight;
          }
        }
      }
      for (int c = 0; c < 3; ++c) {
        const float scale = sum[c] > 0.0 ? float(weightSum / (sum[c] * kMoonAlbedoRange)) : 0.0f;
        for (size_t t = 0; t < size_t(width) * bandHeight; ++t) band[t * 4 + c] *= scale;
      }
    }

    d.moonWidth = width;
    d.moonHeight = bandHeight * uint32_t(count);
    d.moonMips.clear();
    uint32_t w = width, h = bandHeight * uint32_t(count);
    for (;;) {
      std::vector<uint8_t> bytes(level.size());
      for (size_t i = 0; i < level.size(); ++i) {
        bytes[i] = uint8_t(std::min(std::max(level[i], 0.0f), 1.0f) * 255.0f + 0.5f);
      }
      d.moonMips.push_back(std::move(bytes));
      if (h / uint32_t(count) <= 1u) break;
      const uint32_t nw = w / 2, nh = h / 2;  // band heights halve exactly (powers of two)
      std::vector<float> next(size_t(nw) * nh * 4);
      for (uint32_t y = 0; y < nh; ++y)
        for (uint32_t x = 0; x < nw; ++x)
          for (int c = 0; c < 4; ++c) {
            const float* r0 = &level[(size_t(2 * y) * w + 2 * x) * 4 + c];
            const float* r1 = r0 + size_t(w) * 4;
            next[(size_t(y) * nw + x) * 4 + c] = 0.25f * (r0[0] + r0[4] + r1[0] + r1[4]);
          }
      level.swap(next);
      w = nw;
      h = nh;
    }
  }
}

bool Renderer::Impl::setNightSkyAssets(const NightSkyAssets& assets) {
  NightSkyData d;
  bool ok = true;
  uint32_t w[3] = {};
  if (assets.stars != nullptr) {
    // starCount, cellsPerFace, recordCount; then the cell offsets and the 32-byte records.
    if (nightSkyHeader(assets.stars, assets.starsSize, "NSTR", 3, w) && w[1] > 0) {
      const size_t cells = (6u * size_t(w[1]) * w[1] + 1u) * 4u;
      const size_t records = size_t(w[2]) * 32u;
      if (assets.starsSize >= 20 + cells + records) {
        d.cellsPerFace = w[1];
        d.starCells = bytesOf(assets.stars, 20, cells);
        d.starRecords = bytesOf(assets.stars, 20 + cells, records);
      } else ok = false;
    } else ok = false;
  }
  if (assets.milkyWay != nullptr) {
    if (nightSkyHeader(assets.milkyWay, assets.milkyWaySize, "NMWY", 2, w)
        && assets.milkyWaySize >= 16 + size_t(w[0]) * w[1] * 4u) {
      d.milkyWayWidth = w[0];
      d.milkyWayHeight = w[1];
      d.milkyWay = bytesOf(assets.milkyWay, 16, size_t(w[0]) * w[1] * 4u);
    } else ok = false;
  }
  if (assets.starlightLow != nullptr) {
    if (nightSkyHeader(assets.starlightLow, assets.starlightLowSize, "NSLO", 2, w)
        && assets.starlightLowSize >= 16 + size_t(w[0]) * w[1] * 8u) {
      d.lowWidth = w[0];
      d.lowHeight = w[1];
      d.starlightLow = bytesOf(assets.starlightLow, 16, size_t(w[0]) * w[1] * 8u);
    } else ok = false;
  }
  // The moon maps: the Moon's, then any game moon's own, each a band of one atlas.
  {
    using Map = MoonMap;
    auto parse = [&](const void* data, size_t size, Map& map) {
      uint32_t h[2] = {};
      if (data == nullptr) return false;
      if (nightSkyHeader(data, size, "NMON", 2, h) && h[0] >= 2 && h[1] >= 1 && size >= 16 + size_t(h[0]) * h[1] * 4u) {
        map = { h[0], h[1], static_cast<const uint8_t*>(data) + 16 };
        return true;
      }
      ok = false;
      return false;
    };
    std::vector<Map> maps;
    Map map {};
    if (parse(assets.moon, assets.moonSize, map)) {
      maps.push_back(map);
      d.moonLayers[0] = 0;
    }
    for (uint32_t i = 0; i < kMaxMoons; ++i) {
      d.moonLayers[1 + i] = d.moonLayers[0];
      if (parse(assets.gameMoons[i], assets.gameMoonSizes[i], map)) {
        d.moonLayers[1 + i] = int32_t(maps.size());
        maps.push_back(map);
      }
    }
    if (!maps.empty()) {
      buildMoonAtlas(d, maps.data(), maps.size());
    }
  }
  if (!ok) {
    logWarn("[NUMOS] setNightSkyAssets: some night sky data did not parse (wrong file or version); "
            "regenerate with tools/build_nightsky_assets.py");
  }
  d.generation = m_nightSky.generation + 1;
  m_nightSky = std::move(d);
  return ok;
}

// Streams the night sky data to the GPU, at most a few MB per frame so a host's per-frame staging
// stays small: buffers in chunks, then the two textures (one per frame).
void Renderer::Impl::uploadNightSky(PassRecorder& rec) {
  NightSkyData& d = m_nightSky;
  if (d.generation == 0 || d.uploadedGeneration == d.generation) {
    return;
  }
  IGpu& gpu = rec.gpu();
  if (d.uploadingGeneration != d.generation) {
    d.uploadingGeneration = d.generation;
    d.cursor = 0;
    auto buffer = [&](Buffer& slot, const char* name, size_t size) {
      if (size > 0) replaceBuffer(rec, slot, createBuffer(rec, name, BufferUsage::Structured, size));
    };
    buffer(m_nightSkyStarCells, "NUMOS Night Sky Star Cells", d.starCells.size());
    buffer(m_nightSkyStarRecords, "NUMOS Night Sky Stars", d.starRecords.size());
    buffer(m_nightSkyMilkyWay, "NUMOS Night Sky Milky Way", d.milkyWay.size());
    if (!d.starlightLow.empty()) {
      replaceTexture(rec, m_nightSkyStarlightLow, createTexture(rec, "NUMOS Night Sky Starlight", TextureType::Tex2D,
                                                                Format::RGBA16Float, d.lowWidth, d.lowHeight, 1, 1));
    }
    if (!d.moonMips.empty()) {
      replaceTexture(rec, m_nightSkyMoon, createTexture(rec, "NUMOS Night Sky Moon", TextureType::Tex2D,
                                                        Format::RGBA8Unorm, d.moonWidth, d.moonHeight, 1,
                                                        uint32_t(d.moonMips.size())));
    }
  }

  // One running cursor over [cells | records | milky way | low texture | moon mips].
  constexpr size_t kBudget = size_t(5) << 19;  // 2.5 MB per frame
  constexpr size_t kChunk = size_t(1) << 20;
  size_t budget = kBudget;
  struct Piece { const std::vector<uint8_t>* bytes; Buffer* buffer; Texture* texture; uint32_t mip; };
  std::vector<Piece> pieces = {
    { &d.starCells, &m_nightSkyStarCells, nullptr, 0 }, { &d.starRecords, &m_nightSkyStarRecords, nullptr, 0 },
    { &d.milkyWay, &m_nightSkyMilkyWay, nullptr, 0 }, { &d.starlightLow, nullptr, &m_nightSkyStarlightLow, 0 } };
  for (uint32_t mip = 0; mip < uint32_t(d.moonMips.size()); ++mip) {
    pieces.push_back({ &d.moonMips[mip], nullptr, &m_nightSkyMoon, mip });
  }
  size_t base = 0;
  for (const Piece& p : pieces) {
    const size_t size = p.bytes->size();
    while (d.cursor < base + size && budget > 0) {
      const size_t at = d.cursor - base;
      if (p.texture != nullptr) {
        if (budget < size && budget != kBudget) {
          return;  // a texture goes up whole: wait for a fresh frame's budget
        }
        gpu.uploadTexture(p.texture->handle, p.mip, p.bytes->data(), size);
        d.cursor = base + size;
        budget = budget > size ? budget - size : 0;
      } else {
        const size_t n = std::min({ kChunk, budget, size - at });
        gpu.updateBuffer(p.buffer->handle, at, n, p.bytes->data() + at);
        d.cursor += n;
        budget -= n;
      }
    }
    base += size;
    if (d.cursor < base) {
      return;
    }
  }
  d.uploadedGeneration = d.generation;
}

// Pre-exposure for the frame (rtx.atmosphere.radianceAutoScale). The night sky is ~1e-8 of the day
// sky, below the smallest fp16 value, so every 16-bit target NUMOS writes (sky-view LUT, cloud RT,
// dome LUT) would flush it. All light sources are multiplied by R = 2^n chosen so the brightest sky
// lands near daylight levels; outputs are physical x R. n = 0 whenever the sun is up, so daylight is
// unchanged, and R moves in whole powers of two so nothing rounds. Brightening steps down at once
// (overflow), darkening steps up a level late (no flicker on a boundary).
namespace {
  // The lightning glow at the strike's core per unit of flash intensity (evalLightningFlash: N / c^2,
  // the shader's kFlashEnergyNorm / kFlashCoreSoftKm2), per km^2: its brightest radiance.
  constexpr float kLightningCoreGlowPerKm2 = 0.2276f / 0.25f;
}

void Renderer::Impl::updateRadianceScale() {
  m_prevRadianceScale = m_radianceScale;
  int32_t target = 0;
  {
    float sunElevationDeg, sunAzimuthDeg;
    resolveSunAngles(sunElevationDeg, sunAzimuthDeg);

    // The natural night sky (airglow, zodiacal light, starlight) relative to daylight's.
    constexpr float kNightSkyRelative = 6e-8f;
    const float twilight = relativeSkyLuminance(sunElevationDeg);
    float night = kNightSkyRelative;
    for (uint32_t i = 0; i < MAX_MOONS; ++i) {
      const MoonParams& m = m_moons[i];
      if (m.enabled < 0.5f) {
        continue;
      }
      const float fraction = 0.2126f * m.illuminance.x + 0.7152f * m.illuminance.y + 0.0722f * m.illuminance.z;
      night += fraction
             * relativeSkyLuminance(std::asin(std::min(std::max(m.direction.y, -1.0f), 1.0f)) / kDegreesToRadians);
    }
    // Night gain (night_gain.h): the natural night light x nightGain, faded in through twilight. Everything
    // keyed to the sky's level below (this pre-exposure, where a lightning flash ends) sees the gained sky.
    m_nightGain = nightGainInForce(twilight, night, m_settings.nightGainStops);
    float relative = twilight + night * m_nightGain;
    m_skyRelativeLuminance = relative;  // also where a lightning flash ends (advanceLightning)
    if (!m_settings.radianceAutoScale) {
      relative = 1.0f;
    }
    // A lightning flash outshines a night sky ~1e8 times, and at the night's scale its glow would
    // overflow the 16-bit targets. While a flash lasts the scale keeps its peak (the glow at the
    // strike's core at full envelope) at or below kFlashPreExposed, so it steps down once at the
    // strike and back up once after, not with every flicker.
    if (m_settings.radianceAutoScale && m_settings.lightningEnable && m_lightningEnvelope > 0.0f) {
      constexpr float kFlashPreExposed = 256.0f;    // 1/256 of fp16's range: room for the scene light
      relative += lightningIntensityKm2() * kLightningCoreGlowPerKm2 / kFlashPreExposed;
    }
    target = std::min(std::max(int32_t(std::floor(std::log2(1.0f / relative))), 0), 40);
  }
  if (target < m_radianceScaleLog2 || target >= m_radianceScaleLog2 + 2) {
    m_radianceScaleLog2 = target < m_radianceScaleLog2 ? target : target - 1;
  }
  m_radianceScale = std::ldexp(1.0f, m_radianceScaleLog2);
}

// Game units per real metre: what the game's unit actually measures, with no artistic scaling.
// Inherited from the host (Remix: 100 * rtx.sceneScale) when unset. Deprecated-option migrations
// live in numos::migrateDeprecated (src/settings.cpp).
float Renderer::Impl::resolveUnitsPerMeter() const {
  const float configured = m_settings.unitsPerMeter;
  if (configured > 0.0f) {
    return configured;
  }
  // Host fallback (Remix: 100 * rtx.sceneScale), floored like 100 * max(sceneScale, 1e-5).
  return std::max(m_inputs.hostUnitsPerMeter, 100.0f * 1e-5f);
}

// Game units per NUMOS kilometre: the one conversion between the host's world and NUMOS's
// (docs/DESIGN.md "Units and scale"). It is the measurement times the one artistic choice,
// rtx.atmosphere.worldScale. Inside NUMOS everything (planet, atmosphere, clouds, wind, haze, fog,
// lightning, LUTs, march steps, shadow maps) runs at full scale in its own kilometres; every position
// and length that crosses to or from the game converts with this figure (args.worldUnitsPerKm in
// the shaders, through numosWorldToKm and friends), and nothing else converts. A scaled NUMOS is
// therefore the full-scale one shrunk.
float Renderer::Impl::worldUnitsPerKm() const {
  const float scale = std::min(std::max(m_settings.worldScale, 0.001f), 1000.0f);
  return std::max(1000.0f * resolveUnitsPerMeter() * scale, 1e-3f);
}

// Game units per haze kilometre: the aerial perspective's conversion, the measurement times
// rtx.atmosphere.hazeWorldScale. The haze, the height fog, the froxel grid and local-light glow
// convert with it (args.apWorldUnitsPerKm, numosWorldToAirKm in the shaders), so terrain is hazed
// for its own distance while World Scale sizes the clouds and the sky.
float Renderer::Impl::hazeWorldUnitsPerKm() const {
  const float scale = std::min(std::max(m_settings.hazeWorldScale, 0.001f), 1000.0f);
  return std::max(1000.0f * resolveUnitsPerMeter() * scale, 1e-3f);
}

namespace {
  Vector3 fhGrazingTransmittance(const AtmosphereArgs& a, float tangentHeightKm);
}

AtmosphereArgs Renderer::Impl::getAtmosphereArgs() const {
  AtmosphereArgs args = {};

  // The time cycle, when enabled, owns the sun direction, including over API pushes to sunElevation
  // and sunRotation: a game with its own cycle would not enable it.
  float sunElevationDeg, sunAzimuthDeg;
  resolveSunAngles(sunElevationDeg, sunAzimuthDeg);

  float azimuthRad   = sunAzimuthDeg   * kDegreesToRadians;
  float elevationRad = sunElevationDeg * kDegreesToRadians;
  args.sunDirection.x = std::cos(elevationRad) * std::sin(azimuthRad);
  args.sunDirection.y = std::sin(elevationRad);
  args.sunDirection.z = std::cos(elevationRad) * std::cos(azimuthRad);

  args.planetRadius = m_settings.planetRadius;
  args.atmosphereThickness = m_settings.atmosphereThickness;
  args.sunIlluminance = m_settings.sunIlluminance * m_settings.sunIntensity;

  // Scattering coefficients: base spectrum x tint x density.
  float airDensity = m_settings.airDensity;
  args.rayleighScattering = m_settings.rayleighScattering * m_settings.airTint * airDensity;

  float aerosolDensity = m_settings.aerosolDensity;
  args.mieScattering = m_settings.mieScattering * m_settings.aerosolTint * aerosolDensity;

  // Aerosols both scatter and absorb, so Mie extinction needs the absorption term too, with the
  // same density multiplier as the scattering.
  args.mieAbsorption = m_settings.mieAbsorption * m_settings.aerosolTint * aerosolDensity;

  args.mieAnisotropy = m_settings.mieAnisotropy;

  // sunSize is the disk's diameter in degrees.
  float sunSizeRad = m_settings.sunSize * kDegreesToRadians;
  args.sunAngularRadius = sunSizeRad * 0.5f;

  float ozoneDensity = m_settings.ozoneDensity;
  args.ozoneAbsorption = m_settings.ozoneAbsorption * ozoneDensity;

  args.ozoneLayerAltitude = m_settings.ozoneLayerAltitude;
  args.ozoneLayerWidth = m_settings.ozoneLayerWidth;

  // Multiscattering blend: 0 = artistic (analytical inline), 1 = physical (LUT hemisphere).
  args.multiScatterPhysicalStrength = m_settings.multiScatterPhysicalStrength;


  // Diffuse-indirect sky radiance multiplier. evalSkyRadiance applies it after the LUT sample, so
  // no bake reads it and normalizeForSkyLutCache keeps it out of the cache key.
  args.skyIndirectRadianceScale = std::max(m_settings.skyIndirectRadianceScale, 0.0f);

  args.transmittanceLutWidth = kTransmittanceLutWidth;
  args.transmittanceLutHeight = kTransmittanceLutHeight;
  args.multiscatteringLutSize = kMultiscatteringLutSize;
  args.skyViewLutWidth = kSkyViewLutWidth;
  args.skyViewLutHeight = kSkyViewLutHeight;

  args.atmosphereRadius = args.planetRadius + args.atmosphereThickness;
  args.rayleighScaleHeight = kRayleighScaleHeight;
  args.mieScaleHeight = kMieScaleHeight;

  // ----- Night sky (lib/night_sky.slangh) -----
  // Bit 3: the sky is dark enough for starlight to register (the sun's sky under ~3% of daylight,
  // sun below ~-3.4 deg); before that even Sirius is far below the sky's display precision.
  args.nightSkyFlags = (nightSkyStarsReady() ? 1u : 0u) | (m_settings.nightSkyStars ? 2u : 0u)
                     | (nightSkyMoonReady() ? 4u : 0u)
                     | (relativeSkyLuminance(sunElevationDeg) < 0.03f ? 8u : 0u);
  args.starCellsPerFace = m_nightSky.cellsPerFace;
  args.milkyWayWidth = m_nightSky.milkyWayWidth;
  args.milkyWayHeight = m_nightSky.milkyWayHeight;
  args.starScintillation = std::max(m_settings.starScintillation, 0.0f);
  // The natural glow of the night sky, in S10 (one 10th-magnitude star per square degree) of
  // V-band light: 1 S10 = 10^(-0.4 (10 + 26.74)) / (pi/180)^2 = 6.61e-12 of the sun's illuminance
  // per steradian. Typical dark-site values at solar-cycle mean (Leinert et al. 1998): airglow is
  // 130 S10 at the zenith (~22.2 V mag/arcsec^2) from a layer ~85-100 km up, coloured as the CIE
  // colour of its spectrum (557.7 nm OI, Na D, 630/636 nm OI, OH lines and the pseudo-continuum;
  // Broadfoot & Kendall 1968) in linear sRGB at unit luminance. Zodiacal light comes from the
  // Leinert table (zodiacal_light_table.slangh), solar coloured. The air below sees the sky above
  // averaged over the hemisphere: van Rhijn-weighted airglow (1.258 x zenith for a 92 km layer),
  // ~150 S10 of zodiacal light and ~80 S10 of starlight. With the stars and extinction that gives
  // ~21.9 V mag/arcsec^2 at the zenith.
  if ((args.nightSkyFlags & 8u) != 0u) {
    constexpr float kS10 = 6.61e-12f;
    constexpr float kAirglowZenithS10 = 130.0f;
    const vec3 kAirglowColour(1.277f, 0.972f, 0.463f);
    const float sunLuminance = 0.2126f * args.sunIlluminance.x + 0.7152f * args.sunIlluminance.y
                             + 0.0722f * args.sunIlluminance.z;
    args.airglowAltitudeKm = 92.0f;
    args.airglowThicknessKm = 12.0f;
    const vec3 airglowZenith = kAirglowColour * (sunLuminance * kAirglowZenithS10 * kS10);
    args.airglowEmission = airglowZenith * (1.0f / args.airglowThicknessKm);
    args.zodiacalLightScale = kS10;
    const float aboveS10 = 0.5f * (150.0f + ((args.nightSkyFlags & 3u) == 3u ? 80.0f : 0.0f));
    args.nightGlowInScatter = airglowZenith * 1.258f + args.sunIlluminance * (aboveS10 * kS10);
  }
  {
    // A pixel's angular size at the screen centre: 2 tan(fovY / 2) / height.
    const float tanHalfFovY = std::abs(m_inputs.projectionToView[5]);
    args.skyPixelAngle = m_inputs.renderHeight > 0u && tanHalfFovY > 0.0f
                       ? 2.0f * tanHalfFovY / float(m_inputs.renderHeight) : 1e-3f;
  }

  // Animation clock for the stars' scintillation: the frames' own time (FrameInputs::deltaSeconds),
  // so it pauses with the host and repeats run to run. Wrapped to keep float precision; the jump
  // once every ~68 minutes is one frame of different twinkle.
  args.timeSeconds = float(std::fmod(m_animationSeconds, 4096.0));
  // Sample cap for the adaptive view march.
  args.cloudViewSamplesMax         = static_cast<float>(m_settings.cloudViewSamplesMax);

  // ----- Night sky geometry (updateCelestial) -----
  args.celestialToLocalX = m_celestialFrame.x;
  args.celestialToLocalY = m_celestialFrame.y;
  args.celestialToLocalZ = m_celestialFrame.z;
  args.eclipticPoleLocal = m_eclipticPoleLocal;
  // In the Earth's shadow a moon still gets the sunlight the Earth's air refracts into it: the red
  // of a total eclipse. The rays that reach the umbra graze the limb ~12 km up (lower ones meet
  // clouds and ground) and cross the air twice; the ring of air they pass through, spread over the
  // umbra, gives ~5e-5 of the sunlight before extinction, which puts a typical total eclipse at
  // ~1.5e-5 of the full moon (magnitude ~ -0.5) and copper red (Danjon L ~ 2). With the atmosphere
  // as configured, so a dustier sky darkens it the way volcanic years do.
  Vector3 umbralTransmittance(0.0f, 0.0f, 0.0f);
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    if (m_moonUmbra[i].x + m_moonUmbra[i].y + m_moonUmbra[i].z > 0.0f) {
      umbralTransmittance = fhGrazingTransmittance(args, 12.0f) * 5e-5f;
      break;
    }
  }
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    args.moons[i] = m_moons[i];
    // A fraction of the sun's illuminance until here.
    const Vector3 fraction = m_moons[i].illuminance + m_moonUmbra[i] * umbralTransmittance;
    args.moons[i].illuminance = vec3(fraction.x * args.sunIlluminance.x, fraction.y * args.sunIlluminance.y,
                                     fraction.z * args.sunIlluminance.z);
  }
  args.moonSkyLightMask = m_moonSkyLightMask;
  args.cloudKeyLight = m_cloudKeyLight;
  // Debug gate read in atmosphere_sky.slangh: bit 1 replaces sky-miss shading with a flat sky.
  args.debugSkyBisectFlags             = (m_settings.debugEnableSkyMissShading ? 0u : 2u);
  // Hex de-tiling of the cloud noise.
  args.cloudHexTilingEnable            = m_settings.cloudHexTilingEnable ? 1.0f : 0.0f;

  {
    args.cloudColor = m_settings.cloudColor;
    args.cloudDensity = m_settings.cloudDensity;
    args.cloudAltitude = m_settings.cloudBaseHeightMeters * 0.001f;
    args.cloudEnabled = m_settings.cloudEnabled ? 1.0f : 0.0f;

    // Wind advection and field evolution are integrated once per frame by advanceCloudMotion()
    // (offset += velocity * dt). Integrating, rather than speed * time, keeps the accumulated field
    // in place when the weather changes the wind speed or direction.
    args.cloudWindOffset.x     = m_cloudAdvectOffset.x;
    args.cloudWindOffset.y     = m_cloudAdvectOffset.y;
    args.cloudWindStepXKm      = m_cloudAdvectStep.x;
    args.cloudWindStepZKm      = m_cloudAdvectStep.y;
    args.cloudEvolutionOffsetX = m_cloudEvolutionOffset.x;
    args.cloudEvolutionOffsetY = m_cloudEvolutionOffset.y;

    args.cloudShadowStrength = m_settings.cloudShadowStrength;

    // Lightning flash state. The scheduler (advanceLightning, once per frame) owns the envelope and
    // the strike position. lightningFlashIntensity is the flash's luminous intensity this frame
    // (lightningIntensityKm2 x envelope; x radianceScale below), the one source the cloud glow, the
    // haze and the scene light all derive from; the colour carries only the hue.
    args.lightningStrikePosKm    = m_lightningStrikePosKm;
    args.lightningEnvelope       = m_lightningEnvelope;
    args.lightningEnvelopePrevious = m_lightningEnvelopePrevious;
    args.lightningFlashIntensity = m_lightningEnvelope * lightningIntensityKm2();
    args.lightningColor          = lightningHue();
  }

  {
    args.cloudThickness = m_settings.cloudDepthMeters * 0.001f;
    args.cloudLayer2TypeSpread = m_settings.cloudLayer2TypeSpread;
    args.cloudViewSamples = m_settings.cloudViewSamples;
    args.cloudTypeMean = m_settings.cloudTypeMean;
    args.cloudTypeSpread = m_settings.cloudTypeSpread;
    args.cloudTypeNoiseScale = m_settings.cloudTypeNoiseScale;
    args.cloudCoverageMean = m_settings.cloudCoverageMean;
    args.cloudCoverageSpread = m_settings.cloudCoverageSpread;
    args.cloudCoverageNoiseScale = m_settings.cloudCoverageNoiseScale;
    // Nominal coverage the NVDF body SDF bakes at. Option 0 tracks the live weather coverage; the
    // published SDF stays in use while an amortized bake catches up. A nonzero option pins it.
    args.nvdfNominalCoverage = m_nvdfNominalCoverageValid
        ? m_nvdfPublishedNominalCoverage
        : computeCloudNvdfNominalCoverage(m_settings, args.cloudCoverageMean);
    // Nubis3 density model and SDF stepping.
    args.nvdfProfileDepthKm    = std::max(m_settings.nvdfProfileDepthKm, 0.05f);
    args.nvdfCoverageOffsetKm  = std::max(m_settings.nvdfCoverageOffsetKm, 0.0f);
    args.nubis3ErosionStrength = std::max(m_settings.nubis3ErosionStrength, 0.0f);
    args.nvdfStepScale           = std::min(std::max(m_settings.nvdfStepScale, 0.0f), 0.95f);
    // Disk aureoles (docs/night-sky.md, "Disk aureole"): the share of a droplet's extinction in its
    // diffraction peak, 1 - wD of the Jendersie & d'Eon 2023 fit (the reference path tracer's phase),
    // and the droplet diameter that sets the peak's width.
    {
      const float d = std::min(std::max(m_settings.cloudDropletDiameter, 5.0f), 50.0f);
      const float wD = std::exp(-0.599085f / (d - 0.641583f) - 0.665888f);
      args.cloudForwardPeakWeight = m_settings.cloudDiskAureole ? std::min(std::max(1.0f - wD, 0.0f), 0.95f) : 0.0f;
      args.cloudDropletDiameterUm = d;
    }
    // View march and secondary cloud LUT only; the voxel grids keep their full-path bake.
    args.nubis3JitterAnimateKm    = std::max(m_settings.nubis3JitterAnimateKm, 0.0f);
    // Floor of the sqrt-adaptive march step. It affects the view march and the secondary cloud LUT,
    // so it stays in the LUT cache keys like nvdfStepScale and cloudViewStepKm.
    args.nubis3AdaptiveStepKm    = std::min(std::max(m_settings.nubis3AdaptiveStepKm, 0.0f), 0.2f);
    args.cloudMultiScatterOctaves = m_settings.cloudMultiScatterOctaves;
    args.cloudLayer2NoiseSeed = m_settings.cloudLayer2NoiseSeed;
    args.cloudNoiseTileKm = m_settings.cloudNoiseTileKm;
    // Cloud occlusion share of the volumetric sky ambient (sampleSkyAmbientForVolume).
    args.cloudSkyAmbientCloudOcclusionStrength = m_settings.cloudSkyAmbientCloudOcclusionStrength;
    // Cloud cluster footprint for the placement map bake.
    args.cloudCellSizeKm = m_settings.cloudCellSizeKm;
  }

  // Nubis Cubed lighting (Schneider 2023), read by evalNubisCubedSampleCore.
  {
    args.cloudPhaseG1         = m_settings.cloudPhaseG1;
    args.cloudRenderFrameIdx  = m_cloudRenderFrameIdx;

    // Step target of the adaptive view march.
    args.cloudViewStepKm               = m_settings.cloudViewStepKm;
  }

  args.cloudColumnTopVariation = m_settings.cloudColumnTopVariation;
  args.cloudBodyBillowStrength = std::max(m_settings.cloudBodyBillowStrength, 0.0f);
  args.cloudBodyBillowSizeKm = std::max(m_settings.cloudBodyBillowSizeKm, 0.05f);
  args.cloudBodyBillowGain = std::min(std::max(m_settings.cloudBodyBillowGain, 0.0f), 1.0f);
  args.cloudCurlStrengthKm = std::max(m_settings.cloudCurlStrengthKm, 0.0f);
  args.cloudDiffuseSunStrength = std::min(std::max(m_settings.cloudDiffuseSunStrength, 0.0f), 1.0f);
  args.cloudSunOctaveA = std::min(std::max(m_settings.cloudSunOctaveA, 0.0f), 1.0f);
  args.cloudSunOctaveB = std::min(std::max(m_settings.cloudSunOctaveB, 0.0f), 1.0f);
  args.cloudSunOctaveC = std::min(std::max(m_settings.cloudSunOctaveC, 0.0f), 1.0f);
  args.cloudSunOctaveCount = std::min(std::max(m_settings.cloudSunOctaveCount, 1u), 8u);
  args.groundAlbedo = std::min(std::max(m_settings.groundAlbedo, 0.0f), 1.0f);
  args.fogNearFadeKm = std::max(m_settings.fogNearFadeMeters, 0.0f) * 0.001f;
  args.cloudColumnTopShape = m_settings.cloudColumnTopShape;
  args.cloudColumnBaseVariation = m_settings.cloudColumnBaseVariation;

  // Gates the primary-ray branch in evalSkyRadiance that composites the prerendered cloud RT; when
  // off, primary sky misses are cloudless.
  {
    // Also 0 while clouds are off. The screen pass would only write "no cloud" (zero radiance, full
    // transmittance, no entry depth), an exact identity for the composite, so the composite skips
    // the read and dispatchCloudScreenPass skips the pass.
    args.cloudRenderRTEnable = m_cloudCompositeActive ? 1u : 0u;
    // Secondary rays read the cloud LUT.
    args.cloudSecondaryLutEnable = m_settings.cloudSecondaryLutEnable ? 1u : 0u;
    // Screen pass flags: bit 17 temporal history valid, bit 18 cell march (quarter), bit 19 its
    // cells are 4x4 (sixteenth).
    args.cloudScreenInterleave = (m_cloudTemporalHistoryValid ? (1u << 17u) : 0u)
      | (cloudQuarterMarchActive() ? (1u << 18u) : 0u)
      | (cloudSixteenthMarchActive() ? (1u << 19u) : 0u);
    args.cloudTemporalHistoryWeight = std::min(std::max(m_settings.cloudTemporalHistoryWeight, 0.0f), 0.98f);
    args.cloudTemporalStillSamples = float(std::clamp(m_settings.cloudTemporalStillSamples, 1, 1024));
    args.cloudReprojectDepthTolerance = std::max(m_settings.cloudHistoryDepthTolerance, 0.0f);
    // Lighting-bake interleave periods and phases.
    args.cloudDomeInterleave    = packCloudInterleave(m_cloudDomePeriodThisFrame, m_cloudRenderFrameIdx);
    args.cloudDomeInterleave   |= m_cloudDomeResetThisFrame ? (1u << 16u) : 0u;
    // Mode 1 is accepted but behaves like mode 0 (native resolution).
    args.cloudDetailLodBias   = m_settings.cloudDetailLodBias;
    args.cloudDetailLodEnable = m_settings.cloudDetailLodMode >= 2 ? 1u : 0u;

  }

  // The host boundary (docs/DESIGN.md "Units and scale"). worldUnitsPerKm (unitsPerMeter x
  // worldScale) converts clouds, sky and lightning through numosWorldToKm and friends;
  // apWorldUnitsPerKm (unitsPerMeter x hazeWorldScale) converts the haze, fog and local-light glow
  // through numosWorldToAirKm. cameraWorldPosYUpKm is the camera in the cloud frame
  // (cloudFrameCameraKm()): its height above the ground datum less the cloud field's vertical
  // offset. cameraAltitudeKm is the same height without that offset, since the sky, haze and fog
  // stand on the ground datum. Both are NUMOS kilometres.
  {
    args.cloudVoxelShadowsEnable  = m_settings.cloudVoxelShadowsEnable ? 1u : 0u;
    args.worldUnitsPerKm = worldUnitsPerKm();
    args.cloudColumnFeather = m_settings.cloudColumnFeather;
    args.cameraWorldPosYUpKm = cloudFrameCameraKm();
    args.cameraAltitudeKm = m_cameraWorldPosYUpKm.y - m_groundLevelYUpKm;
    args.apWorldUnitsPerKm = hazeWorldUnitsPerKm();
    args.apCameraAltitudeKm = args.cameraAltitudeKm * (args.worldUnitsPerKm / args.apWorldUnitsPerKm);
  }

  // Aerial perspective (docs/aerial-perspective-v2.md). Camera matrices are bound separately.
  {
    args.aerialPerspectiveMieAnisotropyMax =
      std::min(std::max(m_settings.aerialPerspectiveMieAnisotropyMax, -1.0f), 1.0f);
    args.isZUp = m_inputs.zUp ? 1u : 0u;
    args.flipUpAxis = m_settings.flipUpAxis ? 1u : 0u;
    args.cameraPosition = m_apCameraPosition;
    args.apFrameIndex = m_inputs.frameIndex;

    const ApGrid grid = apGrid();
    const bool active = m_settings.aerialPerspective && !m_inputs.hostSkyOverride;
    args.apFroxelSizeX = active ? grid.x : 0u;
    args.apFroxelSizeY = active ? grid.y : 0u;
    args.apFroxelSlices = active ? grid.z : 0u;
    args.apFroxelRange = grid.range;
    args.apFroxelLinearDepth = grid.range / kApFroxelLinearDepthFraction;
    args.apFroxelLogScale = std::log(1.0f + kApFroxelLinearDepthFraction);
    // Host input (Remix: froxelMaxDistanceMeters in world units while its volumetrics are on).
    args.apStartDistance = std::max(m_inputs.volumetricsHandoffWorldUnits, 0.0f);

    args.apFarSizeX = active ? kApFarSizeX : 0u;
    args.apFarSizeY = active ? kApFarSizeY : 0u;
    args.apFarSlices = active ? kApFarSlices : 0u;
    args.apFarLinearDepthKm = kApFarLinearDepthKm;
    args.apFarLogScale = std::log(1.0f + kApFarRangeKm / kApFarLinearDepthKm);

    // A missing TLAS demotes the lighting pass to its untraced variant; these flags then do nothing.
    args.apSceneShadow = m_settings.aerialPerspectiveSceneShadow ? 1u : 0u;
    args.apLocalLightShadows = m_settings.aerialPerspectiveLocalLightShadows ? 1u : 0u;
    args.apHistoryWeightMin = 1.0f / float(std::clamp(m_settings.aerialPerspectiveHistoryFrames, 1, 255));
    args.apHistoryReset = 0u;  // set per dispatch by dispatchAerialPerspective
    args.apDebugView = static_cast<uint32_t>(std::clamp(m_settings.aerialPerspectiveDebugView, 0, 10));

    // Height fog. Koschmieder: contrast falls to 2% at the visibility distance, sigma = ln(50) / V.
    args.fogExtinctionPerKm = m_settings.fogVisibilityMeters > 0.0f
      ? 3.912f / (m_settings.fogVisibilityMeters * 0.001f) : 0.0f;
    args.fogScaleHeightKm = std::max(m_settings.fogHeightFalloffMeters, 1.0f) * 0.001f;
    args.fogBaseAltitudeKm = m_settings.fogBaseHeightMeters * 0.001f;
    args.fogAlbedo = std::min(std::max(m_settings.fogAlbedo, 0.0f), 1.0f);
    args.fogAnisotropy = std::min(std::max(m_settings.fogAnisotropy, -0.95f), 0.95f);

    // Local lights. The count is the single gate on the whole path; buildAerialPerspectiveLights
    // owns it and runs before every consumer of these args.
    const bool localLightsActive = active && m_settings.aerialPerspectiveLocalLights && m_apLightCount > 0u;
    args.apLocalLightCount = localLightsActive ? m_apLightCount : 0u;
    args.apLocalLightTilesX = (grid.x + AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE - 1u) / AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE;
    args.apLocalLightTilesY = (grid.y + AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE - 1u) / AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE;
    args.apLocalLightIntensity = std::max(m_settings.aerialPerspectiveLocalLightIntensity, 0.0f);
    args.apHostLightInjection = active && m_inputs.hostLocalLightInjection ? 1u : 0u;
  }

  // Cloud layer 2: a stratiform layer marched and lit like layer 1. Off by
  // default. Following the camera, its base is the camera's cloud-frame height plus the offset, and
  // the step it took since last frame goes to the temporal filter (0 across a camera cut).
  {
    const bool follow = m_settings.cloudLayer2FollowCamera;
    args.cloudLayer2Enable        = m_settings.cloudLayer2Enable ? 1u : 0u;
    args.cloudLayer2Altitude      = follow
      ? args.cameraWorldPosYUpKm.y + m_settings.cloudLayer2CameraOffsetMeters * 0.001f
      : m_settings.cloudLayer2BaseHeightMeters * 0.001f;
    args.cloudLayer2Thickness     = std::max(m_settings.cloudLayer2DepthMeters, 1.0f) * 0.001f;
    args.cloudLayer2TypeMean      = m_settings.cloudLayer2TypeMean;
    args.cloudLayer2CoverageMean  = m_settings.cloudLayer2CoverageMean;
    args.cloudLayer2DensityScale  = std::max(m_settings.cloudLayer2DensityScale, 0.0f);
    args.cloudLayer2FollowStepKm  = follow && !m_cloudAnchorCutThisFrame && !m_inputs.cameraCut
      ? m_cloudAnchor.deltaKm.y : 0.0f;
    args.cloudLayer2Albedo        = Vector3(std::min(std::max(m_settings.cloudLayer2Albedo.x, 0.0f), 1.0f),
                                            std::min(std::max(m_settings.cloudLayer2Albedo.y, 0.0f), 1.0f),
                                            std::min(std::max(m_settings.cloudLayer2Albedo.z, 0.0f), 1.0f));
    // The patch-field map (updateCloudLayer2Map), readable only while it holds this layer's seed.
    const CloudLayer2MapState& map = m_cloudLayer2MapState;
    const bool mapLive = map.valid && args.cloudLayer2Enable != 0u && map.seed == args.cloudLayer2NoiseSeed
                      && m_cloudLayer2Map.isValid();
    args.cloudLayer2MapCenterX    = mapLive ? map.centerX : 0.0f;
    args.cloudLayer2MapCenterZ    = mapLive ? map.centerZ : 0.0f;
    args.cloudLayer2MapValid      = mapLive ? 1.0f : 0.0f;
  }

  args.cloudLightingLodThreshold = m_settings.cloudLightingLodThreshold;
  args.cloudMissLinearViewZ = m_missLinearViewZ;
  {
    // Wind direction as a unit vector, for the detail drift and base shear.
    const float windDirDeg = m_settings.cloudWindDirection;
    const float windRad = windDirDeg * kDegreesToRadians;
    args.cloudWindDirUnitX = std::cos(windRad);
    args.cloudWindDirUnitZ = std::sin(windRad);
  }
  {
    const CloudShadowMapState& sm = m_cloudShadow;
    bool live = m_settings.cloudShadowMapEnable && m_cloudShadowMap.isValid() && m_cloudHorizonMap.isValid()
             && m_cloudVerticalOd.isValid() && sm.keyValid;
    uint32_t slots = 0u;
    for (uint32_t m = 0; m < 2; ++m) {
      float dx[4], dy[4], dz[4], cu[4], hu[4], cv[4], hv[4];
      for (uint32_t k = 0; k < kCloudShadowMapCascades; ++k) {
        const CloudShadowCascade& cc = sm.cascade[m][k];
        live = live && cc.valid;
        dx[k] = cc.dir.x; dy[k] = cc.dir.y; dz[k] = cc.dir.z;
        cu[k] = cc.centerU; hu[k] = cc.halfU; cv[k] = cc.centerV; hv[k] = cc.halfV;
        slots |= (cc.slot & 7u) << (16u * m + 4u * k);
      }
      args.cloudShadowDirX[m] = vec4(vec2(dx[0], dx[1]), dx[2], dx[3]);
      args.cloudShadowDirY[m] = vec4(vec2(dy[0], dy[1]), dy[2], dy[3]);
      args.cloudShadowDirZ[m] = vec4(vec2(dz[0], dz[1]), dz[2], dz[3]);
      args.cloudShadowCenterU[m] = vec4(vec2(cu[0], cu[1]), cu[2], cu[3]);
      args.cloudShadowHalfU[m] = vec4(vec2(hu[0], hu[1]), hu[2], hu[3]);
      args.cloudShadowCenterV[m] = vec4(vec2(cv[0], cv[1]), cv[2], cv[3]);
      args.cloudShadowHalfV[m] = vec4(vec2(hv[0], hv[1]), hv[2], hv[3]);
    }
    args.cloudShadowSlots = slots;
    args.cloudShadowLive = live ? 1u : 0u;
    args.cloudShadowBakeTarget = 0u;
    args.cloudShadowBakeRow0 = 0u;
  }
  // Pre-exposure (updateRadianceScale): every light source carries R, so everything lit by them is
  // physical x R. A multiply by 1 in daylight, which is exact.
  args.radianceScaleHistoryRatio = m_radianceScale / m_prevRadianceScale;
  // Night gain (rtx.atmosphere.nightGainStops, night_gain.h) scales all natural night light: the
  // moons (their light, discs and the sky they light), airglow, zodiacal light (a fraction of the
  // sun's illuminance, which keeps its own level) and the star field (nightSkyGain).
  args.nightSkyGain = m_nightGain;
  args.zodiacalLightScale *= m_nightGain;
  args.airglowEmission = args.airglowEmission * m_nightGain;
  args.nightGlowInScatter = args.nightGlowInScatter * m_nightGain;
  args.sunIlluminance = args.sunIlluminance * m_radianceScale;
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    args.moons[i].illuminance = args.moons[i].illuminance * (m_radianceScale * m_nightGain);
  }
  args.lightningFlashIntensity *= m_radianceScale;
  args.apLocalLightIntensity *= m_radianceScale;  // the local-light gain; host injection is the host's own units
  args.airglowEmission = args.airglowEmission * m_radianceScale;
  args.nightGlowInScatter = args.nightGlowInScatter * m_radianceScale;
  args.apDebugRadianceScale = m_radianceScale;
  args.apDebugNitsPerUnit = luxPerUnit(m_settings) / m_radianceScale;

  return args;
}

bool Renderer::Impl::needsLutRecompute() const {
  if (!m_initialized || m_lutsNeedRecompute) {
    return true;
  }

  // normalizeForSkyLutCache zeroes the per-frame animated fields no sky LUT reads, so the memcmp
  // only fires when a real sky parameter changed.
  AtmosphereArgs currentArgs = getAtmosphereArgs();
  normalizeForSkyLutCache(currentArgs);
  return memcmp(&currentArgs, &m_cachedArgs, sizeof(AtmosphereArgs)) != 0;
}

bool Renderer::Impl::needsCloudPlacementRebake() const {
  // The only inputs cloud_placement_map_baker.comp.slang reads: the cluster footprint and the map's
  // tile period (the cells-per-tile rounding depends on both).
  return m_cachedPlacementCellSizeKm != m_settings.cloudCellSizeKm
      || m_cachedPlacementTileKm     != m_settings.cloudNoiseTileKm;
}

void Renderer::Impl::cacheCloudPlacementBakeInputs() {
  m_cachedPlacementCellSizeKm = m_settings.cloudCellSizeKm;
  m_cachedPlacementTileKm     = m_settings.cloudNoiseTileKm;
}

Texture Renderer::Impl::createTexture(PassRecorder& rec, const char* name, TextureType type, Format format,
                                      uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevels) {
  // New textures are cleared to zero: some consumers rely on that first-frame content (for example
  // the aerial perspective visibility).
  TextureDesc desc;
  desc.type = type;
  desc.format = format;
  desc.width = width;
  desc.height = height;
  desc.depth = depth;
  desc.mipLevels = mipLevels;
  desc.debugName = name;
  Texture t;
  t.desc = desc;
  t.handle = rec.gpu().createTexture(desc);
  m_ownedResources.push_back(t.handle);
  const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  rec.gpu().clearTexture(t.handle, zero);
  return t;
}

void Renderer::Impl::replaceTexture(PassRecorder& rec, Texture& slot, Texture created) {
  if (slot.isValid()) {
    rec.gpu().destroy(slot.handle);
    m_ownedResources.erase(std::remove(m_ownedResources.begin(), m_ownedResources.end(), slot.handle),
                           m_ownedResources.end());
  }
  slot = created;
}

Buffer Renderer::Impl::createBuffer(PassRecorder& rec, const char* name, BufferUsage usage, size_t size) {
  Buffer b;
  b.desc.usage = usage;
  b.desc.size = size;
  b.desc.debugName = name;
  b.handle = rec.gpu().createBuffer(b.desc);
  m_ownedResources.push_back(b.handle);
  return b;
}

void Renderer::Impl::replaceBuffer(PassRecorder& rec, Buffer& slot, Buffer created) {
  if (slot.isValid()) {
    rec.gpu().destroy(slot.handle);
    m_ownedResources.erase(std::remove(m_ownedResources.begin(), m_ownedResources.end(), slot.handle),
                           m_ownedResources.end());
  }
  slot = created;
}

GpuHandle Renderer::Impl::sampler(IGpu& gpu, Filter filter, Filter mipFilter, AddressMode address, float maxLod) {
  // Samplers are cached by description.
  uint32_t lodBits;
  std::memcpy(&lodBits, &maxLod, sizeof(lodBits));
  const uint64_t key = uint64_t(filter) | (uint64_t(mipFilter) << 8) | (uint64_t(address) << 16) | (uint64_t(lodBits) << 32);
  for (const auto& entry : m_samplers) {
    if (entry.first == key) {
      return entry.second;
    }
  }
  SamplerDesc desc;
  desc.filter = filter;
  desc.mipFilter = mipFilter;
  desc.addressU = desc.addressV = desc.addressW = address;
  desc.maxLod = maxLod;
  const GpuHandle handle = gpu.createSampler(desc);
  m_ownedResources.push_back(handle);
  m_samplers.emplace_back(key, handle);
  return handle;
}

void Renderer::Impl::createLutResources(PassRecorder& rec) {
  // Blue noise, used when the host supplies none.
  if (!m_blueNoise.isValid()) {
    m_blueNoise = createTexture(rec, "NUMOS Blue Noise", TextureType::Tex2DArray, Format::R8Unorm, 128, 128, 64, 1);
    rec.gpu().uploadTexture(m_blueNoise.handle, 0, &blueNoise_128x128x64[0][0], sizeof(blueNoise_128x128x64));
  }

  // Resource recreation invalidates the published front-field metadata.
  m_cloudNvdfSdfFront = 0u;
  m_nvdfNominalCoverageValid = false;

  replaceTexture(rec, m_transmittanceLut, createTexture(rec, "Atmosphere Transmittance LUT", TextureType::Tex2D,
    Format::RGBA16Float, kTransmittanceLutWidth, kTransmittanceLutHeight, 1, 1));
  replaceTexture(rec, m_multiscatteringLut, createTexture(rec, "Atmosphere Multiscattering LUT", TextureType::Tex2D,
    Format::RGBA16Float, kMultiscatteringLutSize, 2 * kMultiscatteringLutSize, 1, 1));  // + twilight half
  replaceTexture(rec, m_skyViewLut, createTexture(rec, "Atmosphere Sky View LUT", TextureType::Tex2D,
    Format::RGBA16Float, kSkyViewLutWidth, kSkyViewLutHeight, 1, 1));

  // The aerial perspective's volumes follow the render resolution; ensureAerialPerspectiveResources
  // creates them on demand.

  // Cloud-occluded sky-ambient LUT, baked every frame from the camera position for the volumetric
  // pass's sky-ambient hemisphere integration. rgb = premultiplied cloud radiance, a = cloud
  // transmittance (cloud_sky_transmittance_lut.comp.slang).
  replaceTexture(rec, m_cloudSkyTransmittanceLut, createTexture(rec, "Atmosphere Cloud Sky Transmittance LUT",
    TextureType::Tex2D, Format::RGBA16Float, kCloudSkyTransmittanceLutWidth, kCloudSkyTransmittanceLutHeight, 1, 1));

  // Cloud shadow maps (lib/cloud_shadow_map.slangh): per slot, 256^2 rays x 32 depths of cumulative
  // optical depth (sun map: body, soft body; horizon map: body); one slot per cascade plus a spare.
  replaceTexture(rec, m_cloudShadowMap, createTexture(rec, "Atmosphere Cloud Shadow Map", TextureType::Tex3D,
    Format::RG16Float, kCloudShadowMapSize, kCloudShadowMapSize, kCloudShadowMapSlices * kCloudShadowMapSlots, 1));
  replaceTexture(rec, m_cloudVerticalOd, createTexture(rec, "Atmosphere Cloud Vertical Optical Depth", TextureType::Tex3D,
    Format::RGBA16Float, kCloudNvdfSizeXZ, kCloudNvdfSizeY, kCloudNvdfSizeXZ, 1));
  replaceTexture(rec, m_cloudHorizonMap, createTexture(rec, "Atmosphere Cloud Horizon Map", TextureType::Tex3D,
    Format::R16Float, kCloudShadowMapSize, kCloudShadowMapSize, kCloudShadowMapSlices * kCloudShadowMapSlots, 1));
  // Beer form of the sun map (front / back depth, body and soft totals per sun ray), one layer per slot.
  replaceTexture(rec, m_cloudBeerMap, createTexture(rec, "Atmosphere Cloud Beer Map", TextureType::Tex3D,
    Format::RGBA16Float, kCloudShadowMapSize, kCloudShadowMapSize, kCloudShadowMapSlots, 1));
  m_cloudShadow = CloudShadowMapState {};  // also re-runs the vertical optical depth
  // Layer 2 patch-field map (cloud_layer2_map.comp): two cascades, one per depth slice; ~8 MB.
  replaceTexture(rec, m_cloudLayer2Map, createTexture(rec, "Atmosphere Cloud Layer 2 Map", TextureType::Tex3D,
    Format::RG16Float, kCloudLayer2MapSize, kCloudLayer2MapSize, 2, 1));
  m_cloudLayer2MapState = CloudLayer2MapState {};

  // Cloud NVDF SDF bake chain, 256x64x256 with texture y vertical (cloud_nvdf.h). Occupancy and the
  // JFA ping-pong are bake scratch; the two R16F SDF volumes are the published front/back pair.
  // initialize() runs the full bake before any consumer can sample, so no consumer may run ahead of
  // the init bake.
  replaceTexture(rec, m_cloudNvdfOccupancy, createTexture(rec, "Atmosphere Cloud NVDF Occupancy", TextureType::Tex3D,
    Format::R8Unorm, kCloudNvdfSizeXZ, kCloudNvdfSizeY, kCloudNvdfSizeXZ, 1));
  for (uint32_t i = 0; i < 2; ++i) {
    replaceTexture(rec, m_cloudNvdfJfa[i], createTexture(rec,
      i == 0 ? "Atmosphere Cloud NVDF JFA Seeds 0" : "Atmosphere Cloud NVDF JFA Seeds 1", TextureType::Tex3D,
      Format::R32Uint, kCloudNvdfSizeXZ, kCloudNvdfSizeY, kCloudNvdfSizeXZ, 1));
    replaceTexture(rec, m_cloudNvdfSdf[i], createTexture(rec,
      i == 0 ? "Atmosphere Cloud NVDF SDF 0" : "Atmosphere Cloud NVDF SDF 1", TextureType::Tex3D,
      Format::R16Float, kCloudNvdfSizeXZ, kCloudNvdfSizeY, kCloudNvdfSizeXZ, 1));
  }

  // 128^3 RGBA8 cumulus turbulence volume (~8 MB) with a mip chain for the detail LOD. Baked once
  // at init by dispatchCloudDetailNoiseBake.
  replaceTexture(rec, m_cloudCumulusNoise3D, createTexture(rec, "Atmosphere Cloud Cumulus Noise 3D", TextureType::Tex3D,
    Format::RGBA8Unorm, kCloudDetailNoise3DSize, kCloudDetailNoise3DSize, kCloudDetailNoise3DSize,
    kCloudDetailNoise3DMipLevels));

  // Secondary-ray cloud LUT (256x256 RGBA16F), written every frame by dispatchCloudSecondaryLut and
  // read by evalSkyRadiance's non-primary branch. The sky-from-clouds bleed samples a coarse mip as
  // a wide blur; the 6 levels (256 to 8) are filled with a Gaussian chain after each bake.
  replaceTexture(rec, m_cloudSecondaryLut, createTexture(rec, "Atmosphere Cloud Secondary LUT", TextureType::Tex2D,
    Format::RGBA16Float, kCloudSecondaryLutWidth, kCloudSecondaryLutHeight, 1, kCloudSecondaryLutMipLevels));

  // Cloud placement map (512x512 RGBA8): R = cluster field, G = top-height jitter, B = base lift,
  // tiled at cloudNoiseTileKm. Baked at init and again when cloudCellSizeKm or cloudNoiseTileKm
  // change.
  replaceTexture(rec, m_cloudPlacementMap, createTexture(rec, "Atmosphere Cloud Placement Map", TextureType::Tex2D,
    Format::RGBA8Unorm, kCloudPlacementMapSize, kCloudPlacementMapSize, 1, 1));
}

void Renderer::Impl::computeLuts(PassRecorder& rec) {
  if (!m_initialized) {
    return;
  }


  // Re-bake the cloud placement map when its inputs change (cloudCellSizeKm / cloudNoiseTileKm).
  // Only the NVDF occupancy bake reads it, and the NVDF key holds the same two inputs, so the
  // state machine below starts a re-bake this frame; the barrier orders the write before that read.
  if (needsCloudPlacementRebake()) {
    dispatchCloudPlacementMapBake(rec);
    cacheCloudPlacementBakeInputs();
    rec.barrier();
  }

  // Amortized NVDF SDF re-bake. A changed bake input (column shape, cell or tile size, quantized
  // thickness, nominal coverage) starts it; it then runs kCloudNvdfJumpPassesPerFrame JFA passes per
  // frame into the back SDF and swaps on completion. Consumers keep reading the last complete bake,
  // so a weather re-bake never shows a half-baked field or spikes a frame.
  stepCloudNvdfBake(rec);

  // The sky LUTs re-bake only when their inputs change. With the split keys each bake compares a key
  // normalized down to the fields it reads: per-frame inputs (wind, camera, the stars' scintillation
  // clock) re-bake nothing, and sun or moon motion re-bakes only the sky-view LUT, not the heavy
  // transmittance + multiscatter pair. The transmittance/MS key is a sub-key of the sky-view key
  // and the sky-view bake reads both LUTs, so tmsDirty implies skyViewDirty.
  //
  // debugDispatchSkyLuts off freezes the whole cascade so its per-frame cost can be measured; the
  // sky then stops tracking the sun.
  if (!m_settings.debugDispatchSkyLuts) {
    // Frozen: skip all three bakes and leave the caches alone so the next enabled frame
    // re-evaluates the gates.
  } else if (m_settings.skyLutCacheKeySplitEnable) {
    AtmosphereArgs currentArgs = getAtmosphereArgs();
    AtmosphereArgs tmsKey = currentArgs;
    normalizeForTransmittanceMsKey(tmsKey, m_settings);
    zeroCloudFieldsForSkyKeys(tmsKey, currentArgs.multiScatterPhysicalStrength);
    quantizeMediumForSkyKeys(tmsKey);
    AtmosphereArgs skyViewKey = currentArgs;
    normalizeForSkyViewLutKey(skyViewKey, m_settings);
    zeroCloudFieldsForSkyKeys(skyViewKey, currentArgs.multiScatterPhysicalStrength);
    quantizeMediumForSkyKeys(skyViewKey);

    const bool tmsDirty = m_lutsNeedRecompute
        || memcmp(&tmsKey, &m_cachedTransmittanceMsKey, sizeof(AtmosphereArgs)) != 0;
    const bool skyViewDirty = tmsDirty
        || memcmp(&skyViewKey, &m_cachedSkyViewKey, sizeof(AtmosphereArgs)) != 0;

    if (tmsDirty) {
      dispatchTransmittanceLut(rec);
      rec.barrier();
      dispatchMultiscatteringLut(rec);
      rec.barrier();

      m_cachedTransmittanceMsKey = tmsKey;
    }

    if (m_settings.cloudProfilingLog && (tmsDirty || skyViewDirty)) {
      logInfo(format("[Sky LUT] re-bake: ", tmsDirty ? "transmittance+multiscattering+sky-view" : "sky-view"));
    }
    if (skyViewDirty) {
      dispatchSkyViewLut(rec);
      rec.barrier();

      m_cachedSkyViewKey = skyViewKey;
      // Keep the single-key cache current so turning the split off does not force a re-bake.
      m_cachedArgs = currentArgs;
      normalizeForSkyLutCache(m_cachedArgs);
      m_lutsNeedRecompute = false;
    }
  } else if (needsLutRecompute()) {
    dispatchTransmittanceLut(rec);
    rec.barrier();
    dispatchMultiscatteringLut(rec);
    rec.barrier();
    dispatchSkyViewLut(rec);
    rec.barrier();

    // The split keys are refreshed too, so turning the split on does not force a re-bake.
    AtmosphereArgs currentArgs = getAtmosphereArgs();
    m_cachedArgs = currentArgs;
    normalizeForSkyLutCache(m_cachedArgs);
    m_cachedSkyViewKey = currentArgs;
    normalizeForSkyViewLutKey(m_cachedSkyViewKey, m_settings);
    zeroCloudFieldsForSkyKeys(m_cachedSkyViewKey, currentArgs.multiScatterPhysicalStrength);
    quantizeMediumForSkyKeys(m_cachedSkyViewKey);
    m_cachedTransmittanceMsKey = currentArgs;
    normalizeForTransmittanceMsKey(m_cachedTransmittanceMsKey, m_settings);
    zeroCloudFieldsForSkyKeys(m_cachedTransmittanceMsKey, currentArgs.multiScatterPhysicalStrength);
    quantizeMediumForSkyKeys(m_cachedTransmittanceMsKey);
    m_lutsNeedRecompute = false;
  }



  // Nothing reads the cloud bakes while clouds are off: the view march and the terrain cloud
  // shadow both early-out on cloudEnabled.
  const bool cloudsEnabled = m_settings.cloudEnabled;

  // The voxel key is compared every frame: it tells the interleaved updates below whether the
  // previous frame's work still describes this frame's inputs.
  bool voxelKeyChanged = false;
  {
    AtmosphereArgs voxelKey = getAtmosphereArgs();
    normalizeForVoxelGridKey(voxelKey, m_settings);
    voxelKeyChanged = memcmp(&voxelKey, &m_cachedVoxelGridKey, sizeof(AtmosphereArgs)) != 0;
    if (voxelKeyChanged) {
      m_cachedVoxelGridKey = voxelKey;
    }
  }

  if (!cloudsEnabled) {
    // Force a fresh bake on the frame clouds come back, rather than trusting a key that went stale
    // while they were off.
    memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));
  }

  resolveCloudInterleave(voxelKeyChanged);

  // The cloud screen march runs later, in dispatchPostDepth, because it needs the host's depth.
  // Everything it reads (shadow maps, layer 2 map, secondary LUT) is baked here first.
  if (cloudsEnabled && m_settings.cloudShadowMapEnable) {
    updateCloudShadowMap(rec);
  }
  if (cloudsEnabled && m_settings.cloudLayer2Enable) {
    updateCloudLayer2Map(rec);
  }

  // Aerial perspective. Camera-fitted, so it runs every frame; after the cloud shadow map because
  // its cloud-shadow hook (apCloudSunTransmittance) reads it. The composite skips it under a host
  // sky override.
  if (m_settings.aerialPerspective && !m_inputs.hostSkyOverride) {
    rec.barrier();
    dispatchAerialPerspective(rec);
  } else {
    m_apHistoryValid = false;
    m_apNearPending = false;
  }

  if (m_settings.cloudSecondaryLutEnable && m_cloudSecondaryLut.isValid()) {
    rec.barrier();
    dispatchCloudSecondaryLut(rec);
    m_cloudDomeHistoryValid = true;
  } else {
    m_cloudDomeHistoryValid = false;
  }

  // Cloud sky-light LUT: a coarse, solid-angle-weighted reduction of the dome LUT above, so it runs
  // after the dome bake and its mip chain. With the dome LUT off it writes "no cloud" (nothing
  // marched to reduce), so it needs no barrier-ordered input in that case.
  if (m_settings.debugDispatchCloudSkyTransmittance) {
    rec.barrier();
    dispatchCloudSkyTransmittanceLut(rec);
  }

  // All LUTs are written before the host's ray tracing reads them.
  rec.barrier();
}

void Renderer::Impl::resolveCloudInterleave(bool cloudInputsChanged) {
  // isCameraCut never fires on an engine that keeps translation out of the view matrix (see the
  // anchor block in updateFrame), which is why the anchor-delta cut is tested alongside it.
  const bool cameraJumped = m_cloudAnchorCutThisFrame || m_inputs.cameraCut;

  // The dome is marched from the exact camera position, so a jump moves every texel at once.
  m_cloudDomeResetThisFrame = cameraJumped || !m_cloudDomeHistoryValid;
  m_cloudDomePeriodThisFrame = (cloudInputsChanged || cameraJumped || !m_cloudDomeHistoryValid)
    ? 1u : cloudInterleavePeriod(m_settings.cloudSecondaryLutInterleaveMode);
  // The temporal filter's history only has to describe the same view: its neighbourhood clamp
  // absorbs a lighting re-bake, and a lightning flash rescales it (cloudTemporalHistory), so only a
  // cut drops it.
  if (cameraJumped) {
    m_cloudTemporalHistoryValid = false;
  }

  if (m_settings.cloudProfilingLog) {
    ++m_cloudBakeWindowFrames;
    m_cloudBakeInputChanges += cloudInputsChanged ? 1u : 0u;
    m_cloudDomeFullFrames += m_cloudDomePeriodThisFrame == 1u ? 1u : 0u;
    if (m_cloudBakeWindowFrames >= 120u) {
      logInfo(format("[Cloud profile] stage=BakeInterleave requestedDomePeriod=",
        cloudInterleavePeriod(m_settings.cloudSecondaryLutInterleaveMode),
        " resolvedDomePeriod=", m_cloudDomePeriodThisFrame, " domeFullFrames=", m_cloudDomeFullFrames,
        " inputChanges=", m_cloudBakeInputChanges, " windowFrames=", m_cloudBakeWindowFrames,
        " coverageKey=", m_cachedVoxelGridKey.cloudCoverageMean));
      m_cloudBakeWindowFrames = 0u;
      m_cloudBakeInputChanges = 0u;
      m_cloudDomeFullFrames = 0u;
    }
  } else {
    m_cloudBakeWindowFrames = 0u;
    m_cloudBakeInputChanges = 0u;
    m_cloudDomeFullFrames = 0u;
  }
}

void Renderer::Impl::dispatchTransmittanceLut(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Transmittance LUT");

  AtmosphereArgs args = getAtmosphereArgs();

  rec.bindConstants(TRANSMITTANCE_LUT_ATMOSPHERE_ARGS, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(TRANSMITTANCE_LUT_OUTPUT, m_transmittanceLut);


  rec.setPass(PassId::transmittance_lut);

  // 16x16 thread groups.
  uint32_t groupsX = (kTransmittanceLutWidth + 15) / 16;
  uint32_t groupsY = (kTransmittanceLutHeight + 15) / 16;
  rec.dispatch(groupsX, groupsY, 1);
}

void Renderer::Impl::dispatchMultiscatteringLut(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Multiscattering LUT");

  AtmosphereArgs args = getAtmosphereArgs();

  rec.bindConstants(MULTISCATTERING_LUT_ATMOSPHERE_ARGS, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(MULTISCATTERING_LUT_TRANSMITTANCE_INPUT, m_transmittanceLut);

  const GpuHandle linearSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge, 0.0f);
  rec.bindSampler(MULTISCATTERING_LUT_SAMPLER, linearSampler);
  rec.bindTexture(MULTISCATTERING_LUT_OUTPUT, m_multiscatteringLut);


  rec.setPass(PassId::multiscattering_lut);
  
  // 8x8 thread groups over both halves.
  uint32_t groupsX = (kMultiscatteringLutSize + 7) / 8;
  uint32_t groupsY = (2 * kMultiscatteringLutSize + 7) / 8;
  rec.dispatch(groupsX, groupsY, 1);
}

void Renderer::Impl::dispatchSkyViewLut(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Sky View LUT");

  AtmosphereArgs args = getAtmosphereArgs();
  
  rec.bindConstants(SKY_VIEW_LUT_ATMOSPHERE_ARGS, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(SKY_VIEW_LUT_TRANSMITTANCE_INPUT, m_transmittanceLut);
  rec.bindTexture(SKY_VIEW_LUT_MULTISCATTERING_INPUT, m_multiscatteringLut);
  rec.bindTexture(SKY_VIEW_LUT_STARLIGHT_LOW, m_nightSkyStarlightLow);

  const GpuHandle linearSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge, 0.0f);
  rec.bindSampler(SKY_VIEW_LUT_SAMPLER, linearSampler);
  rec.bindTexture(SKY_VIEW_LUT_OUTPUT, m_skyViewLut);


  rec.setPass(PassId::sky_view_lut);

  // 16x16 thread groups.
  uint32_t groupsX = (kSkyViewLutWidth + 15) / 16;
  uint32_t groupsY = (kSkyViewLutHeight + 15) / 16;
  rec.dispatch(groupsX, groupsY, 1);
}

void Renderer::Impl::setAerialPerspectiveCamera() {
  m_apCameraPosition = m_inputs.cameraPosition;
  m_apCameraForward = normalize(m_inputs.cameraForward);

  // Match the shader's inverse projection, including signed axes and off-center frusta.
  const float* projectionToView = m_inputs.projectionToView;
  const float* viewToWorld = m_inputs.viewToWorld;
  const Vector2 corners[] = {
    Vector2(-1.0f, -1.0f), Vector2(1.0f, -1.0f), Vector2(1.0f, 1.0f), Vector2(-1.0f, 1.0f)
  };
  Vector3 rays[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const Vector4 view = mulColumnMajor(projectionToView, Vector4(corners[i].x, corners[i].y, 1.0f, 1.0f));
    rays[i] = normalize(mulColumnMajor(viewToWorld, Vector4(view.x, view.y, view.z, 0.0f)).xyz());
  }
  const Vector3 centerRay = rays[0] + rays[1] + rays[2] + rays[3];
  for (uint32_t i = 0; i < 4; ++i) {
    const Vector3 normal = cross(rays[i], rays[(i + 1) % 4]);
    const float normalLength = length(normal);
    m_apFrustumPlanes[i] = normalLength > 1e-6f ? normal / normalLength : Vector3(0.0f);
    if (dot(m_apFrustumPlanes[i], centerRay) < 0.0f) {
      m_apFrustumPlanes[i] = -m_apFrustumPlanes[i];
    }
  }
}

namespace {
  // The lightning flash's emitter as a sphere light (the scene light and the haze glow).
  constexpr float kLightningRadiusKm = 0.15f;

  // Reduce one scene light to the sphere the froxel grid integrates it as, or report that it has no
  // business being there. `equivalentRadius` is matched on area, not extent: a volume point has no
  // normal, so all that survives of an emitter's shape is the solid angle it subtends, and
  // sqrt(area / pi) is the sphere radius that reproduces it in the far field.
  bool describeAerialPerspectiveLight(
    const LocalLight& light, Vector3& position, Vector3& radiance, float& equivalentRadius,
    const LocalLight*& shaping) {
    shaping = nullptr;
    position = light.position;
    radiance = light.radiance;

    switch (light.type) {
    case LocalLight::Type::Sphere:
      equivalentRadius = light.radius;
      shaping = &light;
      return true;
    case LocalLight::Type::Rect: {
      const Vector2 dimensions = light.dimensions;
      equivalentRadius = std::sqrt(std::max(dimensions.x * dimensions.y, 0.0f) / kPi);
      shaping = &light;
      return true;
    }
    case LocalLight::Type::Disk: {
      const Vector2 halfDimensions = light.dimensions;
      // Area of the ellipse is pi * a * b, so the area-matched sphere radius is just sqrt(a * b).
      equivalentRadius = std::sqrt(std::max(halfDimensions.x * halfDimensions.y, 0.0f));
      shaping = &light;
      return true;
    }
    case LocalLight::Type::Cylinder:
      // Lateral area 2 * pi * r * h, so the area-matched radius is sqrt(2 * r * h). Unshaped.
      equivalentRadius = std::sqrt(std::max(2.0f * light.radius * light.axisLength, 0.0f));
      return true;
    }
    // Distant lights reach the air through the atmospheric term; hosts must not pass them.
    return false;
  }
}

Renderer::Impl::ApGrid Renderer::Impl::apGrid() const {
  // The render resolution, not the output: the grid is a property of the shading it hazes.
  uint32_t width = m_inputs.renderWidth;
  uint32_t height = m_inputs.renderHeight;
  if (width == 0u || height == 0u) {
    width = m_inputs.camera.resolution.x;
    height = m_inputs.camera.resolution.y;
  }
  const uint32_t size = static_cast<uint32_t>(std::clamp(m_settings.aerialPerspectiveFroxelSizePixels, 2, 64));
  ApGrid grid;
  grid.x = std::max((width + size - 1u) / size, 1u);
  grid.y = std::max((height + size - 1u) / size, 1u);
  grid.z = static_cast<uint32_t>(std::clamp(m_settings.aerialPerspectiveFroxelDepthSlices, 16, 256));
  // A NUMOS length (metres) into the host's units, where the grid lives.
  grid.range = std::max(m_settings.aerialPerspectiveFroxelRangeMeters, 1.0f) * 0.001f * hazeWorldUnitsPerKm();
  return grid;
}

// Gather the frame's positional lights into the compact form the froxel grid samples. On the CPU
// so the ranking and the frustum reject happen before anything is uploaded, and the cull pass
// iterates only the lights that can matter.
void Renderer::Impl::buildAerialPerspectiveLights(PassRecorder& rec) {
  m_apLightCount = 0u;

  const int maxLightCount = m_settings.aerialPerspectiveLocalLightMaxCount;
  const uint32_t hostLightCount = m_inputs.localLights != nullptr ? m_inputs.localLightCount : 0u;
  // The lightning flash lights the haze as well as the clouds and the scene.
  const bool flash = m_settings.lightningEnable && m_lightningEnvelope > 0.0f && lightningIntensityKm2() > 0.0f;
  if (!m_settings.aerialPerspectiveLocalLights || maxLightCount <= 0 || (hostLightCount == 0u && !flash)) {
    return;
  }

  const float cutoff = std::max(m_settings.aerialPerspectiveLocalLightCutoff, 1e-6f);
  // Nothing past the froxel range receives local light, so no light needs to reach further.
  const float maxInfluence = std::max(apGrid().range, 1.0f);

  struct RankedLight {
    AerialPerspectiveLight light;
    float score;
  };

  std::vector<RankedLight> ranked;
  ranked.reserve(std::min<size_t>(hostLightCount + 1u, static_cast<size_t>(maxLightCount) * 2u));

  for (uint32_t lightIndex = 0; lightIndex < hostLightCount; ++lightIndex) {
    const LocalLight* sceneLight = &m_inputs.localLights[lightIndex];

    Vector3 position;
    Vector3 radiance;
    float equivalentRadius = 0.0f;
    const LocalLight* shaping = nullptr;
    if (!describeAerialPerspectiveLight(*sceneLight, position, radiance, equivalentRadius, shaping)) {
      continue;
    }

    const float peakRadiance = std::max(std::max(radiance.x, radiance.y), radiance.z);
    if (!(peakRadiance > 0.0f) || !(equivalentRadius > 0.0f)) {
      continue;
    }

    // Distance at which this light's incident radiance falls to the cutoff (pi r^2 / d^2 far field):
    // a torch is culled after a few metres while a floodlight survives to a hundred.
    const float influenceRadius = std::min(
      equivalentRadius * std::sqrt(kPi * peakRadiance / cutoff), maxInfluence);

    // Reject anything whose influence sphere cannot touch the grid: planes rather than a bounding
    // sphere, because the frustum is long and narrow and a sphere around it would reject nothing.
    const Vector3 relative = position - m_apCameraPosition;
    const float forward = dot(relative, m_apCameraForward);
    if (forward + influenceRadius < 0.0f || forward - influenceRadius > maxInfluence) {
      continue;
    }
    bool outsideFrustum = false;
    for (const Vector3& plane : m_apFrustumPlanes) {
      outsideFrustum |= dot(relative, plane) < -influenceRadius;
    }
    if (outsideFrustum) {
      continue;
    }

    AerialPerspectiveLight entry = {};
    entry.position = { position.x, position.y, position.z };
    entry.influenceRadius = influenceRadius;
    entry.falloffWindow = m_settings.aerialPerspectiveLocalLightWindow ? 1.0f : 0.0f;
    entry.radiance = { radiance.x, radiance.y, radiance.z };
    entry.equivalentRadius = equivalentRadius;
    if (shaping != nullptr && shaping->shaped) {
      const Vector3 axis = shaping->shapingDirection;
      entry.coneAxis = { axis.x, axis.y, axis.z };
      entry.cosConeAngle = shaping->cosConeAngle;
      entry.coneSoftness = shaping->coneSoftness;
      entry.focusExponent = shaping->focusExponent;
    } else {
      // A zero axis is the shader's unshaped marker; see aerialPerspectiveLightShaping.
      entry.coneAxis = { 0.0f, 0.0f, 0.0f };
    }

    // Rank by the radiance the light delivers at the nearest point of its reach to the camera, a
    // stand-in for how much of the screen it can affect. A light around the camera scores its peak.
    const float distanceToCamera = length(relative);
    const float clearance = std::max(distanceToCamera - influenceRadius, 0.0f);
    const float score = peakRadiance * equivalentRadius * equivalentRadius
      / std::max(clearance * clearance, equivalentRadius * equivalentRadius);
    ranked.push_back({ entry, score });
  }

  // The flash: the same sphere as the scene light (syncDistantLights), physical like the host lights
  // here (apLocalLightIntensity carries the pre-exposure). It reaches the whole grid wherever it
  // strikes and is never culled while it lasts, so its glow fades with the flash rather than
  // stopping at a cutoff, and it outranks every host light.
  if (flash) {
    const Vector3 position = lightningRenderPosition();
    const Vector3 radiance = lightningHue()
      * (m_lightningEnvelope * lightningIntensityKm2() / (kPi * kLightningRadiusKm * kLightningRadiusKm));
    AerialPerspectiveLight entry = {};
    entry.position = { position.x, position.y, position.z };
    entry.influenceRadius = length(position - m_apCameraPosition) + maxInfluence;
    entry.falloffWindow = 0.0f;
    entry.radiance = { radiance.x, radiance.y, radiance.z };
    entry.equivalentRadius = kLightningRadiusKm * worldUnitsPerKm();
    entry.coneAxis = { 0.0f, 0.0f, 0.0f };
    ranked.push_back({ entry, std::numeric_limits<float>::max() });
  }

  if (ranked.empty()) {
    return;
  }

  // Partial sort: the cull pass relies only on the kept prefix being ordered, so an overflowing
  // cluster drops its dimmest lights.
  const size_t keep = std::min(ranked.size(), static_cast<size_t>(maxLightCount));
  std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
    [](const RankedLight& a, const RankedLight& b) { return a.score > b.score; });

  std::vector<AerialPerspectiveLight> packed;
  packed.reserve(keep);
  for (size_t i = 0; i < keep; ++i) {
    packed.push_back(ranked[i].light);
  }

  // Grow-only: a buffer reallocated on every shrink would orphan one a command list still uses.
  if (!m_apLightBuffer.isValid() || m_apLightCapacity < keep) {
    const uint32_t capacity = std::max(static_cast<uint32_t>(keep), 64u);
    replaceBuffer(rec, m_apLightBuffer, createBuffer(rec, "Atmosphere AP Lights",
      BufferUsage::Structured, sizeof(AerialPerspectiveLight) * capacity));
    m_apLightCapacity = capacity;
  }
  rec.gpu().updateBuffer(m_apLightBuffer.handle, 0, sizeof(AerialPerspectiveLight) * keep, packed.data());
  m_apLightCount = static_cast<uint32_t>(keep);
}

void Renderer::Impl::ensureAerialPerspectiveResources(PassRecorder& rec, const ApGrid& grid) {
  if (!m_apFarScatter.isValid()) {
    replaceTexture(rec, m_apFarScatter, createTexture(rec, "Atmosphere AP Far Scatter", TextureType::Tex3D,
      Format::RGBA16Float, kApFarSizeX, kApFarSizeY, kApFarSlices, 1));
    replaceTexture(rec, m_apFarTransmittance, createTexture(rec, "Atmosphere AP Far Transmittance", TextureType::Tex3D,
      Format::RGBA16Float, kApFarSizeX, kApFarSizeY, kApFarSlices, 1));
    m_apFarValid = false;
  }
  if (!m_apFarUsage.isValid()) {
    replaceBuffer(rec, m_apFarUsage, createBuffer(rec, "Atmosphere AP Far Usage", BufferUsage::Structured, 32u));
    for (Buffer& readback : m_apFarUsageReadback) {
      replaceBuffer(rec, readback, createBuffer(rec, "Atmosphere AP Far Usage Readback", BufferUsage::Readback, 16u));
    }
  }

  const Texture& near = m_apNearScatter;
  if (near.isValid() && near.desc.width == grid.x && near.desc.height == grid.y && near.desc.depth == grid.z) {
    return;
  }
  // Any resize invalidates the history; the old images stay alive while a command list uses them.
  replaceTexture(rec, m_apNearScatter, createTexture(rec, "Atmosphere AP Near Scatter", TextureType::Tex3D,
    Format::RGBA16Float, grid.x, grid.y, grid.z, 1));
  for (uint32_t i = 0; i < 2; ++i) {
    replaceTexture(rec, m_apFroxelLighting[i], createTexture(rec,
      i == 0 ? "Atmosphere AP Froxel Lighting 0" : "Atmosphere AP Froxel Lighting 1", TextureType::Tex3D,
      Format::RGBA16Float, grid.x, grid.y, grid.z, 1));
    // RGBA16F, not RGBA8: an 8-bit visibility blended toward 0 never got there (4.2, "Formats").
    replaceTexture(rec, m_apFroxelAux[i], createTexture(rec,
      i == 0 ? "Atmosphere AP Froxel Aux 0" : "Atmosphere AP Froxel Aux 1", TextureType::Tex3D,
      Format::RGBA16Float, grid.x, grid.y, grid.z, 1));
  }
  replaceTexture(rec, m_apLightInjection, createTexture(rec, "Atmosphere AP Light Injection", TextureType::Tex3D,
    Format::RGBA16Float, grid.x, grid.y, grid.z, 1));
  const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  rec.gpu().clearTexture(m_apLightInjection.handle, zero);
  m_apHistoryValid = false;
}

// Far LUT and light cull (docs/aerial-perspective-v2.md). The froxel lighting and integration need
// this frame's linear view Z, so they wait for dispatchPostDepth (dispatchAerialPerspectiveNear).
void Renderer::Impl::dispatchAerialPerspective(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Aerial Perspective");

  const ApGrid grid = apGrid();
  ensureAerialPerspectiveResources(rec, grid);

  // The gather sets the light count the args publish; everything below reads it. The cloud args
  // put the key light (the sun, or the moon at night) in the sun slot.
  buildAerialPerspectiveLights(rec);
  AtmosphereArgs args = getCloudArgs();

  // History is only meaningful for the same grid over consecutive frames.
  const ApHistoryKey key { grid.x, grid.y, grid.z, args.apFroxelRange, args.apWorldUnitsPerKm,
                           args.fogExtinctionPerKm, args.fogScaleHeightKm, args.apSceneShadow };
  const bool historyUsable = m_apHistoryValid && !m_inputs.cameraCut
    && m_apLastFrame + 1u == m_inputs.frameIndex && std::memcmp(&key, &m_apHistoryKey, sizeof(key)) == 0;
  args.apHistoryReset = historyUsable ? 0u : 1u;
  m_apHistoryKey = key;

  const GpuHandle lutSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge);
  const GpuHandle cloudSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::Repeat, 0.0f);

  // ---- far field ----
  // Takes in the far-field counts of earlier frames that have come back. The last frame's count is
  // copied out and the counter cleared here rather than after the integration, so the transfers land
  // among this frame's bakes instead of between the screen passes. Without a free readback slot, that
  // count is dropped.
  for (uint32_t i = 0; i < kApFarUsageSlots; ++i) {
    uint32_t usage[4];
    if (m_apFarUsagePending[i] && rec.gpu().readBuffer(m_apFarUsageReadback[i].handle, usage, sizeof(usage))) {
      m_apFarUsagePending[i] = false;
      m_apFarIdleFrames = usage[0] == 0u ? std::min(m_apFarIdleFrames + 1u, kApFarIdleFrames) : 0u;
    }
  }
  if (m_apFarUsageCounted) {
    m_apFarUsageCounted = false;
    for (uint32_t i = 0; i < kApFarUsageSlots; ++i) {
      if (!m_apFarUsagePending[i]) {
        rec.gpu().copyBuffer(m_apFarUsageReadback[i].handle, 0, m_apFarUsage.handle, 0, 16u);
        m_apFarUsagePending[i] = true;
        break;
      }
    }
  }
  {
    // A partial write (the buffer has a tail pad), so a host may update it in place.
    const uint32_t zero[4] = { 0u, 0u, 0u, 0u };
    rec.gpu().updateBuffer(m_apFarUsage.handle, 0, sizeof(zero), zero);
  }
  // A far field nothing has read for a while is refreshed every kApFarIdlePeriod frames, so the
  // view that next needs it starts from one at most that old and is current again once the count
  // comes back.
  const bool farIdle = m_apFarValid && m_apFarIdleFrames >= kApFarIdleFrames && !m_inputs.cameraCut;
  if (!farIdle || m_inputs.frameIndex - m_apFarUpdateFrame >= kApFarIdlePeriod) {
    GpuZone inner(rec.gpu(), "Atmosphere AP Far LUT");
    m_apFarUpdateFrame = m_inputs.frameIndex;
    m_apFarValid = true;
    rec.bindConstants(AP_FAR_ATMOSPHERE_ARGS, &args, sizeof(AtmosphereArgs));
    rec.bindTexture(AP_FAR_TRANSMITTANCE_INPUT, m_transmittanceLut);
    rec.bindTexture(AP_FAR_MULTISCATTERING_INPUT, m_multiscatteringLut);
    rec.bindSampler(AP_FAR_LUT_SAMPLER, lutSampler);
    rec.bindTexture(AP_FAR_CLOUD_SHADOW_MAP, m_cloudShadowMap);
    rec.bindSampler(AP_FAR_CLOUD_SAMPLER, cloudSampler);
    rec.bindTexture(AP_FAR_SCATTER_OUTPUT, m_apFarScatter);
    rec.bindTexture(AP_FAR_TRANSMITTANCE_OUTPUT, m_apFarTransmittance);
    rec.setPass(PassId::aerial_perspective_far);
    rec.dispatch(kApFarSizeX, kApFarSizeY, 1u);  // one group per direction (parallel slice scan)
  }

  // ---- light cull: every cluster's count is written whenever there are lights ----
  const uint32_t tilesX = args.apLocalLightTilesX;
  const uint32_t tilesY = args.apLocalLightTilesY;
  // Per-slice clusters, then one per-tile list over the whole range (the composite's).
  const uint32_t requiredUints = tilesX * tilesY * (grid.z + 1u) * AERIAL_PERSPECTIVE_LIGHT_CLUSTER_STRIDE;
  if (!m_apLightClusterBuffer.isValid() || m_apLightClusterCapacity < requiredUints) {
    replaceBuffer(rec, m_apLightClusterBuffer, createBuffer(rec, "Atmosphere AP Light Clusters",
      BufferUsage::Structured, sizeof(uint32_t) * requiredUints));
    m_apLightClusterCapacity = requiredUints;
  }
  if (!m_apLightBuffer.isValid()) {
    // The lighting pass declares the buffer whether or not any light survived.
    replaceBuffer(rec, m_apLightBuffer, createBuffer(rec, "Atmosphere AP Lights",
      BufferUsage::Structured, sizeof(AerialPerspectiveLight) * 64u));
    m_apLightCapacity = 64u;
  }
  if (args.apLocalLightCount != 0u) {
    GpuZone inner(rec.gpu(), "Atmosphere AP Light Cull");
    rec.bindConstants(AP_CULL_ATMOSPHERE_ARGS, &args, sizeof(AtmosphereArgs));
    rec.bindConstants(AP_CULL_CAMERA, &m_inputs.camera, sizeof(Camera));
    rec.bindBuffer(AP_CULL_LIGHTS, m_apLightBuffer);
    rec.bindBuffer(AP_CULL_CLUSTERS, m_apLightClusterBuffer);
    rec.setPass(PassId::aerial_perspective_light_cull);
    rec.dispatch((tilesX + 3u) / 4u, (tilesY + 3u) / 4u, (grid.z + 1u + 3u) / 4u);
    rec.barrier();
  }

  m_apNearArgs = args;
  m_apNearGrid = grid;
  m_apNearFrame = m_inputs.frameIndex;
  m_apNearPending = true;
}

// Froxel lighting and integration, in dispatchPostDepth: every froxel sample stays in front of the
// visible surface in `linearViewZ` (docs/aerial-perspective-v2.md 4.2.2). Without depth (0) they run
// as before, unclamped.
void Renderer::Impl::dispatchAerialPerspectiveNear(PassRecorder& rec, GpuHandle linearViewZ, float missLinearViewZ) {
  if (!m_apNearPending) {
    return;
  }
  m_apNearPending = false;
  GpuZone zone(rec.gpu(), "Atmosphere Aerial Perspective Near");
  const AtmosphereArgs& args = m_apNearArgs;
  const ApGrid grid = m_apNearGrid;
  const GpuHandle lutSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge);
  const GpuHandle cloudSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::Repeat, 0.0f);

  // The passes declare the depth whether or not the host has one.
  if (linearViewZ == 0 && !m_apNoDepth.isValid()) {
    replaceTexture(rec, m_apNoDepth, createTexture(rec, "Atmosphere AP No Depth", TextureType::Tex2D,
      Format::R32Float, 1, 1, 1, 1));
    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    rec.gpu().clearTexture(m_apNoDepth.handle, zero);
  }
  const GpuHandle depth = linearViewZ != 0 ? linearViewZ : m_apNoDepth.handle;
  // ApSurfaceArgs (lib/aerial_perspective.slangh).
  const float surfaceArgs[4] = { linearViewZ != 0 ? 1.0f : 0.0f, missLinearViewZ, 0.0f, 0.0f };

  // Covers the cull pass's clusters and the host's linear view Z write.
  rec.barrier();

  // ---- froxel lighting: previous history -> current ----
  const uint32_t current = m_apHistoryIndex ^ 1u;
  {
    GpuZone inner(rec.gpu(), "Atmosphere AP Froxel Lighting");
    const bool traced = m_inputs.opaqueTlas != 0
      && (args.apSceneShadow != 0u || (args.apLocalLightShadows != 0u && args.apLocalLightCount != 0u));
    rec.bindConstants(AP_LIGHTING_ATMOSPHERE_ARGS, &args, sizeof(AtmosphereArgs));
    rec.bindConstants(AP_LIGHTING_CAMERA, &m_inputs.camera, sizeof(Camera));
    if (traced) {
      rec.bindAccelerationStructure(AP_LIGHTING_ACCELERATION_STRUCTURE, m_inputs.opaqueTlas);
    }
    rec.bindBuffer(AP_LIGHTING_LIGHTS, m_apLightBuffer);
    rec.bindBuffer(AP_LIGHTING_CLUSTERS, m_apLightClusterBuffer);
    rec.bindTexture(AP_LIGHTING_HISTORY, m_apFroxelLighting[m_apHistoryIndex]);
    rec.bindTexture(AP_LIGHTING_HISTORY_AUX, m_apFroxelAux[m_apHistoryIndex]);
    rec.bindSampler(AP_LIGHTING_SAMPLER, lutSampler);
    rec.bindTexture(AP_LIGHTING_OUTPUT, m_apFroxelLighting[current]);
    rec.bindTexture(AP_LIGHTING_OUTPUT_AUX, m_apFroxelAux[current]);
    rec.bindTexture(AP_LIGHTING_INJECTION, m_apLightInjection);
    rec.bindTexture(AP_LIGHTING_LINEAR_VIEW_Z, depth);
    rec.bindConstants(AP_LIGHTING_SURFACE_ARGS, surfaceArgs, sizeof(surfaceArgs));
    rec.setPass(traced ? PassId::aerial_perspective_froxel_lighting_traced : PassId::aerial_perspective_froxel_lighting);
    rec.dispatch((grid.x + 7u) / 8u, (grid.y + 7u) / 8u, grid.z);
  }
  rec.barrier();

  // ---- froxel integration ----
  // The integration also counts the columns whose pixels take the far field. The next beginFrame
  // copies the count out and clears the counter (dispatchAerialPerspective).
  {
    GpuZone inner(rec.gpu(), "Atmosphere AP Froxel Integration");
    rec.bindConstants(AP_INTEGRATE_ATMOSPHERE_ARGS, &args, sizeof(AtmosphereArgs));
    rec.bindConstants(AP_INTEGRATE_CAMERA, &m_inputs.camera, sizeof(Camera));
    rec.bindTexture(AP_INTEGRATE_TRANSMITTANCE_INPUT, m_transmittanceLut);
    rec.bindTexture(AP_INTEGRATE_MULTISCATTERING_INPUT, m_multiscatteringLut);
    rec.bindSampler(AP_INTEGRATE_LUT_SAMPLER, lutSampler);
    rec.bindTexture(AP_INTEGRATE_LIGHTING, m_apFroxelLighting[current]);
    rec.bindTexture(AP_INTEGRATE_LIGHTING_AUX, m_apFroxelAux[current]);
    rec.bindTexture(AP_INTEGRATE_CLOUD_SHADOW_MAP, m_cloudShadowMap);
    rec.bindSampler(AP_INTEGRATE_CLOUD_SAMPLER, cloudSampler);
    rec.bindTexture(AP_INTEGRATE_OUTPUT, m_apNearScatter);
    rec.bindTexture(AP_INTEGRATE_LINEAR_VIEW_Z, depth);
    rec.bindConstants(AP_INTEGRATE_SURFACE_ARGS, surfaceArgs, sizeof(surfaceArgs));
    rec.bindBuffer(AP_INTEGRATE_FAR_USAGE, m_apFarUsage);
    rec.setPass(PassId::aerial_perspective_froxel_integrate);
    rec.dispatch((grid.x + 7u) / 8u, (grid.y + 7u) / 8u, 1u);
  }
  m_apFarUsageCounted = true;

  m_apHistoryIndex = current;
  m_apHistoryValid = true;
  m_apLastFrame = m_apNearFrame;
}

void Renderer::Impl::dispatchCloudSkyTransmittanceLut(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud Sky Transmittance LUT");

  AtmosphereArgs args = getAtmosphereArgs();

  // Slot 0 args, 1 the RGBA16F output, 2-3 the dome LUT it reduces (mip 0) and a linear/clamp
  // sampler (sampleCloudDomeLut wraps azimuth itself).
  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(1, m_cloudSkyTransmittanceLut);
  rec.bindTexture(2, m_cloudSecondaryLut);
  rec.bindSampler(3, sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge, 0.0f));

  rec.setPass(PassId::cloud_sky_transmittance_lut);

  // The shader declares [numthreads(8, 8, 1)].
  uint32_t groupsX = (kCloudSkyTransmittanceLutWidth + 7) / 8;
  uint32_t groupsY = (kCloudSkyTransmittanceLutHeight + 7) / 8;
  rec.dispatch(groupsX, groupsY, 1);
}

// Cloud shadow maps (lib/cloud_shadow_map.slangh, cloud_shadow_map.comp.slang): the sun map and the
// horizon map, four cascades each. Both live in the cloud frame, where the body field does not move,
// so wind costs nothing. A cascade is re-baked when the sun (for the horizon map, its azimuth) has
// turned past cloudShadowMapRebakeDegrees, the camera has drifted a quarter of the cascade's window,
// or the cloud field changed; one cascade at a time, cloudShadowMapBakeRowsPerFrame rows per frame,
// into its map's spare slot, which it takes over when done. Lookups read the old slot meanwhile.
Renderer::Impl::CloudShadowCascade Renderer::Impl::cloudShadowCascadeFor(uint32_t map, uint32_t k,
                                                                         const AtmosphereArgs& current) const {
  Vector3 sun { current.sunDirection.x, current.sunDirection.y, current.sunDirection.z };
  const float sunLength = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
  sun = sunLength > 0.0f ? Vector3 { sun.x / sunLength, sun.y / sunLength, sun.z / sunLength }
                         : Vector3 { 0.0f, 1.0f, 0.0f };
  // Plane basis, as cloudShadowBasis: S along the map, U horizontal, V = U x S.
  const float horizontal = std::sqrt(sun.x * sun.x + sun.z * sun.z);
  const float ax = horizontal > 1e-4f ? sun.x / horizontal : 1.0f;
  const float az = horizontal > 1e-4f ? sun.z / horizontal : 0.0f;
  // The horizon map runs along the horizontal toward the sun's azimuth: elevation 0.
  const float sinE = map == 0u ? sun.y : 0.0f;
  const float cosE = map == 0u ? horizontal : 1.0f;

  CloudShadowCascade cc;
  cc.dir = map == 0u ? sun : Vector3 { ax, 0.0f, az };
  cc.bakeCameraCloudXZ = Vector2 { current.cameraWorldPosYUpKm.x - current.cloudWindOffset.x,
                                   current.cameraWorldPosYUpKm.z - current.cloudWindOffset.y };
  // Camera on the plane: u = c . U with U = (-az, 0, ax); along = c . A.
  const float camAlong = cc.bakeCameraCloudXZ.x * ax + cc.bakeCameraCloudXZ.y * az;
  const float camU = -cc.bakeCameraCloudXZ.x * az + cc.bakeCameraCloudXZ.y * ax;
  // The slab around the camera: the planet centre sits below the camera, the ground at y = 0.
  const float baseY = current.cloudAltitude;
  const float topY = current.cloudAltitude + current.cloudThickness;
  static constexpr float kCascadeScale[4] = { 1.0f, 3.3f, 11.0f, 40.0f };
  const float radiusKm = std::max(m_settings.cloudShadowMapWindowKm, 0.1f) * kCascadeScale[k];
  // Slab points within radiusKm of the camera: y in [base - curvature drop, top], position along
  // the azimuth in camAlong +- radius; v = y cosE - along sinE.
  const float dropKm = radiusKm * radiusKm / (2.0f * current.planetRadius);
  const float vMin = (baseY - dropKm) * cosE - (camAlong + radiusKm) * sinE;
  const float vMax = topY * cosE - (camAlong - radiusKm) * sinE;
  cc.halfU = radiusKm * (1.0f + 2.0f / float(kCloudShadowMapSize));
  cc.halfV = 0.5f * (vMax - vMin) * 1.05f + 2.0f * radiusKm / float(kCloudShadowMapSize);
  // Centres snapped to the cascade's texels, so re-bakes resample the same rays.
  const float texelU = 2.0f * cc.halfU / float(kCloudShadowMapSize);
  const float texelV = 2.0f * cc.halfV / float(kCloudShadowMapSize);
  cc.centerU = std::round(camU / texelU) * texelU;
  cc.centerV = std::round(0.5f * (vMin + vMax) / texelV) * texelV;
  cc.valid = true;
  cc.stale = false;
  return cc;
}

void Renderer::Impl::dispatchCloudShadowMapRows(PassRecorder& rec, uint32_t map, uint32_t k,
                                                const CloudShadowCascade& params, uint32_t slot,
                                                uint32_t row0, uint32_t rows) {
  AtmosphereArgs args = getCloudArgs();
  const auto setLane = [k](vec4& v, float value) { (&v.x)[k] = value; };
  setLane(args.cloudShadowDirX[map], params.dir.x);
  setLane(args.cloudShadowDirY[map], params.dir.y);
  setLane(args.cloudShadowDirZ[map], params.dir.z);
  setLane(args.cloudShadowCenterU[map], params.centerU);
  setLane(args.cloudShadowHalfU[map], params.halfU);
  setLane(args.cloudShadowCenterV[map], params.centerV);
  setLane(args.cloudShadowHalfV[map], params.halfV);
  args.cloudShadowBakeTarget = map | (k << 4u) | (slot << 8u);
  args.cloudShadowBakeRow0 = row0;
  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(1, m_cloudShadowMap);
  rec.bindSampler(2, sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::Repeat, 0.0f));
  rec.bindTexture(3, m_cloudNvdfSdf[m_cloudNvdfSdfFront]);
  rec.bindTexture(4, m_cloudHorizonMap);
  rec.bindTexture(5, m_cloudBeerMap);
  rec.setPass(PassId::cloud_shadow_map);
  rec.dispatch((kCloudShadowMapSize + 7u) / 8u, (rows + 7u) / 8u, 1u);
}

// Layer 2 patch-field map (cloud_layer2_map.comp, lib/cloud_nubis3_common.slangh kCloudLayer2Map*).
// The fields hold still in the cloud frame, so the map is baked only when the camera has drifted
// kCloudLayer2MapRecentreKm from its centre over the clouds (camera travel plus wind) or the layer's
// seed changed. The centre snaps to the coarse texel, a multiple of the fine one, so both grids keep
// their texel positions and a re-centre bakes the same values where the old map and the new overlap.
void Renderer::Impl::updateCloudLayer2Map(PassRecorder& rec) {
  if (!m_cloudLayer2Map.isValid()) {
    return;
  }
  AtmosphereArgs args = getCloudArgs();
  CloudLayer2MapState& map = m_cloudLayer2MapState;
  const float camX = args.cameraWorldPosYUpKm.x - args.cloudWindOffset.x;
  const float camZ = args.cameraWorldPosYUpKm.z - args.cloudWindOffset.y;
  const float driftKm = std::sqrt((camX - map.centerX) * (camX - map.centerX) + (camZ - map.centerZ) * (camZ - map.centerZ));
  if (map.valid && map.seed == args.cloudLayer2NoiseSeed && driftKm <= kCloudLayer2MapRecentreKm) {
    return;
  }
  map.centerX = std::round(camX / kCloudLayer2MapCoarseTexelKm) * kCloudLayer2MapCoarseTexelKm;
  map.centerZ = std::round(camZ / kCloudLayer2MapCoarseTexelKm) * kCloudLayer2MapCoarseTexelKm;
  map.seed = args.cloudLayer2NoiseSeed;
  map.valid = true;
  args.cloudLayer2MapCenterX = map.centerX;
  args.cloudLayer2MapCenterZ = map.centerZ;

  GpuZone zone(rec.gpu(), "Atmosphere Cloud Layer 2 Map Bake");
  const GpuHandle cloudSampler = sampler(rec.gpu(), Filter::Linear, Filter::Linear, AddressMode::Repeat, SamplerDesc::kLodClampNone);
  rec.barrier();
  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(1, cloudDensityNoise3D());
  rec.bindSampler(2, cloudSampler);
  rec.bindTexture(3, m_cloudLayer2Map);
  rec.setPass(PassId::cloud_layer2_map);
  rec.dispatch((kCloudLayer2MapSize + 7u) / 8u, (kCloudLayer2MapSize + 7u) / 8u, 2u);
}

void Renderer::Impl::updateCloudShadowMap(PassRecorder& rec) {
  if (!m_cloudShadowMap.isValid() || !m_cloudHorizonMap.isValid() || !m_cloudVerticalOd.isValid()) {
    return;
  }
  const AtmosphereArgs current = getCloudArgs();
  CloudShadowMapState& sm = m_cloudShadow;
  const float windowKm = std::max(m_settings.cloudShadowMapWindowKm, 0.1f);

  // The cloud field: any change makes every cascade stale (re-baked in turn, not at once). Weather
  // drifts coverage every frame; a 1/512 step bounds the error without re-baking per frame.
  const float key[13] = {
    current.planetRadius, current.cloudAltitude, current.cloudThickness,
    std::round(current.cloudCoverageMean * 512.0f) / 512.0f,
    current.nvdfNominalCoverage, current.nvdfCoverageOffsetKm, current.nvdfProfileDepthKm,
    current.cloudNoiseTileKm, current.cloudHexTilingEnable, windowKm, float(m_cloudNvdfPublishCount),
    1.0f /* unused */, float(current.cloudKeyLight) };
  if (!sm.keyValid || memcmp(key, sm.key, sizeof(key)) != 0) {
    // A different slab or window size changes every window, and a new key light (sun -> moon at dusk)
    // every direction: re-bake at once, not in turn.
    const bool geometryChanged = !sm.keyValid || key[0] != sm.key[0] || key[1] != sm.key[1]
                              || key[2] != sm.key[2] || key[9] != sm.key[9] || key[12] != sm.key[12];
    for (auto& mapCascades : sm.cascade) {
      for (CloudShadowCascade& cc : mapCascades) {
        cc.stale = true;
        if (geometryChanged) {
          cc.valid = false;
        }
      }
    }
    if (geometryChanged) {
      sm.jobActive = false;
    }
    memcpy(sm.key, key, sizeof(key));
    sm.keyValid = true;

    // The vertical optical depth reads only the field and the slab: re-run it whole, now (cheap).
    GpuZone verticalZone(rec.gpu(), "Atmosphere Cloud Vertical Optical Depth");
    const AtmosphereArgs args = getAtmosphereArgs();
    rec.barrier();
    rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
    rec.bindTexture(1, m_cloudNvdfSdf[m_cloudNvdfSdfFront]);
    rec.bindTexture(2, m_cloudVerticalOd);
    rec.setPass(PassId::cloud_vertical_od);
    rec.dispatch((kCloudNvdfSizeXZ + 7u) / 8u, (kCloudNvdfSizeXZ + 7u) / 8u, 1u);
  }

  GpuZone zone(rec.gpu(), "Atmosphere Cloud Shadow Map Bake");

  // A cascade with nothing baked yet is baked whole, this frame, in its own slot.
  bool barrierDone = false;
  for (uint32_t m = 0; m < 2; ++m) {
    for (uint32_t k = 0; k < kCloudShadowMapCascades; ++k) {
      CloudShadowCascade& cc = sm.cascade[m][k];
      if (cc.valid) {
        continue;
      }
      const CloudShadowCascade params = cloudShadowCascadeFor(m, k, current);
      if (!barrierDone) {
        rec.barrier();
        barrierDone = true;
      }
      dispatchCloudShadowMapRows(rec, m, k, params, k, 0u, kCloudShadowMapSize);
      cc = params;
      cc.slot = k;
      sm.spare[m] = kCloudShadowMapCascades;
    }
  }

  // Start the next re-bake: the first cascade (sun map first, finest first) that is out of date.
  if (!sm.jobActive) {
    const float rebakeDeg = m_settings.cloudShadowMapRebakeDegrees;
    const float cosLimit = std::cos(rebakeDeg * kDegreesToRadians);
    const CloudShadowCascade now0 = cloudShadowCascadeFor(0u, 0u, current);
    const CloudShadowCascade now1 = cloudShadowCascadeFor(1u, 0u, current);
    for (uint32_t m = 0; m < 2 && !sm.jobActive; ++m) {
      const Vector3 dirNow = m == 0u ? now0.dir : now1.dir;
      for (uint32_t k = 0; k < kCloudShadowMapCascades && !sm.jobActive; ++k) {
        const CloudShadowCascade& cc = sm.cascade[m][k];
        const float cosTurn = dirNow.x * cc.dir.x + dirNow.y * cc.dir.y + dirNow.z * cc.dir.z;
        const float driftX = now0.bakeCameraCloudXZ.x - cc.bakeCameraCloudXZ.x;
        const float driftZ = now0.bakeCameraCloudXZ.y - cc.bakeCameraCloudXZ.y;
        const float radiusKm = cc.halfU;
        const bool due = cc.stale || rebakeDeg <= 0.0f || cosTurn < cosLimit
                      || std::sqrt(driftX * driftX + driftZ * driftZ) > 0.25f * radiusKm;
        if (due) {
          sm.jobActive = true;
          sm.jobMap = m;
          sm.jobCascade = k;
          sm.jobRow = 0u;
          sm.jobParams = cloudShadowCascadeFor(m, k, current);
        }
      }
    }
  }

  // With a fast-moving sun, bake enough rows per frame that the cascades of both maps (re-baked in
  // turn; the horizon map turns with the sun's azimuth) stay within cloudShadowMapMaxLagDegrees.
  // Measured with the viewer's --sun-speed, a fixed 32 rows let a 1 deg/s sunset light the deck for
  // a sun about a degree higher than it was.
  {
    const Vector3 sunNow = cloudShadowCascadeFor(0u, 0u, current).dir;
    const Vector3 prev = sm.lastSunDir;
    const float cosTurn = std::clamp(sunNow.x * prev.x + sunNow.y * prev.y + sunNow.z * prev.z, -1.0f, 1.0f);
    const float turnDeg = (prev.x == 0.0f && prev.y == 0.0f && prev.z == 0.0f)
                        ? 0.0f : std::acos(cosTurn) / kDegreesToRadians;
    sm.sunDegreesPerFrame += (std::min(turnDeg, 5.0f) - sm.sunDegreesPerFrame) * 0.1f;
    sm.lastSunDir = sunNow;
  }
  if (sm.jobActive) {
    int rowsSetting = m_settings.cloudShadowMapBakeRowsPerFrame;
    const float maxLagDeg = m_settings.cloudShadowMapMaxLagDegrees;
    if (maxLagDeg > 0.0f && sm.sunDegreesPerFrame > 0.0f) {
      const float rowsForLag = float(kCloudShadowMapSize * kCloudShadowMapCascades * 2u) * sm.sunDegreesPerFrame / maxLagDeg;
      rowsSetting = std::max(rowsSetting, int(std::ceil(std::min(rowsForLag, float(kCloudShadowMapSize)))));
    }
    const uint32_t rowsPerFrame = uint32_t(std::clamp(rowsSetting, 8, int(kCloudShadowMapSize)));
    const uint32_t rows = std::min(rowsPerFrame, kCloudShadowMapSize - sm.jobRow);
    if (!barrierDone) {
      rec.barrier();
    }
    dispatchCloudShadowMapRows(rec, sm.jobMap, sm.jobCascade, sm.jobParams, sm.spare[sm.jobMap], sm.jobRow, rows);
    sm.jobRow += rows;
    if (sm.jobRow >= kCloudShadowMapSize) {
      CloudShadowCascade& cc = sm.cascade[sm.jobMap][sm.jobCascade];
      const uint32_t oldSlot = cc.slot;
      cc = sm.jobParams;
      cc.slot = sm.spare[sm.jobMap];
      sm.spare[sm.jobMap] = oldSlot;
      sm.jobActive = false;
    }
  }
}

namespace {
  // [numthreads(8, 4, 8)] -> (32, 16, 32) groups for the 256x64x256 NVDF domain.
  constexpr uint32_t kNvdfGroupsX = (CLOUD_NVDF_SIZE_XZ + 7u) / 8u;
  constexpr uint32_t kNvdfGroupsY = (CLOUD_NVDF_SIZE_Y + 3u) / 4u;
  constexpr uint32_t kNvdfGroupsZ = (CLOUD_NVDF_SIZE_XZ + 7u) / 8u;

}

void Renderer::Impl::dispatchCloudDetailNoiseBake(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud Detail Noise Bake");

  // The cumulus turbulence volume (billows, wisps, curl): a fixed pattern, so the bake reads no
  // constants. Mip 0 only: a storage descriptor takes a single-level view.
  rec.bindTexture(1, m_cloudCumulusNoise3D, 0);
  rec.setPass(PassId::cloud_cumulus_noise_baker);
  // Shader declares [numthreads(8, 8, 8)].
  const uint32_t groups = (kCloudDetailNoise3DSize + 7u) / 8u;
  rec.dispatch(groups, groups, groups);

  dispatchCloudDetailNoiseMips(rec, m_cloudCumulusNoise3D);
}

// The volume the density function reads (the cumulus turbulence volume).
const Texture& Renderer::Impl::cloudDensityNoise3D() const {
  return m_cloudCumulusNoise3D;
}

void Renderer::Impl::dispatchCloudDetailNoiseMips(PassRecorder& rec, const Texture& volume) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud Detail Noise Mips");

  // A linear tap at each destination texel's centre is the 2x2x2 box over the source level, and
  // repeat addressing wraps the periodic volume's edges into it.
  const GpuHandle boxSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::Repeat, 0.0f);

  rec.setPass(PassId::cloud_detail_noise_mip);
  for (uint32_t level = 1; level < kCloudDetailNoise3DMipLevels; ++level) {
    rec.barrier();
    rec.bindTexture(0, volume, level - 1);
    rec.bindSampler(1, boxSampler);
    rec.bindTexture(2, volume, level);
    // Shader declares [numthreads(4, 4, 4)].
    const uint32_t dim = std::max(kCloudDetailNoise3DSize >> level, 1u);
    const uint32_t groups = (dim + 3u) / 4u;
    rec.dispatch(groups, groups, groups);
  }
}

void Renderer::Impl::dispatchCloudNvdfOccupancy(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud NVDF Occupancy");

  const AtmosphereArgs& args = m_nvdfPendingArgs;

  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(1, m_cloudNvdfOccupancy);
  rec.bindTexture(2, m_cloudPlacementMap);

  // The placement map tiles at cloudNoiseTileKm and the NVDF's horizontal domain is one tile, so
  // repeat addressing keeps the voxel-centre taps continuous across the wrap seam.
  const GpuHandle placementSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::Repeat, 0.0f);
  rec.bindSampler(3, placementSampler);


  rec.setPass(PassId::cloud_nvdf_occupancy);
  rec.dispatch(kNvdfGroupsX, kNvdfGroupsY, kNvdfGroupsZ);
}

void Renderer::Impl::dispatchCloudNvdfJfaPass(PassRecorder& rec, uint32_t mode,
                                             uint32_t jumpSizeVoxels,
                                             uint32_t srcIdx, uint32_t dstIdx) {
  GpuZone zone(rec.gpu(), mode == 0u ? "Atmosphere Cloud NVDF JFA Seed" : "Atmosphere Cloud NVDF JFA Jump");

  const AtmosphereArgs& args = m_nvdfPendingArgs;

    CloudNvdfJfaArgs pushArgs = {};
  pushArgs.mode           = mode;
  pushArgs.jumpSizeVoxels = jumpSizeVoxels;
  rec.pushConstants(&pushArgs, sizeof(pushArgs));

  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(1, m_cloudNvdfOccupancy);
  rec.bindTexture(2, m_cloudNvdfJfa[srcIdx]);
  rec.bindTexture(3, m_cloudNvdfJfa[dstIdx]);


  rec.setPass(PassId::cloud_nvdf_jfa);
  rec.dispatch(kNvdfGroupsX, kNvdfGroupsY, kNvdfGroupsZ);
}

void Renderer::Impl::dispatchCloudNvdfResolve(PassRecorder& rec, uint32_t seedsIdx) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud NVDF Resolve");

  const AtmosphereArgs& args = m_nvdfPendingArgs;

  const uint32_t backIdx = 1u - m_cloudNvdfSdfFront;

  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(1, m_cloudNvdfOccupancy);
  rec.bindTexture(2, m_cloudNvdfJfa[seedsIdx]);
  rec.bindTexture(3, m_cloudNvdfSdf[backIdx]);


  rec.setPass(PassId::cloud_nvdf_resolve);
  rec.dispatch(kNvdfGroupsX, kNvdfGroupsY, kNvdfGroupsZ);
}

void Renderer::Impl::runCloudNvdfBakeFull(PassRecorder& rec) {
  // Orders the placement map bake from initialize() before the occupancy pass reads it.
  rec.barrier();

  const AtmosphereArgs currentArgs = getAtmosphereArgs();
  m_nvdfPendingArgs = currentArgs;
  m_nvdfPendingArgs.nvdfNominalCoverage = computeCloudNvdfNominalCoverage(m_settings, currentArgs.cloudCoverageMean);
  m_nvdfPendingArgs.cloudTypeMean = cloudNvdfDeckType(m_nvdfPendingArgs.nvdfNominalCoverage, currentArgs.cloudTypeMean);
  m_nvdfPendingArgs.cloudBodyBillowSizeKm = cloudNvdfBillowSizeKm(currentArgs.cloudBodyBillowSizeKm);
  m_nvdfPendingArgs.cloudBodyBillowGain = cloudNvdfBillowGain(currentArgs.cloudBodyBillowGain);
  dispatchCloudNvdfOccupancy(rec);
  rec.barrier();

  // Seed init writes ping-pong buffer 0; jump pass i reads (i % 2) and writes ((i + 1) % 2), so
  // the final seeds land in (passCount % 2).
  dispatchCloudNvdfJfaPass(rec, 0u, 0u, 1u, 0u);
  rec.barrier();
  for (uint32_t i = 0; i < kCloudNvdfJumpPassCount; ++i) {
    dispatchCloudNvdfJfaPass(rec, 1u, kCloudNvdfJumpSchedule[i], i % 2u, (i + 1u) % 2u);
    rec.barrier();
  }

  dispatchCloudNvdfResolve(rec, kCloudNvdfJumpPassCount % 2u);
  rec.barrier();
  m_cloudNvdfSdfFront = 1u - m_cloudNvdfSdfFront;
  m_nvdfPublishedNominalCoverage = m_nvdfPendingArgs.nvdfNominalCoverage;
  m_nvdfNominalCoverageValid = true;
  memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));
  ++m_cloudNvdfPublishCount;

  // Any interrupted amortized re-bake is superseded by this full chain.
  m_nvdfBakeActive = false;
  m_nvdfJumpIdx    = 0;
}

void Renderer::Impl::stepCloudNvdfBake(PassRecorder& rec) {
  if (!m_nvdfBakeActive) {
    if (!needsCloudNvdfRebake()) {
      return;
    }
    // Start a re-bake: occupancy and seed init this frame, jump passes over the following frames.
    // The key is taken at the start, so an input that changes mid-bake starts a follow-up bake
    // once this one completes.
    const AtmosphereArgs currentArgs = getAtmosphereArgs();
    m_nvdfPendingArgs = currentArgs;
    m_nvdfPendingArgs.nvdfNominalCoverage = computeCloudNvdfNominalCoverage(m_settings, currentArgs.cloudCoverageMean);
    m_nvdfPendingArgs.cloudTypeMean = cloudNvdfDeckType(m_nvdfPendingArgs.nvdfNominalCoverage, currentArgs.cloudTypeMean);
    m_nvdfPendingArgs.cloudBodyBillowSizeKm = cloudNvdfBillowSizeKm(currentArgs.cloudBodyBillowSizeKm);
    m_nvdfPendingArgs.cloudBodyBillowGain = cloudNvdfBillowGain(currentArgs.cloudBodyBillowGain);
    cacheCloudNvdfBakeInputs();
    if (m_settings.cloudProfilingLog) {
      logInfo(format("[Cloud NVDF] re-bake started: nominal coverage ", m_nvdfPendingArgs.nvdfNominalCoverage,
                     " (live ", currentArgs.cloudCoverageMean, ")"));
    }
    dispatchCloudNvdfOccupancy(rec);
    rec.barrier();
    dispatchCloudNvdfJfaPass(rec, 0u, 0u, 1u, 0u);
    rec.barrier();
    m_nvdfBakeActive = true;
    m_nvdfJumpIdx    = 0;
    return;
  }

  // Advance the jump chain a bounded number of passes per frame.
  for (uint32_t n = 0; n < kCloudNvdfJumpPassesPerFrame && m_nvdfJumpIdx < kCloudNvdfJumpPassCount; ++n) {
    dispatchCloudNvdfJfaPass(rec, 1u, kCloudNvdfJumpSchedule[m_nvdfJumpIdx],
                             m_nvdfJumpIdx % 2u, (m_nvdfJumpIdx + 1u) % 2u);
    rec.barrier();
    ++m_nvdfJumpIdx;
  }

  if (m_nvdfJumpIdx >= kCloudNvdfJumpPassCount) {
    dispatchCloudNvdfResolve(rec, kCloudNvdfJumpPassCount % 2u);
    rec.barrier();
    m_cloudNvdfSdfFront = 1u - m_cloudNvdfSdfFront;
    m_nvdfPublishedNominalCoverage = m_nvdfPendingArgs.nvdfNominalCoverage;
    m_nvdfNominalCoverageValid = true;
    memset(&m_cachedVoxelGridKey, 0, sizeof(m_cachedVoxelGridKey));
    ++m_cloudNvdfPublishCount;
    m_nvdfBakeActive = false;
    m_nvdfJumpIdx    = 0;
  }
}

// Whether the body's bake coverage is too far from the live one. The live level-set offset,
// (coverage - nominal) x nvdfCoverageOffsetKm, grows and shrinks the bodies smoothly, so while the
// coverage is moving (a weather change) the body re-bakes only once the gap passes
// kNvdfCoverageBand. Once the coverage has held still for kNvdfCoverageSettleFrames a last bake
// lands on the exact value, so a settled sky matches a fresh start. Re-baking on every small step
// would queue back-to-back bakes through a transition and swap bodies in visible jumps.
bool Renderer::Impl::nvdfCoverageStale(float gap) const {
  constexpr float kNvdfCoverageBand = 0.05f;
  constexpr uint32_t kNvdfCoverageSettleFrames = 60;
  return gap > kNvdfCoverageBand || (gap > 1e-4f && m_coverageSteadyFrames >= kNvdfCoverageSettleFrames);
}

bool Renderer::Impl::needsCloudNvdfRebake() const {
  AtmosphereArgs args = getAtmosphereArgs();
  const float desiredNominalCoverage = computeCloudNvdfNominalCoverage(m_settings, args.cloudCoverageMean);
  const float thicknessQ = std::round(args.cloudThickness / 0.25f) * 0.25f;
  return m_cachedNvdfKey.cellSizeKm      != m_settings.cloudCellSizeKm
      || m_cachedNvdfKey.tileKm          != m_settings.cloudNoiseTileKm
      || m_cachedNvdfKey.columnFeather   != m_settings.cloudColumnFeather
      || m_cachedNvdfKey.columnTopShape  != m_settings.cloudColumnTopShape
      || m_cachedNvdfKey.columnTopVar    != m_settings.cloudColumnTopVariation
      || m_cachedNvdfKey.columnBaseVar   != m_settings.cloudColumnBaseVariation
      || nvdfCoverageStale(std::abs(m_cachedNvdfKey.nominalCoverage - desiredNominalCoverage))
      || m_cachedNvdfKey.thicknessQ      != thicknessQ
      || m_cachedNvdfKey.billowStrength  != args.cloudBodyBillowStrength
      || m_cachedNvdfKey.billowSizeKm    != cloudNvdfBillowSizeKm(args.cloudBodyBillowSizeKm)
      || m_cachedNvdfKey.billowGain      != cloudNvdfBillowGain(args.cloudBodyBillowGain)
      || m_cachedNvdfKey.deckType        != cloudNvdfDeckType(desiredNominalCoverage, args.cloudTypeMean);
}

void Renderer::Impl::cacheCloudNvdfBakeInputs() {
  const AtmosphereArgs& args = m_nvdfPendingArgs;
  m_cachedNvdfKey.cellSizeKm      = args.cloudCellSizeKm;
  m_cachedNvdfKey.tileKm          = args.cloudNoiseTileKm;
  m_cachedNvdfKey.columnFeather   = args.cloudColumnFeather;
  m_cachedNvdfKey.columnTopShape  = args.cloudColumnTopShape;
  m_cachedNvdfKey.columnTopVar    = args.cloudColumnTopVariation;
  m_cachedNvdfKey.columnBaseVar   = args.cloudColumnBaseVariation;
  m_cachedNvdfKey.nominalCoverage = args.nvdfNominalCoverage;
  m_cachedNvdfKey.thicknessQ      = std::round(args.cloudThickness / 0.25f) * 0.25f;
  m_cachedNvdfKey.billowStrength  = args.cloudBodyBillowStrength;
  m_cachedNvdfKey.billowSizeKm    = args.cloudBodyBillowSizeKm;
  m_cachedNvdfKey.billowGain      = args.cloudBodyBillowGain;
  m_cachedNvdfKey.deckType        = args.cloudTypeMean;  // pending args hold cloudNvdfDeckType's value
}

void Renderer::Impl::ensureCloudRenderRT(PassRecorder& rec, uint32_t width, uint32_t height) {
  // Zero extents can occur in early frames before the host has settled its size; allocate later.
  if (width == 0u || height == 0u) {
    return;
  }

  const bool extentsMatch = (m_cloudRenderExtentWidth == width) && (m_cloudRenderExtentHeight == height);
  if (extentsMatch && m_cloudRenderRT.isValid() && m_cloudDepthRT.isValid()
      && m_cloudRenderPrevious.isValid() && m_cloudDepthPrevious.isValid() && m_cloudDenoisedRT.isValid()
      && m_cloudFreshColor.isValid() && m_cloudStateRT.isValid() && m_cloudStatePrevious.isValid()) {
    return;
  }

  // Color (premultiplied cloud radiance + transmittance) and depth companion (32-bit float: the
  // march reaches 50+ km at the horizon), each with a history copy for reprojection. Zero clear =
  // "no cloud, full transmittance"; depth is overwritten every texel every frame.
  replaceTexture(rec, m_cloudRenderRT, createTexture(rec, "Atmosphere Cloud Render RT", TextureType::Tex2D,
    Format::RGBA16Float, width, height, 1, 1));
  replaceTexture(rec, m_cloudDepthRT, createTexture(rec, "Atmosphere Cloud Depth RT", TextureType::Tex2D,
    Format::RGBA32Float, width, height, 1, 1));
  replaceTexture(rec, m_cloudRenderPrevious, createTexture(rec, "Atmosphere Cloud Render Previous", TextureType::Tex2D,
    Format::RGBA16Float, width, height, 1, 1));
  replaceTexture(rec, m_cloudDepthPrevious, createTexture(rec, "Atmosphere Cloud Depth Previous", TextureType::Tex2D,
    Format::RGBA32Float, width, height, 1, 1));
  replaceTexture(rec, m_cloudDenoisedRT, createTexture(rec, "Atmosphere Cloud Render Denoised", TextureType::Tex2D,
    Format::RGBA16Float, width, height, 1, 1));
  // The temporal filter's per-pixel state (docs/cloud-temporal-filter.md): history length in samples,
  // and the short "fast" history of luminance and transmittance its change detection compares with.
  replaceTexture(rec, m_cloudStateRT, createTexture(rec, "Atmosphere Cloud Temporal State", TextureType::Tex2D,
    Format::RGBA16Float, width, height, 1, 1));
  replaceTexture(rec, m_cloudStatePrevious, createTexture(rec, "Atmosphere Cloud Temporal State Previous",
    TextureType::Tex2D, Format::RGBA16Float, width, height, 1, 1));
  // Sixteenth march fresh samples (cloud_render's march pass -> rebuild pass), one texel per 4x4 cell.
  const uint32_t cellsW = (width + 3u) / 4u;
  const uint32_t cellsH = (height + 3u) / 4u;
  replaceTexture(rec, m_cloudFreshColor, createTexture(rec, "Atmosphere Cloud Fresh Color", TextureType::Tex2D,
    Format::RGBA16Float, cellsW, cellsH, 1, 1));
  replaceTexture(rec, m_cloudFreshDepth, createTexture(rec, "Atmosphere Cloud Fresh Depth", TextureType::Tex2D,
    Format::RGBA32Float, cellsW, cellsH, 1, 1));
  replaceTexture(rec, m_cloudFreshAux, createTexture(rec, "Atmosphere Cloud Fresh Aux", TextureType::Tex2D,
    Format::RG16Float, cellsW, cellsH, 1, 1));

  m_cloudTemporalHistoryValid = false;
  m_cloudRenderExtentWidth = width;
  m_cloudRenderExtentHeight = height;
  logInfo(format("[Cloud render] extent=", width, "x", height, " (internal resolution)"));
}

void Renderer::Impl::setCloudShadowCameraPosition(const Vector3& cameraWorldPosYUpKm) {
  m_cameraWorldPosYUpKm = cameraWorldPosYUpKm;
}

// The camera in the cloud frame (args.cameraWorldPosYUpKm): the resolved anchor, with its height
// measured from the ground datum and the cloud field's vertical offset taken out. Everything placed
// in the cloud frame (the lightning strike, numosWorldToCloudKm in the shaders) is relative to this.
Vector3 Renderer::Impl::cloudFrameCameraKm() const {
  Vector3 camera = m_cameraWorldPosYUpKm;
  camera.y -= m_groundLevelYUpKm + m_settings.cloudVerticalOffsetWorldUnits / worldUnitsPerKm();
  return camera;
}

// Integrates the cloud motion once per frame (from updateFrame) as offset += velocity * dt into
// members that getAtmosphereArgs() reads. Integrating, rather than speed * time, means a changing
// wind eases the field instead of rescaling or rotating the whole accumulated offset. The
// accumulators grow with session time; the shaders' frac() wraps them.
void Renderer::Impl::advanceCloudMotion(float dt) {
  m_cloudAdvectStep = { 0.0f, 0.0f };
  // A paused, first or invalid frame (dt <= 0) leaves the field where it is.
  if (!(dt > 0.0f)) {
    return;
  }

  const float windDirection = m_settings.cloudWindDirection;
  const float windAngle = windDirection * kDegreesToRadians;
  const float windSpeed = m_settings.cloudWindSpeed;  // km/s
  m_cloudAdvectStep = { std::cos(windAngle) * windSpeed * dt, std::sin(windAngle) * windSpeed * dt };
  m_cloudAdvectOffset.x += m_cloudAdvectStep.x;
  m_cloudAdvectOffset.y += m_cloudAdvectStep.y;

  // Detail-field drift: .y is the convective rise (straight up at every height), .x the downwind
  // shear, which the shader scales by height fraction so tops stream downwind while bases stay put.
  // The shader takes the direction from cloudWindDirUnitX/Z; only the magnitudes integrate here.
  const float evoSpeed = m_settings.cloudEvolutionSpeed;   // km/s, convective rise
  const float shearSpeed = m_settings.cloudBoilSpeed;      // km/s, downwind top shear
  m_cloudEvolutionOffset.y += evoSpeed * dt;
  m_cloudEvolutionOffset.x += shearSpeed * dt;
  m_cloudEvolutionOffset.z = 0.0f;  // unused

  m_cloudBoilPhase = 0.0f;  // unused
}

// Lightning strike scheduler (advanceLightning), run once per frame from updateFrame before the
// pre-exposure, which follows the flash. placeLightning positions a new strike later in the frame,
// once the camera anchor is known. It owns the flicker envelope and strike position that
// getAtmosphereArgs publishes.
//
// Strikes arrive with exponential inter-arrival times at the lightningStrikesPerMinute mean rate
// (a Poisson process: irregular gaps, the occasional quick double). Each strike sets the envelope to
// a random peak and schedules 0-2 restrike pulses 40-150 ms apart; between pulses the envelope
// decays with a ~70 ms time constant. Real flashes flicker for 100-300 ms, and single-frame pops
// smear badly under the host's temporal accumulation.
std::atomic<bool> Renderer::Impl::s_lightningStrikeRequested { false };

// The flash's peak luminous intensity (rtx.atmosphere.lightningLuminousIntensity, candela) in NUMOS
// units: the sun's illuminance units (sunIlluminance x sunIntensity is the real sun's 1.27e5 lux,
// the calibration the night sky is built on) times km^2, so intensity / d_km^2 is the illuminance at
// d km. Physical, not pre-exposed.
float Renderer::Impl::lightningIntensityKm2() const {
  constexpr float kSunLux = 1.27e5f;
  const Vector3 sun = m_settings.sunIlluminance * m_settings.sunIntensity;
  const float sunLuminance = 0.2126f * sun.x + 0.7152f * sun.y + 0.0722f * sun.z;
  return std::max(m_settings.lightningLuminousIntensity, 0.0f) * (sunLuminance / kSunLux) * 1e-6f;
}

// lightningColor at unit luminance: the hue only, so the intensity stays the one setting.
Vector3 Renderer::Impl::lightningHue() const {
  const Vector3 c = m_settings.lightningColor;
  const float lum = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
  return lum > 1e-6f ? c * (1.0f / lum) : Vector3(1.0f, 1.0f, 1.0f);
}

// The CPU twins of numosWorldToCloudKm (lib/atmosphere_common.slangh): a render-space position into
// the cloud frame and back, measured from the rendered camera, whose cloud-frame position is
// cloudFrameCameraKm(). The host renders around its own camera position; NUMOS stores what it places
// world-anchored, in the frame the cloud march samples.
Vector3 Renderer::Impl::worldToCloudKm(const Vector3& renderPosition) const {
  const Vector3 rel = renderPosition - m_inputs.cameraPosition;
  Vector3 relYUp = m_inputs.zUp ? Vector3(rel.x, rel.z, rel.y) : rel;
  if (m_settings.flipUpAxis) {
    relYUp.y = -relYUp.y;
  }
  return cloudFrameCameraKm() + relYUp * (1.0f / worldUnitsPerKm());
}

Vector3 Renderer::Impl::cloudKmToWorld(const Vector3& cloudKm) const {
  Vector3 rel = cloudKm - cloudFrameCameraKm();
  if (m_settings.flipUpAxis) {
    rel.y = -rel.y;
  }
  const Vector3 relWorld = m_inputs.zUp ? Vector3(rel.x, rel.z, rel.y) : rel;
  return m_inputs.cameraPosition + relWorld * worldUnitsPerKm();
}

// The strike in render space.
Vector3 Renderer::Impl::lightningRenderPosition() const {
  return cloudKmToWorld(m_lightningStrikePosKm);
}

// ---- Ground level (docs/DESIGN.md "Ground level") ----
// Altitude zero for the sky, the height fog and the cloud field. Manual: groundLevelWorldUnits.
// Auto: the measured, world-anchored ground field (GroundField) evaluated under the resolved anchor,
// so the deck sits its configured height above the local ground wherever the player goes; jumping or
// climbing does not move it (the field is a function of horizontal position), and the field's
// smoothing (groundSmoothingMeters, NUMOS metres) keeps it to the land's large-scale shape.
namespace {
  constexpr float  kGroundProbeAboveKm = 4.0f;     // probe rays start this far above the camera (NUMOS km)
  constexpr float  kGroundProbeBelowKm = 4.0f;     // ... and reach this far below it
  constexpr float  kGroundRateKmPerSecond = 0.02f; // fastest a refined measurement moves the field
  constexpr double kGroundCutWindowSeconds = 2.0;  // after a camera cut, new measurements apply at once

  // An engine-space vector into the atmosphere's Y-up frame (unscaled), as getAtmosphereArgs does.
  Vector3 engineToYUp(const Vector3& v, bool zUp, bool flip) {
    Vector3 yUp = zUp ? Vector3(v.x, v.z, v.y) : v;
    if (flip) {
      yUp.y = -yUp.y;
    }
    return yUp;
  }
}

void Renderer::Impl::updateGroundLevel(PassRecorder& rec, const Vector3& renderToWorld) {
  const bool zUp = m_inputs.zUp;
  const bool flip = m_settings.flipUpAxis;
  const float unitsPerKm = worldUnitsPerKm();
  const float dt = std::max(m_inputs.deltaSeconds, 0.0f);
  m_groundClockSeconds += dt;
  const double now = m_groundClockSeconds;
  // Possibly a different world: the host says so, or the anchor jumped (a teleport, a load, entering
  // an interior or another worldspace). Places at the same coordinates can then be unrelated, so the
  // field (cells and session mean) is discarded and measured afresh, and probes still in flight from
  // the old world are dropped when they return. A camera cut alone (a view change: another camera, a
  // scope, a menu over the same world) keeps the field: the world under it is the same one, and a
  // reset would trade a good measurement for the configured level until fresh probes return.
  const bool worldChanged = m_inputs.worldChanged || m_cloudAnchorCutThisFrame;
  if (worldChanged) {
    m_groundField.reset();
    for (GroundProbeSlot& slot : m_groundProbeSlots) {
      slot.stale = slot.pending;
    }
  }
  if (m_inputs.cameraCut || worldChanged) {
    m_groundFastUntil = now + kGroundCutWindowSeconds;
  }
  const bool fast = now <= m_groundFastUntil;

  // The configured level, as a Y-up height in world units.
  const float configured = m_settings.groundLevelWorldUnits;
  float groundYUp = engineToYUp(zUp ? Vector3(0.0f, 0.0f, configured) : Vector3(0.0f, configured, 0.0f), zUp, flip).y;
  bool measured = false;

  if (m_settings.groundLevelAuto) {
    // Cells half the smoothing sigma: the kernel spans a dozen of them, and each holds enough probe
    // hits for its low quantile to see past the roofs.
    const float sigmaWorld = std::max(m_settings.groundSmoothingMeters, 100.0f) * 0.001f * unitsPerKm;
    m_groundField.configure(0.5f * sigmaWorld, sigmaWorld);
    auto addWorldPoint = [&](const Vector3& world) {
      const Vector3 p = engineToYUp(world, zUp, flip);
      m_groundField.addSample(p.x, p.z, p.y, now, fast);
    };
    // Probe hits that have come back, in the world frame of the frame that cast them.
    for (GroundProbeSlot& slot : m_groundProbeSlots) {
      if (!slot.pending) {
        continue;
      }
      float hits[GROUND_PROBE_COUNT * 4];
      if (!rec.gpu().readBuffer(slot.readback.handle, hits, sizeof(hits))) {
        continue;  // still in flight
      }
      slot.pending = false;
      if (slot.stale) {
        slot.stale = false;
        continue;  // the old world's
      }
      for (uint32_t i = 0; i < GROUND_PROBE_COUNT; ++i) {
        if (hits[i * 4 + 3] > 0.5f) {
          addWorldPoint(Vector3(hits[i * 4 + 0], hits[i * 4 + 1], hits[i * 4 + 2]) + slot.renderToWorld);
        }
      }
    }
    // Host ground points, this frame's.
    for (uint32_t i = 0; m_inputs.groundPoints != nullptr && i < m_inputs.groundPointCount; ++i) {
      addWorldPoint(m_inputs.groundPoints[i] + renderToWorld);
    }
    m_groundField.update(now, dt, kGroundRateKmPerSecond * unitsPerKm, fast);
    const Vector3 camera = engineToYUp(m_cloudAnchor.resolvedRawWorldUnits, zUp, flip);
    float height = 0.0f;
    if (m_groundField.evaluate(camera.x, camera.z, now, height)) {
      groundYUp = height;
      measured = true;
    }
    if (m_inputs.opaqueTlas != 0) {
      dispatchGroundProbes(rec, renderToWorld, sigmaWorld);
    }
  }

  m_groundLevelYUpKm = groundYUp / unitsPerKm;
  m_groundLevelMeasured = measured;
  m_groundLevelWorldUnits = flip ? -groundYUp : groundYUp;
}

// One batch of probes (lib/ground_probe.h) into a free readback slot; nothing when all are in flight.
void Renderer::Impl::dispatchGroundProbes(PassRecorder& rec, const Vector3& renderToWorld, float sigmaWorld) {
  GroundProbeSlot* slot = nullptr;
  for (GroundProbeSlot& s : m_groundProbeSlots) {
    if (!s.pending) {
      slot = &s;
      break;
    }
  }
  if (slot == nullptr) {
    return;
  }
  const size_t size = size_t(GROUND_PROBE_COUNT) * 4u * sizeof(float);
  if (!m_groundProbeHits.isValid()) {
    replaceBuffer(rec, m_groundProbeHits, createBuffer(rec, "Ground Probe Hits", BufferUsage::Structured, size));
  }
  if (!slot->readback.isValid()) {
    replaceBuffer(rec, slot->readback, createBuffer(rec, "Ground Probe Readback", BufferUsage::Readback, size));
  }
  const float unitsPerKm = worldUnitsPerKm();
  GroundProbeArgs a {};
  a.originWorld = m_inputs.cameraPosition;
  a.probeCount = GROUND_PROBE_COUNT;
  // As far as the field's kernel reaches around the camera.
  a.radiusWorld = GroundField::kKernelSigmas * sigmaWorld;
  a.startAboveWorld = kGroundProbeAboveKm * unitsPerKm;
  a.lengthWorld = (kGroundProbeAboveKm + kGroundProbeBelowKm) * unitsPerKm;
  a.sequenceBase = m_groundProbeSequence;
  a.isZUp = m_inputs.zUp ? 1u : 0u;
  a.flipUpAxis = m_settings.flipUpAxis ? 1u : 0u;
  m_groundProbeSequence += GROUND_PROBE_COUNT;

  // The previous batch's copy out of the hits buffer comes first (hosts that only honour barrier()).
  rec.barrier();
  rec.bindAccelerationStructure(GROUND_PROBE_BINDING_ACCELERATION_STRUCTURE, m_inputs.opaqueTlas);
  rec.bindBuffer(GROUND_PROBE_BINDING_HITS, m_groundProbeHits.handle, 0, size);
  rec.pushConstants(&a, sizeof(a));
  rec.setPass(PassId::ground_probe);
  rec.dispatch((GROUND_PROBE_COUNT + GROUND_PROBE_GROUP_SIZE - 1) / GROUND_PROBE_GROUP_SIZE, 1u, 1u);
  rec.barrier();
  rec.gpu().copyBuffer(slot->readback.handle, 0, m_groundProbeHits.handle, 0, size);
  slot->renderToWorld = renderToWorld;
  slot->pending = true;
}


void Renderer::Impl::advanceLightning(float dt) {
  m_lightningEnvelopePrevious = m_lightningEnvelope;  // what last frame rendered
  if (!m_settings.lightningEnable) {
    m_lightningEnvelope = 0.0f;
    m_lightningPulsesLeft = 0;
    s_lightningStrikeRequested.store(false);  // don't bank a Test Strike while disabled
    return;
  }
  if (!(dt > 0.0f)) {
    return;  // pause / first frame: hold the envelope, no decay jump on resume
  }

  // xorshift32: cheap and deterministic per session; no distribution quality is needed.
  auto rand01 = [this]() -> float {
    uint32_t x = m_lightningRngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    m_lightningRngState = x;
    return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
  };

  // Envelope decay (~70 ms time constant), cut to 0 (the glow, haze and scene light go inert) once
  // the flash's brightest glow, at the strike's core, is down to 1e-4 of the scene's own light: a
  // white cloud under the sky updateRadianceScale tracks (last frame's), sunLuminance x relative /
  // pi. That is invisible at any time of day. A fixed fraction of the peak would end a night flash
  // while it is still far brighter than everything else, a visible pop. A night flash fades for
  // ~1.4 s, a day one for ~0.3 s.
  constexpr float kDecayTau = 0.07f;
  constexpr float kSunLux = 1.27e5f;  // as lightningIntensityKm2: the sun's luminance cancels
  m_lightningEnvelope *= std::exp(-dt / kDecayTau);
  const float intensity = std::max(m_settings.lightningLuminousIntensity, 0.0f);
  const float endEnvelope = intensity > 0.0f
    ? 1e-4f * m_skyRelativeLuminance * (kSunLux * 1e6f / kPi) / (intensity * kLightningCoreGlowPerKm2)
    : 1.0f;
  if (m_lightningEnvelope < std::max(endEnvelope, 1e-12f)) {
    m_lightningEnvelope = 0.0f;
  }

  // Restrike pulses re-peak the envelope 0-2 times at random 40-150 ms gaps (multi-stroke flicker).
  if (m_lightningPulsesLeft > 0) {
    m_lightningTimeToPulse -= dt;
    if (m_lightningTimeToPulse <= 0.0f) {
      --m_lightningPulsesLeft;
      m_lightningEnvelope = std::max(m_lightningEnvelope, 0.45f + 0.55f * rand01());
      m_lightningTimeToPulse = 0.04f + 0.11f * rand01();
    }
  }

  // A per-frame Bernoulli draw at probability (rate / 60) * dt is a memoryless (Poisson) process:
  // exponential gaps with no countdown state. That matters because weather ramps the rate
  // continuously (clear 0, thunderstorm 12/min), and a countdown drawn at a low mid-blend rate
  // would sit on a minutes-long gap after the storm arrived. Rate 0 means manual strikes only.
  const float rate = std::max(m_settings.lightningStrikesPerMinute, 0.0f);
  bool fire = s_lightningStrikeRequested.exchange(false) || m_inputs.lightningStrike;
  if (rate > 0.0f && rand01() < (rate / 60.0f) * dt) {
    fire = true;
  }

  if (fire) {
    m_lightningEnvelope = 0.7f + 0.3f * rand01();
    m_lightningPulsesLeft = static_cast<int>(rand01() * 3.0f);  // 0-2 restrikes
    m_lightningTimeToPulse = 0.04f + 0.11f * rand01();
    m_lightningPlacePending = true;
    m_lightningPlaceAtInput = m_inputs.lightningStrike && m_inputs.lightningStrikeAtPosition;
  }
}

// Places a strike advanceLightning fired this frame. Separate because advanceLightning runs before
// the pre-exposure (which must see the flash) and the placement needs this frame's camera anchor.
void Renderer::Impl::placeLightning() {
  if (!m_lightningPlacePending) {
    return;
  }
  m_lightningPlacePending = false;
  if (m_lightningPlaceAtInput) {
    // A host-placed strike: render space into the cloud frame (the inverse of lightningRenderPosition).
    m_lightningStrikePosKm = worldToCloudKm(m_inputs.lightningStrikePosition);
    return;
  }
  // Uniform in area over an annulus around the camera in the cloud frame, low in the slab: bolts
  // glow brightest near the base, and a base-height flash lights the underside of the deck. A strike
  // where there is no cloud lights nothing, since the march term scales by local density, so no CPU
  // cloud query is needed.
  auto rand01 = [this]() -> float {
    uint32_t x = m_lightningRngState;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    m_lightningRngState = x;
    return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
  };
  constexpr float kMinStrikeKm = 1.0f;
  const float maxR = std::max(m_settings.lightningRangeKm, kMinStrikeKm + 0.1f);
  const float r = std::sqrt(kMinStrikeKm * kMinStrikeKm
                            + (maxR * maxR - kMinStrikeKm * kMinStrikeKm) * rand01());
  const float ang = rand01() * 2.0f * 3.14159265358979323846f;
  const float cloudThicknessKm = m_settings.cloudDepthMeters * 0.001f;
  const float strikeY = m_settings.cloudBaseHeightMeters * 0.001f + 0.15f * cloudThicknessKm;
  m_lightningStrikePosKm = Vector3(m_cameraWorldPosYUpKm.x + std::cos(ang) * r,
                                   strikeY,
                                   m_cameraWorldPosYUpKm.z + std::sin(ang) * r);
}

// Cloud screen pass entry point. The host calls it after its primary rays have written the linear
// view depth the march clamps against. Every bake the march reads (placement map, NVDF chain, shadow
// maps, dome LUT) ran earlier in the frame from computeLuts.
void Renderer::Impl::dispatchPostDepth(IGpu& gpu, GpuHandle linearViewZ, float missLinearViewZ) {
  PassRecorder recorder(gpu);
  PassRecorder& rec = recorder;
  m_missLinearViewZ = missLinearViewZ;
  // The aerial perspective's froxel lighting and integration keep their samples in front of this
  // frame's visible surfaces, so they run here, whatever the cloud pass does below.
  dispatchAerialPerspectiveNear(rec, linearViewZ, missLinearViewZ);
  // debugDispatchCloudRender skips the pass for an A/B frame-time read. The pass is also skipped
  // when nothing reads it: the composite ignores the RT unless m_cloudCompositeActive (with clouds
  // off the pass would only write "no cloud", an exact identity). The debug views read the RT
  // directly, so an active debug view keeps the pass running.
  const bool debugViewActive = m_inputs.debugViewActive;
  if (!m_initialized || !m_settings.debugDispatchCloudRender || !m_cloudRenderRT.isValid()
      || (!m_cloudCompositeActive && !debugViewActive)) {
      m_cloudTemporalHistoryValid = false;
    m_cloudDenoisedThisFrame = false;
    if (m_initialized) {
      dispatchCloudSampleStatistics(rec);
    }
    publishOutputs();
    return;
  }

  // Orders the LUT, SDF and shadow map writes from computeLuts, and the host's linearViewZ write,
  // before this pass reads them.
  rec.barrier();

  dispatchCloudRender(rec, linearViewZ);
  dispatchCloudDenoise(rec);
  dispatchCloudSampleStatistics(rec);
  publishOutputs();
}

// Edge-aware spatial filter after the temporal one (cloud_denoise.comp.slang), into its own texture:
// the host composites that, while m_cloudRenderRT stays next frame's unfiltered history.
void Renderer::Impl::dispatchCloudDenoise(PassRecorder& rec) {
  m_cloudDenoisedThisFrame = false;
  if (m_settings.cloudSpatialDenoise <= 0.0f || !m_cloudDenoisedRT.isValid() || m_settings.cloudProfilingMode != 0) {
    return;
  }
  GpuZone zone(rec.gpu(), "Atmosphere Cloud Denoise");
  struct CloudDenoiseArgs {
    float params[4];
  };
  const CloudDenoiseArgs denoiseArgs { { std::min(m_settings.cloudSpatialDenoise, 1.0f), 0.05f,
    std::max(m_settings.cloudHistoryDepthTolerance, 0.01f), 0.0f } };
  rec.barrier();
  rec.setPass(PassId::cloud_denoise);
  rec.bindConstants(0, &denoiseArgs, sizeof(denoiseArgs));
  rec.bindTexture(1, m_cloudRenderRT);
  rec.bindTexture(2, m_cloudDepthRT);
  rec.bindTexture(3, m_cloudDenoisedRT);
  rec.dispatch((m_cloudRenderExtentWidth + 7u) / 8u, (m_cloudRenderExtentHeight + 7u) / 8u, 1u);
  m_cloudDenoisedThisFrame = true;
}

bool Renderer::Impl::getCloudOffsetAtPlayer(float& offsetWorldUnits) const {
  const AtmosphereArgs args = getAtmosphereArgs();
  // Callers read the last cloud snapshot; the live camera may already belong to another frame.
  if (!m_cloudAnchorHasFirstSample || args.cloudAltitude < 0.0f || args.cloudThickness <= 0.0f) {
    return false;
  }

  const Vector3 freecamOffset = m_cloudAnchor.rawWorldUnitsFreecam - m_cloudAnchor.rawWorldUnits;
  const Vector3 player = m_cloudAnchor.source == CloudAnchor::Source::CameraWorldOverride
    ? m_cloudAnchor.resolvedRawWorldUnits - freecamOffset
    : m_cloudAnchor.rawWorldUnits + m_cloudHostWorldOrigin;  // world position (host world origin)
  const float playerHeight = m_inputs.zUp ? player.z : player.y;
  const float upSign = m_settings.flipUpAxis ? -1.0f : 1.0f;
  const float layerMidpointKm = args.cloudAltitude + 0.5f * args.cloudThickness;
  // Move the datum instead of pushing the cloud shell below the planet surface.
  offsetWorldUnits = upSign * (playerHeight - m_groundLevelWorldUnits)
    - layerMidpointKm * args.worldUnitsPerKm;
  return std::isfinite(offsetWorldUnits);
}

void Renderer::Impl::traceCloudPlacement(const AtmosphereArgs& args) {
  if (!m_traceCloudPlacement) {
    m_cloudPlacementTraceFrame = 0;
    return;
  }
  if (m_cloudPlacementTraceFrame++ % 120 != 0) {
    return;
  }

  const Vector3 activePosition = m_inputs.cameraPosition;
  const auto& anchor = m_cloudAnchor;
  logInfo(format(
    "[Numos placement] frame=", m_cloudRenderFrameIdx,
    " freecam=", (m_inputs.cameraPosition != m_inputs.cameraPositionGame),
    " source=", static_cast<uint32_t>(anchor.source),
    " raw=(", anchor.rawWorldUnits.x, ",", anchor.rawWorldUnits.y, ",", anchor.rawWorldUnits.z, ")",
    " active=(", activePosition.x, ",", activePosition.y, ",", activePosition.z, ")",
    " resolved=(", anchor.resolvedRawWorldUnits.x, ",", anchor.resolvedRawWorldUnits.y, ",", anchor.resolvedRawWorldUnits.z, ")",
    " shaderKm=(", args.cameraWorldPosYUpKm.x, ",", args.cameraWorldPosYUpKm.y, ",", args.cameraWorldPosYUpKm.z, ")",
    " groundUnits=", m_groundLevelWorldUnits, (m_groundLevelMeasured ? " (measured)" : " (configured)"),
    " groundKm=", m_groundLevelYUpKm,
    " cameraAltitudeM=", args.cameraAltitudeKm * 1000.0f,
    " cloudCameraAltitudeM=", args.cameraWorldPosYUpKm.y * 1000.0f,
    " cloudOffsetUnits=", m_settings.cloudVerticalOffsetWorldUnits,
    " cloudBaseM=", args.cloudAltitude * 1000.0f,
    " cloudDepthM=", args.cloudThickness * 1000.0f,
    " baseFromCameraM=", (args.cloudAltitude - args.cameraWorldPosYUpKm.y) * 1000.0f,
    " topFromCameraM=", (args.cloudAltitude + args.cloudThickness - args.cameraWorldPosYUpKm.y) * 1000.0f,
    " unitsPerKm=", args.worldUnitsPerKm,
    " planetRadiusKm=", args.planetRadius,
    " zUp=", args.isZUp, " flipUp=", args.flipUpAxis,
    " cloudEnabled=", args.cloudEnabled, " coverage=", args.cloudCoverageMean));
}

// The quality level scales the settings the host chose rather than replacing them, so a game's own
// froxel size or sample cap keeps its relative weight at every level. The quarter march of Medium and
// Low is resolved in cloudMarchStride.
void Renderer::Impl::applyQualityLevel() {
  const int32_t level = std::clamp(m_settings.qualityLevel, 0, 2);
  if (level == 0) {
    return;
  }
  Settings& s = m_settings;
  const float froxelSizeScale = level == 1 ? 1.5f : 2.0f;
  const float froxelSliceScale = level == 1 ? 2.0f / 3.0f : 0.5f;
  s.aerialPerspectiveFroxelSizePixels = std::clamp(
    static_cast<int32_t>(std::lround(float(s.aerialPerspectiveFroxelSizePixels) * froxelSizeScale)), 2, 64);
  s.aerialPerspectiveFroxelDepthSlices = std::clamp(
    static_cast<int32_t>(std::lround(float(s.aerialPerspectiveFroxelDepthSlices) * froxelSliceScale)), 16, 256);
  if (level >= 2) {
    s.cloudPerformancePreset = 1;
    s.cloudViewSamplesMax = std::max(s.cloudViewSamplesMax / 2u, 2u);
  }
}

// The screen march rate (docs/cloud-temporal-filter.md, "Choosing the march rate"). The sparse rates run in
// the screen pass's temporal-filter path: the filter on, the normal profiling mode.
//
// Auto takes the sparsest stride k (1, 2, 4) that passes both tests:
//  * Motion: while the history is resampled it keeps about F = 1 / (1 - cloudTemporalHistoryWeight) frames,
//    and a pixel gets its own march once every k^2 frames. The change test compares the history with a fast
//    history of kFastSamples (4) samples, so the window must hold at least that many of the pixel's own
//    marches: F / k^2 >= 4. At the default 0.95 (F = 20): k <= 2. The sixteenth march (16 > 5) never
//    qualifies; it stays a manual choice.
//  * Magnification: the layer reaches the screen at the upscaler's ratio s = output / render (per axis), and
//    the upscaler is told to trust it as is, so its noise and its reconstruction show at s output pixels per
//    render pixel. A stride-k march costs what a full-rate march at 1/k of the resolution costs, and in motion
//    it is about as noisy (k^2 fewer own samples at the same pixel area, against k^2 larger pixels with the
//    full count); in a still view it converges to the full resolution. So it is acceptable where that
//    equal-cost full-rate march would be: no coarser than the coarsest upscale the full-rate layer holds up
//    at, the host upscaler's own limit kMaxUpscale = 3 (DLSS Ultra Performance; in game the full-rate layer
//    scaled cleanly there): k * s <= 3; and no fewer lines than that ratio leaves at the lowest output the
//    renderer targets, 1080 / 3 = 360: renderHeight / k >= 360.
//  At the defaults: native and DLSS Quality (s 1.5) with a render height of at least 720 take the quarter
//  march; Balanced (1.72), Performance (2) and Ultra Performance (3) march at full rate.
uint32_t Renderer::Impl::cloudMarchStride() const {
  if (m_settings.cloudTemporalHistoryWeight <= 0.0f || m_settings.cloudProfilingMode != 0) {
    return 1u;
  }
  switch (m_settings.cloudMarchRate) {
  case 1: return 1u;
  case 2: return 2u;
  case 3: return 4u;
  default: break;
  }
  constexpr float kFastSamples = 4.0f;          // the change test's fast history (cloud_render.comp.slang)
  constexpr float kMaxUpscale = 3.0f;           // the coarsest host upscale the full-rate layer holds up at
  constexpr float kMinEquivalentLines = 360.0f; // that upscale at the lowest targeted output (1080 / 3)
  const float renderHeight = float(m_inputs.renderHeight);
  if (!(renderHeight > 0.0f)) {
    return 1u;
  }
  const float outputHeight = m_inputs.outputHeight > 0u ? float(m_inputs.outputHeight) : renderHeight;
  const float upscale = std::max(outputHeight / renderHeight, 1.0f);
  const float movingFrames = 1.0f / std::max(1.0f - std::min(m_settings.cloudTemporalHistoryWeight, 0.98f), 1e-3f);
  uint32_t stride = 1u;
  for (uint32_t k : { 2u, 4u }) {
    const float kf = float(k);
    if (movingFrames / (kf * kf) >= kFastSamples && kf * upscale <= kMaxUpscale
        && renderHeight / kf >= kMinEquivalentLines) {
      stride = k;
    }
  }
  // Medium and Low drop the magnification tests: the quarter march then runs at any resolution, as
  // long as the history holds enough of a pixel's own marches.
  if (m_settings.qualityLevel >= 1 && movingFrames / 4.0f >= kFastSamples) {
    stride = std::max(stride, 2u);
  }
  return stride;
}

void Renderer::Impl::dispatchCloudRender(PassRecorder& rec, GpuHandle linearViewZ) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud Render (Nubis Cubed)");

  if (!m_cloudRenderRT.isValid()) {
    return;  // ensureCloudRenderRT hasn't allocated yet (first frame with zero extent)
  }

  if (m_cloudLastRenderFrame + 1u != m_cloudRenderFrameIdx) {
    m_cloudTemporalHistoryValid = false;
  }
  std::swap(m_cloudRenderRT, m_cloudRenderPrevious);
  std::swap(m_cloudDepthRT, m_cloudDepthPrevious);
  std::swap(m_cloudStateRT, m_cloudStatePrevious);
  AtmosphereArgs args = getCloudArgs();
  traceCloudPlacement(args);
  m_cloudProfileState.samples = args.cloudViewSamples;
  m_cloudProfileState.maxSamples = static_cast<uint32_t>(args.cloudViewSamplesMax);
  m_cloudProfileState.sampleSpacingKm = args.cloudViewStepKm;
  m_cloudProfileState.domePeriod    = m_cloudDomePeriodThisFrame;
  m_cloudProfileState.renderWidth   = m_cloudRenderExtentWidth;
  m_cloudProfileState.renderHeight  = m_cloudRenderExtentHeight;
  m_cloudProfileState.detailLod     = args.cloudDetailLodEnable;
  m_cloudProfileState.detailLodBias = args.cloudDetailLodBias;

  // Linear/repeat for the cloud volumes, matching the frac() tile wrap of the cloud math. It is
  // mip-capable so the detail-LOD taps can ask for explicit levels of the detail volume.
  const GpuHandle cloudSampler = sampler(rec.gpu(), Filter::Linear, Filter::Linear, AddressMode::Repeat, SamplerDesc::kLodClampNone);

  // Linear/clamp for the sky LUTs: the sky-view LUT is keyed by (azimuth, elevation), and repeat
  // would alias the bottom row onto the top.
  const GpuHandle skyViewSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge, 0.0f);

  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindSampler(2, cloudSampler);
  rec.bindTexture(5, blueNoise());
  rec.bindTexture(6, m_cloudRenderRT);
  rec.bindTexture(7, m_skyViewLut);
  rec.bindSampler(9, skyViewSampler);
  rec.bindTexture(10, m_transmittanceLut);
  // Nubis3 model inputs: front SDF and detail volume.
  rec.bindTexture(13, m_cloudNvdfSdf[m_cloudNvdfSdfFront]);
  rec.bindTexture(14, cloudDensityNoise3D());
  rec.bindTexture(20, m_cloudShadowMap);
  rec.bindTexture(21, m_cloudHorizonMap);
  rec.bindTexture(22, m_cloudVerticalOd);
  rec.bindTexture(23, m_cloudLayer2Map);
  rec.bindTexture(24, m_cloudBeerMap);

  // Depth-aware march: slot 15 is this frame's primary-ray linear view depth from the host, slot 16
  // the depth companion RT this pass writes (format in ensureCloudRenderRT). These slots are local
  // to cloud_render.comp.slang, not the shared binding indices.
  rec.bindTexture(15, linearViewZ);
  rec.bindTexture(16, m_cloudDepthRT);
  rec.bindConstants(17, &m_inputs.camera, sizeof(Camera));
  rec.bindTexture(18, m_cloudRenderPrevious);
  rec.bindTexture(19, m_cloudDepthPrevious);
  rec.bindTexture(25, m_cloudFreshColor);
  rec.bindTexture(26, m_cloudFreshDepth);
  rec.bindTexture(27, m_cloudFreshAux);
  rec.bindTexture(28, m_cloudStateRT);
  rec.bindTexture(29, m_cloudStatePrevious);

  if (m_settings.cloudProfilingMode == 9) {
    // Reference path tracer (cloud_reference.slangh): ground truth for tuning the march.
    rec.setPass(PassId::cloud_render_reference);
  } else {
    rec.setPass(m_settings.cloudPerformancePreset == 1 ? PassId::cloud_render_fast : PassId::cloud_render);
  }

  const uint32_t groupWidth = 8u;
  const uint32_t groupHeight = 8u;
  const uint32_t width = m_cloudRenderExtentWidth;
  const uint32_t height = m_cloudRenderExtentHeight;
  if (cloudSixteenthMarchActive()) {
    // Sixteenth march, two dispatches: the march pass (one lane per 4x4 cell) into the fresh
    // buffers, then the rebuild pass (one lane per pixel, screen pass bit 20) reads them. The
    // quarter march keeps one pass with the cells shared in groupshared memory: reading them back
    // from memory costs 0.2 ms more, while at 4x4 a group's own cells are too few (its tiles show
    // as 32 px blocks).
    const uint32_t cell = 4u;
    const uint32_t cellsW = (width + cell - 1u) / cell;
    const uint32_t cellsH = (height + cell - 1u) / cell;
    rec.dispatch((cellsW + groupWidth - 1u) / groupWidth, (cellsH + groupHeight - 1u) / groupHeight, 1);
    rec.barrier();
    AtmosphereArgs rebuildArgs = args;
    rebuildArgs.cloudScreenInterleave |= 1u << 20u;
    rec.bindConstants(0, &rebuildArgs, sizeof(AtmosphereArgs));
    rec.dispatch((width + groupWidth - 1u) / groupWidth, (height + groupHeight - 1u) / groupHeight, 1);
  } else {
    // The quarter march has one lane per 2x2 cell. The pass shifts its group grid each frame for
    // the temporal filter (cloud_render's tileShift), so it needs one more group per axis to keep
    // covering the edge pixels.
    const uint32_t cell = cloudQuarterMarchActive() ? 2u : 1u;
    const uint32_t shiftPad = 1u;
    rec.dispatch((width + cell * groupWidth - 1u) / (cell * groupWidth) + shiftPad,
                 (height + cell * groupHeight - 1u) / (cell * groupHeight) + shiftPad, 1);
  }
  m_cloudTemporalHistoryValid = true;
  m_cloudLastRenderFrame = m_cloudRenderFrameIdx;
}

void Renderer::Impl::dispatchCloudSampleStatistics(PassRecorder& rec) {
  if (m_cloudStatisticsPending) {
    std::vector<uint32_t> rows(size_t(m_cloudStatisticsGroups) * 4u);
    if (!rec.gpu().readBuffer(m_cloudStatisticsReadback.handle, rows.data(), rows.size() * sizeof(uint32_t))) {
      return;  // still in flight
    }
    const uint32_t* pRows = rows.data();
    uint64_t sum = 0u, active = 0u, pixels = 0u;
    m_cloudSamplesMaximum = 0u;
    for (uint32_t i = 0u; i < m_cloudStatisticsGroups; ++i) {
      sum += pRows[i * 4u];
      m_cloudSamplesMaximum = std::max(m_cloudSamplesMaximum, pRows[i * 4u + 1u]);
      active += pRows[i * 4u + 2u];
      pixels += pRows[i * 4u + 3u];
    }
    m_cloudSamplesMean = pixels ? double(sum) / double(pixels) : 0.0;
    m_cloudSamplesActiveMean = active ? double(sum) / double(active) : 0.0;
    m_cloudSamplesActivePercent = pixels ? 100.0 * double(active) / double(pixels) : 0.0;
    logInfo(format("[Cloud samples GPU] frame=", m_cloudStatisticsFrame,
      " base=", m_cloudStatisticsConfig.samples, " cap=", m_cloudStatisticsConfig.maxSamples,
      " spacingKm=", m_cloudStatisticsConfig.sampleSpacingKm,
      " meanAll=", m_cloudSamplesMean, " meanEvaluated=", m_cloudSamplesActiveMean,
      " max=", m_cloudSamplesMaximum, " evaluatedPercent=", m_cloudSamplesActivePercent));
    m_cloudStatisticsPending = false;
  }
  if (!m_settings.cloudProfilingLog || !m_settings.debugDispatchCloudRender
      || !m_cloudDepthRT.isValid() || m_cloudLastRenderFrame != m_cloudRenderFrameIdx) {
    return;
  }
  if (m_cloudStatisticsCounter++ % 120u != 0u) {
    return;
  }
  const uint32_t groupsX = (m_cloudRenderExtentWidth + 7u) / 8u;
  const uint32_t groupsY = (m_cloudRenderExtentHeight + 7u) / 8u;
  const uint32_t groups = groupsX * groupsY;
  const size_t size = size_t(groups) * 4u * sizeof(uint32_t);
  if (!m_cloudStatisticsGpu.isValid() || m_cloudStatisticsGpu.desc.size != size) {
    replaceBuffer(rec, m_cloudStatisticsGpu, createBuffer(rec, "Cloud Sample Statistics GPU", BufferUsage::Structured, size));
    replaceBuffer(rec, m_cloudStatisticsReadback, createBuffer(rec, "Cloud Sample Statistics Readback", BufferUsage::Readback, size));
  }
  // The screen pass wrote the depth RT just before this, and the copy reads what this dispatch
  // writes: hosts that only honour barrier() (gpu.h) need both.
  rec.barrier();
  rec.bindTexture(0, m_cloudDepthRT);
  rec.bindBuffer(1, m_cloudStatisticsGpu.handle, 0, size);
  rec.setPass(PassId::cloud_sample_statistics);
  rec.dispatch(groupsX, groupsY, 1u);
  rec.barrier();
  rec.gpu().copyBuffer(m_cloudStatisticsReadback.handle, 0, m_cloudStatisticsGpu.handle, 0, size);
  m_cloudStatisticsGroups = groups;
  m_cloudStatisticsFrame = m_cloudRenderFrameIdx;
  m_cloudStatisticsConfig = m_cloudProfileState;
  m_cloudStatisticsPending = true;
}

void Renderer::Impl::dispatchCloudSecondaryLut(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud Secondary LUT");

  if (!m_cloudSecondaryLut.isValid()) {
    return;
  }

  AtmosphereArgs args = getCloudArgs();

  // Samplers as in dispatchCloudRender.
  const GpuHandle cloudSampler = sampler(rec.gpu(), Filter::Linear, Filter::Linear, AddressMode::Repeat, SamplerDesc::kLodClampNone);

  const GpuHandle skyViewSampler = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge, 0.0f);

  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindSampler(2, cloudSampler);
  rec.bindTexture(5, blueNoise());
  rec.bindTexture(6, m_cloudSecondaryLut, 0);  // mip 0 storage write
  rec.bindTexture(7, m_skyViewLut);
  rec.bindSampler(9, skyViewSampler);
  rec.bindTexture(10, m_transmittanceLut);
  // Nubis3 model inputs: front SDF and detail volume.
  rec.bindTexture(13, m_cloudNvdfSdf[m_cloudNvdfSdfFront]);
  rec.bindTexture(14, cloudDensityNoise3D());
  rec.bindTexture(20, m_cloudShadowMap);
  rec.bindTexture(21, m_cloudHorizonMap);
  rec.bindTexture(22, m_cloudVerticalOd);
  rec.bindTexture(23, m_cloudLayer2Map);
  rec.bindTexture(24, m_cloudBeerMap);


  rec.setPass(PassId::cloud_secondary_lut);

  // The shader declares [numthreads(4, 4, 1)]: the march is latency-bound (its time follows the
  // longest rays), and small groups schedule those best (8x8 measured 0.29 ms, 4x4 0.25 ms).
  // With an interleave period P the dispatch covers 1/P of the rows and the shader maps each
  // thread onto row P * y + phase; the mip chain below still rebuilds from all of mip 0.
  const uint32_t period = std::max(args.cloudDomeInterleave & 0xFFu, 1u);
  const uint32_t groupsX = (kCloudSecondaryLutWidth  + 3u) / 4u;
  const uint32_t groupsY = (kCloudSecondaryLutHeight / period + 3u) / 4u;
  rec.dispatch(groupsX, groupsY, 1);

  // Blur mip 0 down the chain so the sky-from-clouds bleed can sample a coarse, wide-blurred level.
  rec.barrier();
  {
    GpuZone innerZone(rec.gpu(), "Atmosphere Cloud Secondary LUT Mipmap");
    generateMipmapGaussian(rec, m_cloudSecondaryLut);
  }
}

// Gaussian mip chain: each level from the previous one with the generate_mipmap pass (3x3
// Gaussian), a linear/clamp sampler and 16x16 groups, with a barrier between levels.
void Renderer::Impl::generateMipmapGaussian(PassRecorder& rec, const Texture& texture) {
  const GpuHandle linearClamp = sampler(rec.gpu(), Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge);
  for (uint32_t i = 1; i < texture.desc.mipLevels; i++) {
    const uint32_t width = std::max(texture.desc.width >> i, 1u);
    const uint32_t height = std::max(texture.desc.height >> i, 1u);
    GenerateMipmapArgs pushArgs = {};
    pushArgs.resolution = { width, height };
    pushArgs.method = MipmapMethod::Gaussian;
    rec.pushConstants(&pushArgs, sizeof(pushArgs));
    rec.bindTexture(GENERATE_MIPMAP_INPUT, texture, i - 1);
    rec.bindSampler(GENERATE_MIPMAP_INPUT, linearClamp);
    rec.bindTexture(GENERATE_MIPMAP_OUTPUT, texture, i);
    rec.setPass(PassId::generate_mipmap);
    rec.dispatch((width + 15u) / 16u, (height + 15u) / 16u, 1u);
    rec.barrier();
  }
}

void Renderer::Impl::dispatchCloudPlacementMapBake(PassRecorder& rec) {
  GpuZone zone(rec.gpu(), "Atmosphere Cloud Placement Map Bake");

  // Baked at init and when needsCloudPlacementRebake() says an input changed. Fills the 512x512
  // RGBA8 map with the cluster, top-jitter and base-lift fields of
  // cloud_placement_map_baker.comp.slang.
  AtmosphereArgs args = getAtmosphereArgs();

  rec.bindConstants(0, &args, sizeof(AtmosphereArgs));
  rec.bindTexture(1, m_cloudPlacementMap);

  rec.setPass(PassId::cloud_placement_map_baker);

  // Shader declares [numthreads(8, 8, 1)].
  const uint32_t groupCount = (kCloudPlacementMapSize + 7u) / 8u;
  rec.dispatch(groupCount, groupCount, 1);
}

void Renderer::Impl::updateFrame(PassRecorder& rec, float deltaTimeSeconds) {
  m_animationSeconds += std::max(double(deltaTimeSeconds), 0.0);
  // How long the cloud coverage has held still (nvdfCoverageStale).
  m_coverageSteadyFrames = m_settings.cloudCoverageMean == m_lastCoverageMean ? m_coverageSteadyFrames + 1u : 0u;
  m_lastCoverageMean = m_settings.cloudCoverageMean;
  initialize(rec);
  uploadNightSky(rec);

  // Latched before any getAtmosphereArgs read this frame, so the composite's flag and the screen
  // pass skip cannot disagree if a setting flips mid-frame.
  m_cloudCompositeActive = m_settings.cloudRenderRTEnable && m_settings.cloudEnabled;

  // The integrators advance exactly once per frame, before any LUT bake or AtmosphereArgs read.
  advanceCloudMotion(deltaTimeSeconds);
  advanceTimeCycle(deltaTimeSeconds);
  updateCelestial();
  // Before the pre-exposure, which follows the flash; placeLightning below positions a new strike.
  advanceLightning(deltaTimeSeconds);
  updateRadianceScale();

  const Vector3 forward = m_inputs.cameraForward;

  // Positions and shader ray directions must use the same world-to-atmosphere conversion.
  const bool isZUp = m_inputs.zUp;
  const bool flipUpAxisForYUp = m_settings.flipUpAxis;
  auto toYUp = [isZUp, flipUpAxisForYUp](const Vector3& v) -> Vector3 {
    const Vector3 yUp = isZUp ? Vector3(v.x, v.z, v.y) : v;
    return flipUpAxisForYUp ? Vector3(yUp.x, -yUp.y, yUp.z) : yUp;
  };

  // ---- Cloud anchor ----
  // Resolve where the clouds are anchored this frame before anything downstream runs. Some engines
  // keep the camera translation out of the view matrix (the camera position then reads (0,0,0)), so
  // the anchor can also come from an explicit world position the host pushes.
  {
    const Vector3 rawWorldUnits        = m_inputs.cameraPositionGame;
    const Vector3 rawWorldUnitsFreecam = m_inputs.cameraPosition;
    // Both readings are kept for the debug view. They differ only while a free camera is flying,
    // and the anchor follows the free camera so clouds are positioned and oriented consistently.

    // useCameraWorldOverride + cameraWorldOverride let the game integration push the real camera
    // world position, in the same raw game units the camera position would have, converted with the
    // same toYUp and worldUnitsPerKm() as the view-matrix path, so the two are interchangeable.
    const bool useOverride = m_settings.useCameraWorldOverride;
    // Free-camera displacement in raw game units, zero unless a free camera is active. The host
    // builds the free camera's transform itself, so this is a real position change even on an
    // engine whose own camera translation never reaches the view matrix.
    const Vector3 freecamOffsetWorldUnits = rawWorldUnitsFreecam - rawWorldUnits;
    // Adding it to the override lets the free camera fly into the deck: the override says where the
    // player is, the offset where the viewer has flown relative to them. It is zero when the free
    // camera is off, so a per-frame override is unaffected.
    // A host that submits the scene around a moving render origin publishes that origin (the idea
    // comes from BetaRT, sambow23/dxvk-remix-gmod), and the camera's world position is render
    // position + origin. Without it the anchor would jump with each origin move and the clouds would
    // snap. The override is already a world position, so it takes no origin.
    m_cloudHostWorldOrigin = m_inputs.hasWorldOrigin ? m_inputs.worldOrigin : Vector3(0.0f);
    const Vector3 resolvedRawWorldUnits = useOverride
      ? m_settings.cameraWorldOverride + freecamOffsetWorldUnits
      : rawWorldUnitsFreecam + m_cloudHostWorldOrigin;
    const CloudAnchor::Source resolvedSource = useOverride
      ? CloudAnchor::Source::CameraWorldOverride
      : CloudAnchor::Source::CameraViewMatrix;

    const Vector3 posYUpKm = toYUp(resolvedRawWorldUnits) * (1.0f / worldUnitsPerKm());

    const Vector3 prevPosYUpKm = m_cloudAnchor.posYUpKm;
    const Vector3 deltaKm = posYUpKm - prevPosYUpKm;
    const Vector3 forwardYUpForAnchor = toYUp(forward);

    // Per-frame streak for the debug readout only, not the warning gate below. deltaKm == 0 while the
    // view rotates is also what a healthy engine reports while the player stands still looking
    // around; everMoved and cumulativeRotationRadians are what tell the two apart.
    const bool isStaticThisFrame = (deltaKm == Vector3(0.0f, 0.0f, 0.0f));
    const bool isRotatingThisFrame = (forwardYUpForAnchor != m_cloudAnchorPrevDirectionYUp);
    m_cloudAnchor.staticFrameCount =
      (isStaticThisFrame && isRotatingThisFrame) ? (m_cloudAnchor.staticFrameCount + 1u) : 0u;

    // A session-long "has it ever moved" latch plus the accumulated view rotation, which together
    // gate the warning below. They measure the raw view-matrix position, not the resolved anchor,
    // because the question is whether the view matrix itself ever moves on this engine. everMoved
    // compares against the session's first sample, so a move and return is still caught, and the
    // first frame is skipped so the default start state never counts as movement or rotation.
    if (!m_cloudAnchorHasFirstSample) {
      m_cloudAnchorFirstRawWorldUnits = rawWorldUnits;
      m_cloudAnchorHasFirstSample = true;
    } else {
      if (rawWorldUnits != m_cloudAnchorFirstRawWorldUnits) {
        m_cloudAnchor.everMoved = true;
      }
      const float cosAngle = std::min(std::max(
        dot(forwardYUpForAnchor, m_cloudAnchorPrevDirectionYUp), -1.0f), 1.0f);
      m_cloudAnchor.cumulativeRotationRadians += std::acos(cosAngle);
    }

    // A host cut flag cannot catch a teleport on an engine with rotation-only view matrices, so a
    // jump far above the recent speed counts as a cut. Testing against the speed average keeps
    // sustained fast flight from forcing a full dome bake every frame.
    const float deltaLenKm = length(deltaKm);
    m_cloudAnchorSpeedEmaKm = m_cloudAnchorSpeedEmaKm <= 0.0f
      ? deltaLenKm
      : (0.9f * m_cloudAnchorSpeedEmaKm + 0.1f * deltaLenKm);
    constexpr float kAnchorCutOutlierFactor = 8.0f;
    m_cloudAnchorCutThisFrame =
      deltaLenKm > std::max(m_settings.cloudAnchorCutKm, 0.0f)
      && deltaLenKm > kAnchorCutOutlierFactor * m_cloudAnchorSpeedEmaKm;

    m_cloudAnchor.rawWorldUnits         = rawWorldUnits;
    m_cloudAnchor.rawWorldUnitsFreecam  = rawWorldUnitsFreecam;
    m_cloudAnchor.resolvedRawWorldUnits = resolvedRawWorldUnits;
    m_cloudAnchor.posYUpKm              = posYUpKm;
    m_cloudAnchor.prevPosYUpKm          = prevPosYUpKm;
    m_cloudAnchor.deltaKm               = deltaKm;
    m_cloudAnchor.source                = resolvedSource;
    m_cloudAnchorPrevDirectionYUp       = forwardYUpForAnchor;

    const float resolvedScale = worldUnitsPerKm();
    NUMOS_ONCE(logInfo(format(
      "[RTX Atmosphere] Cloud anchor calibration (one-shot): source=",
      (useOverride ? "CameraWorldOverride" : "CameraViewMatrix"),
      " rawWorldUnits=(", rawWorldUnits.x, ", ", rawWorldUnits.y, ", ", rawWorldUnits.z,
      ") rawWorldUnitsFreecam=(", rawWorldUnitsFreecam.x, ", ", rawWorldUnitsFreecam.y, ", ", rawWorldUnitsFreecam.z,
      ") resolvedRawWorldUnits=(", resolvedRawWorldUnits.x, ", ", resolvedRawWorldUnits.y, ", ", resolvedRawWorldUnits.z,
      ") posYUpKm=(", posYUpKm.x, ", ", posYUpKm.y, ", ", posYUpKm.z, ") worldUnitsPerKm=", resolvedScale,
      " unitsPerMeter=", resolveUnitsPerMeter(), " worldScale=", m_settings.worldScale,
      " zUp=", (isZUp ? "true" : "false"), " flipUpAxis=", (flipUpAxisForYUp ? "true" : "false"))));

    // Gamebryo-family engines (Fallout: New Vegas) put the camera translation in the world matrices
    // and hand D3D a rotation-only view matrix, so the camera position never moves. The signal is
    // not "static right now" (a player standing still gives that too) but "never moved" across four
    // full turns of view rotation, by which point a real session would almost certainly have moved.
    // Raise kWarnRotationRadians if this fires on an engine that is fine. With the override on the
    // problem already has its fix, so the warning is skipped.
    constexpr float kWarnRotationRadians = 4.0f * 2.0f * kPi;
    if (!useOverride && !m_cloudAnchor.everMoved && m_cloudAnchor.cumulativeRotationRadians > kWarnRotationRadians) {
      NUMOS_ONCE(logWarn(format(
        "[RTX Atmosphere] Cloud anchor position has not changed even once across ",
        m_cloudAnchor.cumulativeRotationRadians, " radians (~",
        m_cloudAnchor.cumulativeRotationRadians / (2.0f * kPi), " full turns) of accumulated "
        "view rotation this session. A real play session ordinarily moves the tracked position at "
        "least once well before that much looking-around accumulates; never seeing that suggests "
        "this engine keeps camera translation out of the D3D view matrix (RtCamera::getPosition() "
        "may be reading a rotation-only matrix — see rtx_camera.cpp:57-59). If so, enable "
        "rtx.atmosphere.useCameraWorldOverride and push the game's real camera position through "
        "rtx.atmosphere.cameraWorldOverride instead of relying on this path.")));
    }
  }

  // The ground level under the resolved anchor (manual, or measured: updateGroundLevel). After the
  // anchor, which it is evaluated at; before anything reads the cloud frame.
  updateGroundLevel(rec, m_cloudAnchor.resolvedRawWorldUnits - m_inputs.cameraPosition);

  m_cloudRenderFrameIdx = m_inputs.frameIndex;

  // The density frame uses the resolved anchor, so the override reaches the shaders
  // (args.cameraWorldPosYUpKm) the same way it reaches everything else.
  setCloudShadowCameraPosition(m_cloudAnchor.posYUpKm);

  // Aerial perspective is fitted to this frame's frustum; set it before any getAtmosphereArgs read.
  // The Camera constants are m_inputs.camera, bound per dispatch.
  setAerialPerspectiveCamera();

  // Placement uses this frame's camera position.
  placeLightning();

  ensureCloudRenderRT(rec, m_inputs.renderWidth, m_inputs.renderHeight);

  computeLuts(rec);
  syncDistantLights(getAtmosphereArgs());
}

// Publishes everything the host binds for its own shaders.
void Renderer::Impl::publishOutputs() {
  FrameOutputs& o = m_outputs;
  o.args = getAtmosphereArgs();
  o.transmittanceLut = m_transmittanceLut.handle;
  o.multiscatteringLut = m_multiscatteringLut.handle;
  o.skyViewLut = m_skyViewLut.handle;
  o.cloudSkyTransmittanceLut = m_cloudSkyTransmittanceLut.handle;
  o.cloudShadowMap = m_cloudShadowMap.handle;
  o.cloudSecondaryLut = m_cloudSecondaryLut.handle;
  o.cloudRender = m_cloudDenoisedThisFrame ? m_cloudDenoisedRT.handle : m_cloudRenderRT.handle;
  o.cloudDepth = m_cloudDepthRT.handle;
  o.apNearScatter = m_apNearScatter.handle;
  o.apFarScatter = m_apFarScatter.handle;
  o.apFarTransmittance = m_apFarTransmittance.handle;
  o.apFroxelLighting = m_apFroxelLighting[m_apHistoryIndex].handle;
  o.apFroxelAux = m_apFroxelAux[m_apHistoryIndex].handle;
  o.apLightInjection = m_apLightInjection.handle;
  o.groundLevelWorldUnits = m_groundLevelWorldUnits;
  o.luxPerUnit = luxPerUnit(m_settings);
  o.nightGain = m_nightGain;
  o.groundLevelMeasured = m_groundLevelMeasured;
  o.cloudMarchStride = cloudMarchStride();
  o.apLights = m_apLightBuffer.handle;
  o.apLightClusters = m_apLightClusterBuffer.handle;
  o.cloudNvdfSdf = m_cloudNvdfSdf[m_cloudNvdfSdfFront].handle;
  o.cloudDetailNoise3D = cloudDensityNoise3D().handle;
  o.cloudCompositeActive = m_cloudCompositeActive;
  o.radianceScale = m_radianceScale;
  const bool nightSky = m_nightSky.uploadedGeneration != 0;
  o.nightSkyStarCells = nightSky ? m_nightSkyStarCells.handle : 0;
  o.nightSkyStars = nightSky ? m_nightSkyStarRecords.handle : 0;
  o.nightSkyMilkyWay = nightSky ? m_nightSkyMilkyWay.handle : 0;
  o.nightSkyStarlightLow = nightSky ? m_nightSkyStarlightLow.handle : 0;
  o.nightSkyMoon = nightSky ? m_nightSkyMoon.handle : 0;
}

namespace {
  constexpr float kFhPi = 3.14159265358979323846f;

  inline float fhSmoothstep(float e0, float e1, float x) {
    const float denom = e1 - e0;
    float t = (denom != 0.0f) ? (x - e0) / denom : 0.0f;
    t = std::min(std::max(t, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  inline Vector3 fhMul(const Vector3& a, const Vector3& b) {
    return Vector3(a.x * b.x, a.y * b.y, a.z * b.z);
  }

  inline float fhOzoneDensity(const AtmosphereArgs& a, float altitudeKm) {
    const float halfWidth = std::max(a.ozoneLayerWidth, 1e-3f);
    return std::max(0.0f, 1.0f - std::abs(altitudeKm - a.ozoneLayerAltitude) / halfWidth);
  }

  // Ray-sphere roots for a unit-length direction, sorted. Returns false when the ray misses.
  bool fhIntersectSphere(const Vector3& origin, const Vector3& direction, const Vector3& center,
                         float radius, float& outNear, float& outFar) {
    const Vector3 oc = origin - center;
    const float b = 2.0f * dot(oc, direction);
    const float c = dot(oc, oc) - radius * radius;
    const float discriminant = b * b - 4.0f * c;

    if (discriminant < 0.0f) {
      return false;
    }

    const float sqrtDiscriminant = std::sqrt(discriminant);
    outNear = (-b - sqrtDiscriminant) * 0.5f;
    outFar = (-b + sqrtDiscriminant) * 0.5f;

    return true;
  }

  // CPU counterpart of the GPU transmittance bake. It marches the optical depth from ground level
  // toward dirYUp through the spherical atmosphere: the same integral transmittance_lut.comp.slang
  // bakes, with the same tent ozone profile and Mie scattering + absorption extinction, so the
  // distant sun and moon lights agree with the LUT-lit sky.
  //
  // A plane-parallel Kasten-Young air mass would be cheaper but drops nearly all the ozone (its
  // usual `airMass * 0.15` term gives ~7.5e-4 optical depth at zenith against a physical ~0.028)
  // and needs an ad-hoc twilight fade. The spherical march returns zero once the planet occludes
  // the body.
  //
  // dirYUp must be normalized and in Y-up space. Cost is 40 steps once per body per frame.
  Vector3 fhAtmTransmittanceYUp(const AtmosphereArgs& a, const Vector3& dirYUp) {
    const Vector3 planetCenter(0.0f, -a.planetRadius, 0.0f);
    const Vector3 origin(0.0f, 0.0f, 0.0f);

    float tNear = 0.0f;
    float tFar = 0.0f;

    // Any intersection ahead of the origin means the body is below the local horizon. The radius is
    // nudged inward so a sample sitting exactly on the ground is not self shadowed.
    if (fhIntersectSphere(origin, dirYUp, planetCenter, a.planetRadius * (1.0f - 1e-5f), tNear, tFar)
        && tFar >= 0.0f) {
      return Vector3(0.0f, 0.0f, 0.0f);
    }

    // March to the top of the atmosphere.
    if (!fhIntersectSphere(origin, dirYUp, planetCenter, a.atmosphereRadius, tNear, tFar) || tFar <= 0.0f) {
      return Vector3(1.0f, 1.0f, 1.0f);
    }

    const float tEnd = tFar;

    // Matches the shader's power-distributed steps: short near the origin where the air is densest.
    constexpr int kSteps = 40;
    constexpr float kStepExponent = 2.0f;
    Vector3 opticalDepth(0.0f, 0.0f, 0.0f);
    float segmentStart = 0.0f;

    for (int i = 0; i < kSteps; ++i) {
      const float segmentEnd = std::pow(float(i + 1) / float(kSteps), kStepExponent);
      const float dt = (segmentEnd - segmentStart) * tEnd;
      const float t = (segmentStart + (segmentEnd - segmentStart) * 0.5f) * tEnd;
      segmentStart = segmentEnd;

      if (dt <= 0.0f) {
        continue;
      }

      const Vector3 samplePos = origin + dirYUp * t;
      const float h = std::min(
        std::max(length(samplePos - planetCenter) - a.planetRadius, 0.0f), a.atmosphereThickness);

      const float densityR = std::exp(-h / std::max(a.rayleighScaleHeight, 1e-3f));
      const float densityM = std::exp(-h / std::max(a.mieScaleHeight, 1e-3f));
      const float densityO3 = fhOzoneDensity(a, h);

      opticalDepth.x += (a.rayleighScattering.x * densityR
                       + (a.mieScattering.x + a.mieAbsorption.x) * densityM
                       + a.ozoneAbsorption.x * densityO3) * dt;
      opticalDepth.y += (a.rayleighScattering.y * densityR
                       + (a.mieScattering.y + a.mieAbsorption.y) * densityM
                       + a.ozoneAbsorption.y * densityO3) * dt;
      opticalDepth.z += (a.rayleighScattering.z * densityR
                       + (a.mieScattering.z + a.mieAbsorption.z) * densityM
                       + a.ozoneAbsorption.z * densityO3) * dt;
    }

    return Vector3(
      std::exp(-std::min(opticalDepth.x, 1e3f)),
      std::exp(-std::min(opticalDepth.y, 1e3f)),
      std::exp(-std::min(opticalDepth.z, 1e3f)));
  }

  // Transmittance of a ray grazing the planet at tangentHeightKm, through the whole atmosphere and out
  // the other side: twice the optical depth from the tangent point outward.
  Vector3 fhGrazingTransmittance(const AtmosphereArgs& a, float tangentHeightKm) {
    const float r0 = a.planetRadius + tangentHeightKm;
    const float tEnd = std::sqrt(std::max(a.atmosphereRadius * a.atmosphereRadius - r0 * r0, 0.0f));
    constexpr int kSteps = 64;
    Vector3 opticalDepth(0.0f, 0.0f, 0.0f);
    float segmentStart = 0.0f;
    for (int i = 0; i < kSteps; ++i) {
      const float segmentEnd = std::pow(float(i + 1) / float(kSteps), 2.0f);
      const float dt = (segmentEnd - segmentStart) * tEnd;
      const float t = (segmentStart + (segmentEnd - segmentStart) * 0.5f) * tEnd;
      segmentStart = segmentEnd;
      const float h = std::min(std::sqrt(r0 * r0 + t * t) - a.planetRadius, a.atmosphereThickness);
      const float densityR = std::exp(-h / std::max(a.rayleighScaleHeight, 1e-3f));
      const float densityM = std::exp(-h / std::max(a.mieScaleHeight, 1e-3f));
      const float densityO3 = fhOzoneDensity(a, h);
      opticalDepth += (a.rayleighScattering * densityR + (a.mieScattering + a.mieAbsorption) * densityM
                       + a.ozoneAbsorption * densityO3) * (2.0f * dt);
    }
    return Vector3(std::exp(-std::min(opticalDepth.x, 80.0f)), std::exp(-std::min(opticalDepth.y, 80.0f)),
                   std::exp(-std::min(opticalDepth.z, 80.0f)));
  }
}  // anonymous namespace

void Renderer::Impl::syncDistantLights(const AtmosphereArgs& args) {
  const bool isZUp = m_inputs.zUp;
  const float radScale = m_settings.directionalLightRadianceScale;
  constexpr float kMinHalfAngle = 0.0005f;

  // Inverse of worldToAtmosphereYUp(). Both the axis swap and the up flip are their own inverses,
  // so the same operations in the opposite order take a Y-up direction back to world space.
  const bool flipUpAxis = m_settings.flipUpAxis;
  auto toWorld = [isZUp, flipUpAxis](const Vector3& yup) -> Vector3 {
    const Vector3 flipped = flipUpAxis ? Vector3(yup.x, -yup.y, yup.z) : yup;
    return isZUp ? Vector3(flipped.x, flipped.z, flipped.y) : flipped;
  };

  // Fills a distant light slot; the host turns each slot into its own light.
  auto ensureLight = [&](EmittedLight& slot, const Vector3& propDir, float halfAngle, const Vector3& radiance, bool cloudShadowed) {
    const Vector3 clamped(std::max(radiance.x, 0.0f), std::max(radiance.y, 0.0f), std::max(radiance.z, 0.0f));
    slot.type = EmittedLight::Type::Distant;
    slot.active = true;
    slot.direction = propDir;
    slot.halfAngle = std::max(halfAngle, kMinHalfAngle);
    slot.radiance = clamped;
    slot.cloudShadowed = cloudShadowed;
  };
  EmittedLight* lights = m_outputs.lights;

  // ---- Sun (always present in Numos; radiance 0 below horizon) ----
  {
    const Vector3 sunDirYUp(args.sunDirection.x, args.sunDirection.y, args.sunDirection.z);
    Vector3 radiance(0.0f, 0.0f, 0.0f);
    if (sunDirYUp.y > 0.0f) {
      // Direct beam: the illuminance at the top of the atmosphere times the air's transmittance.
      const Vector3 T = fhAtmTransmittanceYUp(args, sunDirYUp);
      const Vector3 sunIll(args.sunIlluminance.x, args.sunIlluminance.y, args.sunIlluminance.z);
      const Vector3 sample = fhMul(sunIll, T);
      radiance = sample * (radScale / kFhPi);
    }
    const float softnessDeg = m_settings.sunShadowSoftnessDeg;
    const float sunHalfAngle = (softnessDeg > 0.0f) ? (softnessDeg * (kFhPi / 180.0f))
                                                     : args.sunAngularRadius;
    const Vector3 toSun = toWorld(sunDirYUp);
    const Vector3 propDir = (sunDirYUp.y > 0.0f) ? Vector3(-toSun.x, -toSun.y, -toSun.z)
                                                  : Vector3(0.0f, -1.0f, 0.0f);
    // The cloud shadow map is baked along the clouds' key light (cloudKeyLight), so it shades the
    // sun only while the sun is that light.
    ensureLight(lights[FrameOutputs::kSunLight], propDir, sunHalfAngle, radiance,
                /*cloudShadowed=*/args.cloudKeyLight == 0u);
  }

  // ---- Moons: the same distant light as the sun, carrying each moon's physical illuminance (its
  // phase law and colour; see updateCelestial), so the moon/sun ratio on surfaces is the real one.
  // Emitted while the moon lights the sky (moonSkyLightMask), that is, not lost under the sun. ----
  for (uint32_t i = 0; i < MAX_MOONS; ++i) {
    const MoonParams& m = args.moons[i];
    const Vector3 dirRaw(m.direction.x, m.direction.y, m.direction.z);
    const float len = std::sqrt(dirRaw.x * dirRaw.x + dirRaw.y * dirRaw.y + dirRaw.z * dirRaw.z);
    const bool lit = (m.enabled >= 0.5f) && ((args.moonSkyLightMask >> i) & 1u) != 0u && (len > 1e-4f)
                  && dirRaw.y > 0.0f;

    if (!lit && !lights[FrameOutputs::kFirstMoonLight + i].active) {
      continue;
    }

    const Vector3 dirN = (len > 1e-4f) ? Vector3(dirRaw.x / len, dirRaw.y / len, dirRaw.z / len)
                                       : Vector3(0.0f, 1.0f, 0.0f);
    Vector3 radiance(0.0f, 0.0f, 0.0f);
    if (lit) {
      const Vector3 T = fhAtmTransmittanceYUp(args, dirN);
      const Vector3 moonIll(m.illuminance.x, m.illuminance.y, m.illuminance.z);
      const Vector3 sample = fhMul(moonIll, T);
      radiance = sample * (radScale / kFhPi);
    }
    const Vector3 toMoon = toWorld(dirN);
    const Vector3 propDir = lit ? Vector3(-toMoon.x, -toMoon.y, -toMoon.z) : Vector3(0.0f, -1.0f, 0.0f);
    // The real moon's half-angle (0.26 deg) whatever its drawn size: that is what softens its shadows.
    ensureLight(lights[FrameOutputs::kFirstMoonLight + i], propDir, 0.2605f * (kFhPi / 180.0f), radiance,
                /*cloudShadowed=*/args.cloudKeyLight == i + 1u);
  }

  // ---- Lightning: the flash as a scene light, a sphere at the strike ----
  // The same source as the cloud glow: args.lightningFlashIntensity is the flash's luminous intensity
  // (illuminance x km^2, pre-exposed once in getAtmosphereArgs), so a sphere of radius r carries
  // radiance I / (pi r^2). Not cloudShadowed: the host's cloud shadow is the key light's directional
  // map and says nothing about a source inside the deck, and the cloud below the strike diffuses the
  // flash rather than absorbing it, so about the half an isotropic source sends downward still leaves
  // through the base.
  {
    constexpr float kRadiusKm = kLightningRadiusKm;
    const bool lit = m_settings.lightningEnable && m_settings.lightningSceneLight
                  && args.lightningFlashIntensity > 0.0f;
    if (lit || lights[FrameOutputs::kLightningLight].active) {
      Vector3 radiance(0.0f, 0.0f, 0.0f);
      Vector3 posWorld(0.0f, 0.0f, 0.0f);
      if (lit) {
        const Vector3 c(args.lightningColor.x, args.lightningColor.y, args.lightningColor.z);
        radiance = c * (args.lightningFlashIntensity / (kFhPi * kRadiusKm * kRadiusKm));
        posWorld = lightningRenderPosition();
      }
      const float radiusWorld = kRadiusKm * args.worldUnitsPerKm;
      // An unshaped sphere light, inactive between flashes.
      EmittedLight& slot = lights[FrameOutputs::kLightningLight];
      slot.type = EmittedLight::Type::Sphere;
      slot.active = lit;
      slot.position = posWorld;
      slot.radius = radiusWorld;
      slot.radiance = radiance;
      slot.cloudShadowed = false;
    }
  }
}

} // namespace numos
