// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Weather: a small set of the physical model's own settings that a weather state moves, named and
// measured exactly like the Settings fields they override. A host hands the renderer a
// WeatherSnapshot (FrameInputs::weather) and the renderer applies it over the user's settings for
// that frame (applyWeather), so every consumer sees one consistent value and the user's settings stay
// the baseline. Nothing here is artistic: clouds (coverage, type, density, depth, base), the second
// cloud layer (ground fog, a stratus or high deck), wind, the air (molecular and aerosol density),
// height fog and the lightning rate. Precipitation belongs to the host's particle system; the night
// sky, the sun and the colours are not weather.
//
// Presets (weatherPreset) are physically motivated starting points built on top of a base Settings;
// blendWeather / WeatherBlender interpolate between snapshots with per-field rules.

#include <cstdint>

#include "numos/settings.h"

namespace numos {

  enum class WeatherFieldKind : uint8_t {
    Scalar,      // linear
    Angle,       // degrees, shortest way round
    Visibility,  // a visibility distance where 0 means none: blended as extinction (1 / distance)
    Flag,        // a bool Settings field held as 0 / 1: on through a transition if either end is on
  };

// X(name, kind, label). Every name is a float Settings field of the same unit, or a bool (Flag).
// Layer 2 blends as a unit: while one end has it off (or at zero coverage), that end takes the other
// end's height, depth, type and density at zero coverage, so the layer fades in or out where it
// stands instead of sweeping down from wherever the switched-off settings left it.
#define NUMOS_WEATHER_FIELDS(X)                                                              \
  X(cloudCoverageMean,         WeatherFieldKind::Scalar,     "Cloud coverage")               \
  X(cloudCoverageSpread,       WeatherFieldKind::Scalar,     "Coverage spread")              \
  X(cloudCoverageNoiseScale,   WeatherFieldKind::Scalar,     "Coverage patch frequency")     \
  X(cloudTypeMean,             WeatherFieldKind::Scalar,     "Cloud type")                   \
  X(cloudTypeSpread,           WeatherFieldKind::Scalar,     "Type spread")                  \
  X(cloudDensity,              WeatherFieldKind::Scalar,     "Cloud density")                \
  X(cloudBaseHeightMeters,     WeatherFieldKind::Scalar,     "Cloud base (m)")               \
  X(cloudDepthMeters,          WeatherFieldKind::Scalar,     "Cloud depth (m)")              \
  X(cloudLayer2Enable,         WeatherFieldKind::Flag,       "Layer 2")                      \
  X(cloudLayer2CoverageMean,   WeatherFieldKind::Scalar,     "Layer 2 coverage")             \
  X(cloudLayer2BaseHeightMeters, WeatherFieldKind::Scalar,   "Layer 2 base (m)")             \
  X(cloudLayer2DepthMeters,    WeatherFieldKind::Scalar,     "Layer 2 depth (m)")            \
  X(cloudLayer2TypeMean,       WeatherFieldKind::Scalar,     "Layer 2 type")                 \
  X(cloudLayer2DensityScale,   WeatherFieldKind::Scalar,     "Layer 2 density")              \
  X(cloudWindSpeed,            WeatherFieldKind::Scalar,     "Wind speed (km/s)")            \
  X(cloudWindDirection,        WeatherFieldKind::Angle,      "Wind direction (deg)")         \
  X(airDensity,                WeatherFieldKind::Scalar,     "Air density")                  \
  X(aerosolDensity,            WeatherFieldKind::Scalar,     "Aerosol density")              \
  X(fogVisibilityMeters,       WeatherFieldKind::Visibility, "Fog visibility (m, 0 = none)") \
  X(fogHeightFalloffMeters,    WeatherFieldKind::Scalar,     "Fog height falloff (m)")       \
  X(fogBaseHeightMeters,       WeatherFieldKind::Scalar,     "Fog base (m)")                 \
  X(fogAlbedo,                 WeatherFieldKind::Scalar,     "Fog albedo")                   \
  X(lightningStrikesPerMinute, WeatherFieldKind::Scalar,     "Lightning (strikes/min)")        X(nubis3ErosionStrength,     WeatherFieldKind::Scalar,     "Detail erosion")                 X(cloudBodyBillowSizeKm,     WeatherFieldKind::Scalar,     "Billow size (km)")               X(cloudBodyBillowGain,       WeatherFieldKind::Scalar,     "Billow gain")                    X(cloudCurlStrengthKm,       WeatherFieldKind::Scalar,     "Detail curl (km)")               X(cloudDiffuseSunStrength,   WeatherFieldKind::Scalar,     "Diffuse sunlight")

  // Colour settings blend per channel: one scalar weather field per component (name, setting, component).
#define NUMOS_WEATHER_COMPONENT_FIELDS(X)                                                    \
  X(airTintR,     airTint,     x, "Air tint R")                                             \
  X(airTintG,     airTint,     y, "Air tint G")                                             \
  X(airTintB,     airTint,     z, "Air tint B")                                             \
  X(aerosolTintR, aerosolTint, x, "Aerosol tint R")                                         \
  X(aerosolTintG, aerosolTint, y, "Aerosol tint G")                                         \
  X(aerosolTintB, aerosolTint, z, "Aerosol tint B")

  struct WeatherSnapshot {
#define NUMOS_WEATHER_MEMBER_(name, kind, label) float name = float(Settings{}.name);
    NUMOS_WEATHER_FIELDS(NUMOS_WEATHER_MEMBER_)
#undef NUMOS_WEATHER_MEMBER_
#define NUMOS_WEATHER_COMPONENT_MEMBER_(name, setting, c, label) float name = Settings{}.setting.c;
    NUMOS_WEATHER_COMPONENT_FIELDS(NUMOS_WEATHER_COMPONENT_MEMBER_)
#undef NUMOS_WEATHER_COMPONENT_MEMBER_
  };

  struct WeatherFieldInfo {
    const char*      name;
    WeatherFieldKind kind;
    const char*      label;
    float WeatherSnapshot::* member;
  };

  // Every field, in list order.
  const WeatherFieldInfo* weatherFields(size_t& count);

  // The weather the settings themselves describe, and the settings with a weather applied.
  WeatherSnapshot weatherFromSettings(const Settings& settings);
  void applyWeather(const WeatherSnapshot& weather, Settings& settings);

  // a at t = 0, b at t = 1, per field kind; t is clamped to [0, 1].
  WeatherSnapshot blendWeather(const WeatherSnapshot& a, const WeatherSnapshot& b, float t);

  enum class WeatherPreset : uint32_t {
    Clear,         // no cloud, clean air
    Fair,          // scattered fair-weather cumulus
    PartlyCloudy,  // the default cumulus field
    Cloudy,        // broken, deeper cumulus
    Overcast,      // a low, nearly closed deck
    Haze,          // thin cloud, heavy aerosol
    Fog,           // a ground fog bank (cloud layer 2) and height fog under a low deck
    Storm,         // dark, dense broken convection under a grey sheet, with lightning
    Drizzle,       // the storm's sky, lighter: thinner broken cloud under a grey sheet, no lightning
    Altostratus,   // a thin high grey sheet the sun shows through, little cloud below
    Count
  };

  const char* weatherPresetName(WeatherPreset preset);
  bool weatherPresetFromName(const char* name, WeatherPreset& preset);  // case-insensitive
  // The preset built over `base`: fields the preset does not set keep the base's value.
  WeatherSnapshot weatherPreset(WeatherPreset preset, const Settings& base);

  // Eases from the current weather to a target over a duration (smoothstep in time). The host calls
  // update once per frame and passes &current() as FrameInputs::weather.
  class WeatherBlender {
  public:
    explicit WeatherBlender(const WeatherSnapshot& start = WeatherSnapshot{}) : m_from(start), m_to(start), m_current(start) {}
    // Starts a transition from wherever the weather is now; seconds <= 0 jumps.
    void setTarget(const WeatherSnapshot& target, float seconds);
    void update(float deltaSeconds);
    const WeatherSnapshot& current() const { return m_current; }
    bool transitioning() const { return m_elapsed < m_duration; }

  private:
    WeatherSnapshot m_from, m_to, m_current;
    float m_duration = 0.0f;
    float m_elapsed = 0.0f;
  };

} // namespace numos
