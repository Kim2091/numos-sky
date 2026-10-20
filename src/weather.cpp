// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Weather snapshots, presets and blending (include/numos/weather.h).
#include "numos/weather.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace numos {

namespace {

  const WeatherFieldInfo kFields[] = {
#define NUMOS_WEATHER_INFO_(name, kind, label) { #name, kind, label, &WeatherSnapshot::name },
    NUMOS_WEATHER_FIELDS(NUMOS_WEATHER_INFO_)
#undef NUMOS_WEATHER_INFO_
#define NUMOS_WEATHER_COMPONENT_INFO_(name, setting, c, label) { #name, WeatherFieldKind::Scalar, label, &WeatherSnapshot::name },
    NUMOS_WEATHER_COMPONENT_FIELDS(NUMOS_WEATHER_COMPONENT_INFO_)
#undef NUMOS_WEATHER_COMPONENT_INFO_
  };

  float lerpAngleDeg(float a, float b, float t) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    float r = std::fmod(a + d * t, 360.0f);
    return r < 0.0f ? r + 360.0f : r;
  }

  // Visibility v (0 = none) as extinction 1 / v, so fog thickens smoothly from none.
  float lerpVisibility(float a, float b, float t) {
    const float ea = a > 0.0f ? 1.0f / a : 0.0f;
    const float eb = b > 0.0f ? 1.0f / b : 0.0f;
    const float e = ea + (eb - ea) * t;
    return e > 0.0f ? 1.0f / e : 0.0f;
  }

  const char* const kPresetNames[] = {
    "Clear", "Fair", "PartlyCloudy", "Cloudy", "Overcast", "Haze", "Fog", "Storm", "Drizzle", "Altostratus",
  };
  static_assert(sizeof(kPresetNames) / sizeof(kPresetNames[0]) == size_t(WeatherPreset::Count),
                "one name per preset");

} // namespace

const WeatherFieldInfo* weatherFields(size_t& count) {
  count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

WeatherSnapshot weatherFromSettings(const Settings& settings) {
  WeatherSnapshot w;
#define NUMOS_WEATHER_FROM_(name, kind, label) w.name = float(settings.name);
  NUMOS_WEATHER_FIELDS(NUMOS_WEATHER_FROM_)
#undef NUMOS_WEATHER_FROM_
#define NUMOS_WEATHER_COMPONENT_FROM_(name, setting, c, label) w.name = settings.setting.c;
  NUMOS_WEATHER_COMPONENT_FIELDS(NUMOS_WEATHER_COMPONENT_FROM_)
#undef NUMOS_WEATHER_COMPONENT_FROM_
  return w;
}

void applyWeather(const WeatherSnapshot& weather, Settings& settings) {
#define NUMOS_WEATHER_APPLY_(name, kind, label) settings.name = static_cast<decltype(settings.name)>(weather.name);
  NUMOS_WEATHER_FIELDS(NUMOS_WEATHER_APPLY_)
#undef NUMOS_WEATHER_APPLY_
#define NUMOS_WEATHER_COMPONENT_APPLY_(name, setting, c, label) settings.setting.c = weather.name;
  NUMOS_WEATHER_COMPONENT_FIELDS(NUMOS_WEATHER_COMPONENT_APPLY_)
#undef NUMOS_WEATHER_COMPONENT_APPLY_
}

namespace {

  bool layer2Active(const WeatherSnapshot& w) {
    return w.cloudLayer2Enable > 0.5f && w.cloudLayer2CoverageMean > 0.0f;
  }

  // An end without layer 2 holds it where the other end has it, at zero coverage (weather.h).
  void holdLayer2(WeatherSnapshot& off, const WeatherSnapshot& on) {
    off.cloudLayer2CoverageMean = 0.0f;
    off.cloudLayer2BaseHeightMeters = on.cloudLayer2BaseHeightMeters;
    off.cloudLayer2DepthMeters = on.cloudLayer2DepthMeters;
    off.cloudLayer2TypeMean = on.cloudLayer2TypeMean;
    off.cloudLayer2DensityScale = on.cloudLayer2DensityScale;
  }

} // namespace

WeatherSnapshot blendWeather(const WeatherSnapshot& aIn, const WeatherSnapshot& bIn, float t) {
  t = std::min(std::max(t, 0.0f), 1.0f);
  if (t <= 0.0f) {
    return aIn;
  }
  if (t >= 1.0f) {
    return bIn;
  }
  WeatherSnapshot a = aIn;
  WeatherSnapshot b = bIn;
  if (!layer2Active(a) && layer2Active(b)) {
    holdLayer2(a, b);
  } else if (layer2Active(a) && !layer2Active(b)) {
    holdLayer2(b, a);
  }
  WeatherSnapshot r;
  for (const WeatherFieldInfo& f : kFields) {
    const float x = a.*f.member;
    const float y = b.*f.member;
    switch (f.kind) {
    case WeatherFieldKind::Scalar:     r.*f.member = x + (y - x) * t; break;
    case WeatherFieldKind::Angle:      r.*f.member = lerpAngleDeg(x, y, t); break;
    case WeatherFieldKind::Visibility: r.*f.member = lerpVisibility(x, y, t); break;
    case WeatherFieldKind::Flag:       r.*f.member = std::max(x, y); break;
    }
  }
  return r;
}

const char* weatherPresetName(WeatherPreset preset) {
  const uint32_t i = uint32_t(preset);
  return i < uint32_t(WeatherPreset::Count) ? kPresetNames[i] : "";
}

bool weatherPresetFromName(const char* name, WeatherPreset& preset) {
  if (!name) return false;
  for (uint32_t i = 0; i < uint32_t(WeatherPreset::Count); ++i) {
    const char* a = kPresetNames[i];
    const char* b = name;
    while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b))) {
      ++a;
      ++b;
    }
    if (!*a && !*b) {
      preset = WeatherPreset(i);
      return true;
    }
  }
  return false;
}

// Values are physical where the model is: cloud base and depth in metres from typical climatology
// (fair-weather cumulus ~1 km deep, stratiform decks low and thin, cumulonimbus to ~8 km), fog as a
// meteorological visibility (fog < 1 km, mist and rain haze a few km), aerosol as a multiple of the
// clean continental default. Coverage and type are the field's own [0, 1] controls.
// Layer 2 is off in every preset but Fog, Overcast, Storm, Drizzle and Altostratus. In Fog it is the
// fog bank: a stratus sheet on the ground, 200 m deep, nearly closed, whose body sees about 400 m at
// its base (cloudDensity 20 /km x 0.5) and thins upward, over the preset's lighter height fog. In the
// others it is a thin grey sheet high up. Only Fog's low deck (75 %) reaches into the closed-deck
// model (cloudDeckBodyFieldKm, 70-90 % coverage); the grey-sky presets use broken cloud instead.
namespace {
// Overcast, Storm and Drizzle: towering cumulus (type 1, whose bodies keep their lumps down to the
// base) with small, strongly eroded billows, under a grey layer-2 sheet at 5.5 km. The look is
// detailed, sunlit cloud in a grey sky rather than a closed, featureless deck.
void stormSky(WeatherSnapshot& w, const Settings& base) {
  (void)base;
  w.cloudTypeMean = 1.0f;
  w.cloudBaseHeightMeters = 800.0f;
  w.fogHeightFalloffMeters = 600.0f;
  w.nubis3ErosionStrength = 1.0f;
  w.cloudBodyBillowSizeKm = 0.8f;
  w.cloudBodyBillowGain = 0.8f;
  w.cloudCurlStrengthKm = 0.25f;
  w.cloudDiffuseSunStrength = 1.0f;
  w.cloudLayer2Enable = 1.0f;
  w.cloudLayer2CoverageMean = 0.97f;
  w.cloudLayer2BaseHeightMeters = 5500.0f;
  w.cloudLayer2DepthMeters = 600.0f;
  w.cloudLayer2TypeMean = 0.0f;
  w.cloudLayer2DensityScale = 0.5f;
}
}  // namespace

WeatherSnapshot weatherPreset(WeatherPreset preset, const Settings& base) {
  WeatherSnapshot w = weatherFromSettings(base);
  w.fogVisibilityMeters = 0.0f;
  w.lightningStrikesPerMinute = 0.0f;
  w.cloudLayer2Enable = 0.0f;
  w.cloudLayer2CoverageMean = 0.0f;
  switch (preset) {
  case WeatherPreset::Clear:
    w.cloudCoverageMean = 0.0f;
    w.aerosolDensity = 0.6f;
    break;
  case WeatherPreset::Fair:
    w.cloudCoverageMean = 0.3f;
    w.cloudTypeMean = 0.8f;
    w.cloudBaseHeightMeters = 1200.0f;
    w.cloudDepthMeters = 1200.0f;
    w.aerosolDensity = 0.9f;
    break;
  case WeatherPreset::PartlyCloudy:
    w.cloudCoverageMean = 0.45f;
    w.cloudTypeMean = 0.7f;
    w.cloudBaseHeightMeters = 1300.0f;
    w.cloudDepthMeters = 2000.0f;
    break;
  case WeatherPreset::Cloudy:
    w.cloudCoverageMean = 0.62f;
    w.cloudTypeMean = 0.5f;
    w.cloudBaseHeightMeters = 1200.0f;
    w.cloudDepthMeters = 2500.0f;
    w.aerosolDensity = 1.2f;
    break;
  case WeatherPreset::Overcast:
    // Drizzle's sky, shallower and clearer: the same detailed cloud under the grey sheet, 1.8 km
    // deep, with a light haze.
    stormSky(w, base);
    w.cloudCoverageMean = 0.6f;
    w.cloudDensity = base.cloudDensity * 1.25f;
    w.cloudDepthMeters = 1800.0f;
    w.fogVisibilityMeters = 40000.0f;
    w.fogAlbedo = 0.5f;
    break;
  case WeatherPreset::Haze:
    w.cloudCoverageMean = 0.2f;
    w.cloudTypeMean = 0.6f;
    w.aerosolDensity = 4.0f;
    w.fogVisibilityMeters = 5000.0f;
    w.fogHeightFalloffMeters = 800.0f;
    w.fogAlbedo = 0.85f;
    break;
  case WeatherPreset::Fog:
    w.cloudCoverageMean = 0.75f;
    w.cloudTypeMean = 0.0f;
    w.cloudTypeSpread = 0.1f;
    w.cloudBaseHeightMeters = 400.0f;
    w.cloudDepthMeters = 800.0f;
    w.aerosolDensity = 2.5f;
    w.fogVisibilityMeters = 300.0f;
    w.fogHeightFalloffMeters = 150.0f;
    w.fogAlbedo = 0.95f;
    w.cloudWindSpeed = base.cloudWindSpeed * 0.3f;
    w.cloudLayer2Enable = 1.0f;
    w.cloudLayer2CoverageMean = 0.92f;
    w.cloudLayer2BaseHeightMeters = -20.0f;
    w.cloudLayer2DepthMeters = 220.0f;
    w.cloudLayer2TypeMean = 0.0f;
    w.cloudLayer2DensityScale = 0.5f;
    break;
  case WeatherPreset::Storm:
    // Dense broken convection, 4 km deep, under a grey sheet that dims the sun without hiding it:
    // the sun still picks out the towers' detail while the sky reads dark. Dark rain haze low down.
    stormSky(w, base);
    w.cloudCoverageMean = 0.65f;
    w.cloudDensity = base.cloudDensity * 1.5f;
    w.cloudDepthMeters = 4000.0f;
    w.cloudWindSpeed = base.cloudWindSpeed * 1.75f;
    w.fogVisibilityMeters = 25000.0f;  // rain haze
    w.fogAlbedo = 0.2f;
    w.lightningStrikesPerMinute = 6.0f;
    break;
  case WeatherPreset::Drizzle:
    // The storm's sky, lighter: thinner and less dense cloud, a lighter grey haze, no lightning.
    stormSky(w, base);
    w.cloudCoverageMean = 0.58f;
    w.cloudDensity = base.cloudDensity * 1.25f;
    w.cloudDepthMeters = 3500.0f;
    w.cloudWindSpeed = base.cloudWindSpeed * 1.25f;
    w.fogVisibilityMeters = 20000.0f;
    w.fogAlbedo = 0.4f;
    break;
  case WeatherPreset::Altostratus:
    // A thin, nearly closed sheet at 4 km the sun shows through as a bright disc, a few small
    // cumulus below.
    w.cloudCoverageMean = 0.15f;
    w.cloudTypeMean = 0.5f;
    w.cloudLayer2Enable = 1.0f;
    w.cloudLayer2CoverageMean = 0.97f;
    w.cloudLayer2BaseHeightMeters = 4000.0f;
    w.cloudLayer2DepthMeters = 800.0f;
    w.cloudLayer2TypeMean = 0.0f;
    w.cloudLayer2DensityScale = 0.35f;
    break;
  case WeatherPreset::Count:
    break;
  }
  return w;
}

void WeatherBlender::setTarget(const WeatherSnapshot& target, float seconds) {
  m_from = m_current;
  m_to = target;
  m_duration = std::max(seconds, 0.0f);
  m_elapsed = 0.0f;
  if (m_duration <= 0.0f) {
    m_current = target;
  }
}

void WeatherBlender::update(float deltaSeconds) {
  if (m_elapsed >= m_duration) {
    return;
  }
  m_elapsed = std::min(m_elapsed + std::max(deltaSeconds, 0.0f), m_duration);
  const float x = m_elapsed / m_duration;
  m_current = blendWeather(m_from, m_to, x * x * (3.0f - 2.0f * x));
}

} // namespace numos
