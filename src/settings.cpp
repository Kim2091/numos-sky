// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "numos/settings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "numos/log.h"

namespace numos {

namespace {

template<typename T> constexpr SettingType settingTypeOf();
template<> constexpr SettingType settingTypeOf<bool>() { return SettingType::Bool; }
template<> constexpr SettingType settingTypeOf<int32_t>() { return SettingType::Int; }
template<> constexpr SettingType settingTypeOf<uint32_t>() { return SettingType::Uint; }
template<> constexpr SettingType settingTypeOf<float>() { return SettingType::Float; }
template<> constexpr SettingType settingTypeOf<Vector3>() { return SettingType::Vec3; }

std::vector<SettingInfo> buildInfos() {
  std::vector<SettingInfo> infos;
#define NUMOS_SETTING(type, name, def, key, mn, mx, flags, desc) \
  infos.push_back({ key, settingTypeOf<type>(), float(mn), float(mx), flags, desc, offsetof(Settings, name) });
#include "numos/settings_fields.inl"
  for (uint32_t m = 0; m < kMaxMoons; ++m) {
    const std::string prefix = "rtx.atmosphere.moon" + std::to_string(m) + ".";
    const size_t base = offsetof(Settings, moons) + m * sizeof(MoonSettings);
#define NUMOS_MOON_SETTING(type, name, def, key, mn, mx, flags, desc) \
    infos.push_back({ prefix + key, settingTypeOf<type>(), float(mn), float(mx), flags, desc, base + offsetof(MoonSettings, name) });
#include "numos/settings_fields.inl"
  }
  return infos;
}

std::string_view trim(std::string_view s) {
  const char* ws = " \t\r\n";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string_view::npos) {
    return {};
  }
  const size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
}

// from_chars rejects a leading '+', which hand-written configs use; accept one (not "+-1").
std::string_view stripPlus(std::string_view s) {
  if (s.size() > 1 && s.front() == '+' && s[1] != '-' && s[1] != '+') {
    s.remove_prefix(1);
  }
  return s;
}

// Finite values only: NaN would pass through the range clamp (every comparison with it is false)
// and infinity is never a meaningful setting.
bool parseFloat(std::string_view s, float& out) {
  s = stripPlus(trim(s));
  if (!s.empty() && (s.back() == 'f' || s.back() == 'F')) {
    s.remove_suffix(1);
  }
  float v;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v)) {
    return false;
  }
  out = v;
  return true;
}

template<typename T>
bool parseInt(std::string_view s, T& out) {
  s = stripPlus(trim(s));
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// Case-insensitive ("True", "true", "TRUE"), as rtx.conf files are written.
bool parseBool(std::string_view s, bool& out) {
  s = trim(s);
  auto is = [s](const char* word) {
    const size_t n = std::strlen(word);
    if (s.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
      const char c = (s[i] >= 'A' && s[i] <= 'Z') ? char(s[i] - 'A' + 'a') : s[i];
      if (c != word[i]) return false;
    }
    return true;
  };
  if (is("true") || s == "1") { out = true; return true; }
  if (is("false") || s == "0") { out = false; return true; }
  return false;
}

float clampTo(const SettingInfo& info, float v) {
  return std::min(std::max(v, info.minValue), info.maxValue);
}

// Integer clamp in double, also bounded by the type: an unbounded range (NUMOS_NO_MIN/MAX) or
// a value beyond 2^24 must not round through float, and float(INT32_MAX) cast back is out of range.
template<typename T>
T clampInt(const SettingInfo& info, T v) {
  const double lo = std::max(double(info.minValue), double(std::numeric_limits<T>::min()));
  const double hi = std::min(double(info.maxValue), double(std::numeric_limits<T>::max()));
  return T(std::min(std::max(double(v), lo), hi));
}

void* field(Settings& s, const SettingInfo& info) { return reinterpret_cast<char*>(&s) + info.offset; }
const void* field(const Settings& s, const SettingInfo& info) { return reinterpret_cast<const char*>(&s) + info.offset; }

// Shortest text that reads back to the same float. to_chars ignores the C locale, so a host that
// calls setlocale() cannot turn the decimal point into a comma (from_chars would then reject it).
std::string formatFloat(float v) {
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, r.ptr);
}

size_t indexOf(const std::string& key) {
  const SettingInfo* info = findSetting(key);
  return info ? size_t(info - settingInfos().data()) : size_t(-1);
}

bool isExplicit(const ExplicitSettings& e, const char* key) {
  const size_t i = indexOf(key);
  return i < e.size() && e[i];
}

void logMigrated(const char* from, const char* to) {
  std::string msg = std::string("[Deprecated Config] rtx.atmosphere.") + from + " has been migrated to rtx.atmosphere." + to +
                    ". Please re-save your config to get rid of this message.";
  log(LogLevel::Info, msg.c_str());
}

} // namespace

const std::vector<SettingInfo>& settingInfos() {
  static const std::vector<SettingInfo> infos = buildInfos();
  return infos;
}

const SettingInfo* findSetting(std::string_view key) {
  static const std::unordered_map<std::string_view, size_t> index = [] {
    std::unordered_map<std::string_view, size_t> m;
    const auto& infos = settingInfos();
    for (size_t i = 0; i < infos.size(); ++i) {
      m.emplace(infos[i].key, i);
    }
    return m;
  }();
  const auto it = index.find(key);
  return it == index.end() ? nullptr : &settingInfos()[it->second];
}

bool setSetting(Settings& settings, std::string_view key, std::string_view value) {
  const SettingInfo* info = findSetting(trim(key));
  if (!info) {
    return false;
  }
  void* p = field(settings, *info);
  switch (info->type) {
  case SettingType::Bool: {
    bool v;
    if (!parseBool(value, v)) return false;
    *static_cast<bool*>(p) = v;
    return true;
  }
  case SettingType::Int: {
    int32_t v;
    if (!parseInt(value, v)) return false;
    *static_cast<int32_t*>(p) = clampInt(*info, v);
    return true;
  }
  case SettingType::Uint: {
    uint32_t v;
    if (!parseInt(value, v)) return false;
    *static_cast<uint32_t*>(p) = clampInt(*info, v);
    return true;
  }
  case SettingType::Float: {
    float v;
    if (!parseFloat(value, v)) return false;
    *static_cast<float*>(p) = clampTo(*info, v);
    return true;
  }
  case SettingType::Vec3: {
    float c[3];
    std::string_view rest = trim(value);
    for (int i = 0; i < 3; ++i) {
      const size_t comma = rest.find(',');
      const std::string_view part = i < 2 ? rest.substr(0, comma) : rest;
      if ((i < 2 && comma == std::string_view::npos) || !parseFloat(part, c[i])) return false;
      if (i < 2) rest = rest.substr(comma + 1);
    }
    *static_cast<Vector3*>(p) = Vector3(clampTo(*info, c[0]), clampTo(*info, c[1]), clampTo(*info, c[2]));
    return true;
  }
  }
  return false;
}

std::string formatSetting(const Settings& settings, const SettingInfo& info) {
  const void* p = field(settings, info);
  switch (info.type) {
  case SettingType::Bool:  return *static_cast<const bool*>(p) ? "True" : "False";
  case SettingType::Int:   return std::to_string(*static_cast<const int32_t*>(p));
  case SettingType::Uint:  return std::to_string(*static_cast<const uint32_t*>(p));
  case SettingType::Float: return formatFloat(*static_cast<const float*>(p));
  case SettingType::Vec3: {
    const Vector3 v = *static_cast<const Vector3*>(p);
    return formatFloat(v.x) + ", " + formatFloat(v.y) + ", " + formatFloat(v.z);
  }
  }
  return {};
}

size_t applyConfig(Settings& settings, std::string_view text, ExplicitSettings* explicitlySet) {
  size_t applied = 0;
  if (explicitlySet) {
    explicitlySet->resize(settingInfos().size(), false);
  }
  // A UTF-8 byte order mark (Notepad) would otherwise glue itself to the first key.
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
    text.remove_prefix(3);
  }
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
    const size_t hash = line.find('#');
    if (hash != std::string_view::npos) {
      line = line.substr(0, hash);
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view key = trim(line.substr(0, eq));
    if (setSetting(settings, key, line.substr(eq + 1))) {
      ++applied;
      if (explicitlySet) {
        (*explicitlySet)[indexOf(std::string(key))] = true;
      }
    } else if (findSetting(key)) {
      const std::string msg = "[NUMOS] Ignored " + std::string(key) + " = '" + std::string(trim(line.substr(eq + 1))) +
                              "': not a valid value for this setting.";
      log(LogLevel::Warn, msg.c_str());
    }
  }
  return applied;
}

std::string writeConfig(const Settings& settings) {
  static const Settings defaults{};
  std::string out;
  for (const SettingInfo& info : settingInfos()) {
    if (info.flags & NUMOS_SETTING_NO_SAVE) {
      continue;
    }
    const std::string value = formatSetting(settings, info);
    if (value != formatSetting(defaults, info)) {
      out += info.key + " = " + value + "\n";
    }
  }
  return out;
}

void migrateDeprecated(Settings& s, const ExplicitSettings& e, float legacySceneScale) {
  static const Settings defaults{};

  // The cloud unit scale the deprecated values describe, captured before anything below resets them.
  const float legacyCloudUnitsPerKm =
    100000.0f * std::max(s.cloudScale > 0.0f ? s.cloudScale : legacySceneScale, 1e-5f);

  // km -> m, declining when the config already set the replacement.
  auto kmToMeters = [&](const char* from, float& src, float srcDefault, const char* to, float& dst) {
    if (!isExplicit(e, (std::string("rtx.atmosphere.") + from).c_str())) return;
    if (!isExplicit(e, (std::string("rtx.atmosphere.") + to).c_str())) {
      dst = src * 1000.0f;
      logMigrated(from, to);
    }
    src = srcDefault;
  };
  kmToMeters("cloudAltitude", s.cloudAltitude, defaults.cloudAltitude, "cloudBaseHeightMeters", s.cloudBaseHeightMeters);
  kmToMeters("cloudThickness", s.cloudThickness, defaults.cloudThickness, "cloudDepthMeters", s.cloudDepthMeters);
  kmToMeters("cloudLayer2Altitude", s.cloudLayer2Altitude, defaults.cloudLayer2Altitude,
             "cloudLayer2BaseHeightMeters", s.cloudLayer2BaseHeightMeters);
  kmToMeters("cloudLayer2Thickness", s.cloudLayer2Thickness, defaults.cloudLayer2Thickness,
             "cloudLayer2DepthMeters", s.cloudLayer2DepthMeters);

  // cloudLayer2Color tinted all of layer 2's light the way cloudColor does layer 1's. Layer 2 takes
  // cloudColor and a physical albedo, so the tint becomes its ratio to cloudColor (at most 1: a tint
  // brighter than cloudColor cannot be an albedo).
  if (isExplicit(e, "rtx.atmosphere.cloudLayer2Color")) {
    if (!isExplicit(e, "rtx.atmosphere.cloudLayer2Albedo")) {
      auto ratio = [](float tint, float base) {
        return std::min(std::max(tint / std::max(base, 1e-3f), 0.0f), 1.0f);
      };
      s.cloudLayer2Albedo = Vector3(ratio(s.cloudLayer2Color.x, s.cloudColor.x),
                                    ratio(s.cloudLayer2Color.y, s.cloudColor.y),
                                    ratio(s.cloudLayer2Color.z, s.cloudColor.z));
      logMigrated("cloudLayer2Color", "cloudLayer2Albedo");
    }
    s.cloudLayer2Color = defaults.cloudLayer2Color;
  }
  // There is one ground albedo: the clouds' own copy becomes the shared one.
  if (isExplicit(e, "rtx.atmosphere.cloudGroundAlbedo")) {
    if (!isExplicit(e, "rtx.atmosphere.groundAlbedo")) {
      s.groundAlbedo = s.cloudGroundAlbedo;
      logMigrated("cloudGroundAlbedo", "groundAlbedo");
    }
    s.cloudGroundAlbedo = defaults.cloudGroundAlbedo;
  }
  for (const char* name : { "multiScatterStrength", "sunsetSaturation" }) {
    if (isExplicit(e, (std::string("rtx.atmosphere.") + name).c_str())) {
      const std::string msg = std::string("[Deprecated Config] rtx.atmosphere.") + name + " no longer exists and was "
        "ignored: the sky, the haze and the clouds share one physical multiple scattering, and colour grading "
        "belongs in the tonemapper.";
      log(LogLevel::Warn, msg.c_str());
    }
  }
  for (const char* name : { "cloudShadowMarchStrength", "cloudShadowFactorStrength" }) {
    if (isExplicit(e, (std::string("rtx.atmosphere.") + name).c_str())) {
      const std::string msg = std::string("[Deprecated Config] rtx.atmosphere.") + name + " no longer exists and was "
        "ignored: cloud shadows are the transmittance the clouds compute toward the sun (fade them with "
        "rtx.atmosphere.cloudShadowStrength).";
      log(LogLevel::Warn, msg.c_str());
    }
  }
  for (const char* name : { "cloudLayer2StepFloor", "cloudLayer2StepMax" }) {
    if (isExplicit(e, (std::string("rtx.atmosphere.") + name).c_str())) {
      const std::string msg = std::string("[Deprecated Config] rtx.atmosphere.") + name + " no longer exists and was "
        "ignored: cloud layer 2 is marched with the main deck's adaptive step budget.";
      log(LogLevel::Warn, msg.c_str());
    }
  }

  // The two sparse-march switches map onto cloudMarchRate (Auto by default). An explicit switch
  // becomes the fixed rate it asked for; cloudSixteenthMarch takes precedence.
  {
    const bool quarterSet = isExplicit(e, "rtx.atmosphere.cloudQuarterMarch");
    const bool sixteenthSet = isExplicit(e, "rtx.atmosphere.cloudSixteenthMarch");
    if ((quarterSet || sixteenthSet) && !isExplicit(e, "rtx.atmosphere.cloudMarchRate")) {
      if (sixteenthSet && s.cloudSixteenthMarch) {
        s.cloudMarchRate = 3;
        logMigrated("cloudSixteenthMarch", "cloudMarchRate");
      } else if (quarterSet) {
        s.cloudMarchRate = s.cloudQuarterMarch ? 2 : 1;
        logMigrated("cloudQuarterMarch", "cloudMarchRate");
      }
      // cloudSixteenthMarch = False alone asked for nothing but "not sixteenth": Auto stays.
    }
    s.cloudQuarterMarch = defaults.cloudQuarterMarch;
    s.cloudSixteenthMarch = defaults.cloudSixteenthMarch;
  }

  // A game moon's angularRadius key always held the diameter: it maps to angularDiameter unchanged.
  for (uint32_t m = 0; m < kMaxMoons; ++m) {
    const std::string moon = "moon" + std::to_string(m) + ".";
    MoonSettings& ms = s.moons[m];
    if (isExplicit(e, ("rtx.atmosphere." + moon + "angularRadius").c_str())) {
      if (!isExplicit(e, ("rtx.atmosphere." + moon + "angularDiameter").c_str())) {
        ms.angularDiameter = ms.angularRadius;
        logMigrated((moon + "angularRadius").c_str(), (moon + "angularDiameter").c_str());
      }
    }
    ms.angularRadius = defaults.moons[m].angularRadius;
  }

  // Units per centimetre to units per metre, with compression left at 1 so the look is unchanged.
  // When both deprecated scales are set, cloudScale wins: the datums below are converted with it,
  // and in an alphabetically sorted rtx.conf it comes after aerialPerspectiveScale.
  auto scaleToUnitsPerMeter = [&](const char* from, float& src, float srcDefault, bool replacementSet) {
    if (!isExplicit(e, (std::string("rtx.atmosphere.") + from).c_str())) return;
    if (!replacementSet && src > 0.0f) {
      s.unitsPerMeter = src * 100.0f;
      logMigrated(from, "unitsPerMeter");
    }
    src = srcDefault;
  };
  const bool hadApScale = isExplicit(e, "rtx.atmosphere.aerialPerspectiveScale") && s.aerialPerspectiveScale > 0.0f;
  const bool unitsPerMeterSet = isExplicit(e, "rtx.atmosphere.unitsPerMeter");
  const bool cloudScaleSet = isExplicit(e, "rtx.atmosphere.cloudScale") && s.cloudScale > 0.0f;
  scaleToUnitsPerMeter("cloudScale", s.cloudScale, defaults.cloudScale, unitsPerMeterSet);
  scaleToUnitsPerMeter("aerialPerspectiveScale", s.aerialPerspectiveScale, defaults.aerialPerspectiveScale,
                       unitsPerMeterSet || cloudScaleSet);
  if (hadApScale) {
    log(LogLevel::Info, "[Deprecated Config] The clouds and the aerial perspective now share rtx.atmosphere.unitsPerMeter. "
                        "If they were calibrated differently before, check the aerial perspective Range and Near Fade values.");
  }

  // Both datums fold into groundLevelWorldUnits and accumulate. (h - s) * a + v == (h - (s - v/a)) * a,
  // so raising the observer is the same as lowering the ground.
  if (isExplicit(e, "rtx.atmosphere.seaLevelWorldKm") && s.seaLevelWorldKm != 0.0f) {
    s.groundLevelWorldUnits += s.seaLevelWorldKm * legacyCloudUnitsPerKm;
    s.seaLevelWorldKm = defaults.seaLevelWorldKm;
    logMigrated("seaLevelWorldKm", "groundLevelWorldUnits");
  }
  if (isExplicit(e, "rtx.atmosphere.viewAltitudeKm") && s.viewAltitudeKm != 0.0f) {
    s.groundLevelWorldUnits -= s.viewAltitudeKm * legacyCloudUnitsPerKm / std::max(s.altitudeScale, 1e-5f);
    s.viewAltitudeKm = defaults.viewAltitudeKm;
    logMigrated("viewAltitudeKm", "groundLevelWorldUnits");
  }

  // One world scale (docs/DESIGN.md "Units and scale"). The deprecated keys gave three systems their
  // own game units per NUMOS metre (P = the resolved unitsPerMeter):
  //   clouds      (cloudUnitsPerMeter > 0 ? cloudUnitsPerMeter : P) / cloudWorldCompression
  //   haze        P / aerialPerspectiveWorldCompression
  //   near field  aerialPerspectiveFroxelUnitsPerMeter > 0 ? it : P   (fog, lamp glow, froxel range;
  //               or aerialPerspectiveLocalLightUnitsPerMeter for the glow)
  // One figure, P x worldScale, serves all of them. The clouds' figure is kept exactly: worldScale =
  // clouds / P, with unitsPerMeter left as configured. If P is unknown (unitsPerMeter 0 and no
  // legacySceneScale), a set cloudUnitsPerMeter becomes unitsPerMeter itself and worldScale =
  // 1 / cloudWorldCompression, again the clouds' exact figure. An explicit worldScale wins over all
  // of it (the deprecated keys are then only reset). The haze and near field follow the clouds;
  // where their figure differed, a notice names them and the factor.
  {
    const bool cloudCompressionSet = isExplicit(e, "rtx.atmosphere.cloudWorldCompression");
    const bool cloudUnitsSet = isExplicit(e, "rtx.atmosphere.cloudUnitsPerMeter") && s.cloudUnitsPerMeter > 0.0f;
    const bool hazeCompressionSet = isExplicit(e, "rtx.atmosphere.aerialPerspectiveWorldCompression");
    const bool froxelUnitsSet = isExplicit(e, "rtx.atmosphere.aerialPerspectiveFroxelUnitsPerMeter")
                                && s.aerialPerspectiveFroxelUnitsPerMeter > 0.0f;
    const bool glowUnitsSet = isExplicit(e, "rtx.atmosphere.aerialPerspectiveLocalLightUnitsPerMeter")
                              && s.aerialPerspectiveLocalLightUnitsPerMeter > 0.0f;
    const float nearUnits = froxelUnitsSet ? s.aerialPerspectiveFroxelUnitsPerMeter
                          : glowUnitsSet ? s.aerialPerspectiveLocalLightUnitsPerMeter : 0.0f;

    if (cloudCompressionSet || cloudUnitsSet || hazeCompressionSet || nearUnits > 0.0f) {
      float measurement = s.unitsPerMeter > 0.0f ? s.unitsPerMeter
                        : legacySceneScale > 0.0f ? 100.0f * std::max(legacySceneScale, 1e-5f) : 0.0f;
      const float cloudCompression = std::max(s.cloudWorldCompression, 0.1f);
      const bool worldScaleSet = isExplicit(e, "rtx.atmosphere.worldScale");

      // The clouds' scale relative to the measurement.
      float cloudScale = 1.0f / cloudCompression;
      if (cloudUnitsSet) {
        if (measurement > 0.0f) {
          cloudScale = s.cloudUnitsPerMeter / (measurement * cloudCompression);
        } else if (!worldScaleSet) {
          s.unitsPerMeter = s.cloudUnitsPerMeter;
          measurement = s.unitsPerMeter;
          logMigrated("cloudUnitsPerMeter", "unitsPerMeter");
        }
      }
      if (!worldScaleSet) {
        s.worldScale = clampTo(*findSetting("rtx.atmosphere.worldScale"), cloudScale);
        const std::string msg = "[Deprecated Config] rtx.atmosphere.cloudWorldCompression / cloudUnitsPerMeter / "
          "aerialPerspectiveWorldCompression / aerialPerspectiveFroxelUnitsPerMeter have been migrated to "
          "rtx.atmosphere.worldScale = " + formatFloat(s.worldScale) +
          ", which keeps the clouds' scale. Please re-save your config to get rid of this message.";
        log(LogLevel::Info, msg.c_str());
      }

      // What the haze and the near field implied, against the shared scale they follow.
      std::string differ;
      auto compare = [&](const char* what, float impliedScale) {
        if (std::fabs(impliedScale / s.worldScale - 1.0f) > 1e-3f) {
          differ += (differ.empty() ? "" : "; ") + std::string(what) + " was at scale " + formatFloat(impliedScale);
        }
      };
      compare("the haze (aerialPerspectiveWorldCompression)", 1.0f / std::max(s.aerialPerspectiveWorldCompression, 0.1f));
      if (nearUnits > 0.0f && measurement > 0.0f) {
        compare("the fog, lamp glow and froxel range (aerialPerspectiveFroxelUnitsPerMeter)", nearUnits / measurement);
      } else if (nearUnits > 0.0f) {
        differ += (differ.empty() ? "" : "; ") + std::string("the fog, lamp glow and froxel range had their own "
          "units per metre, which cannot be compared without rtx.atmosphere.unitsPerMeter");
      }
      if (worldScaleSet) {
        compare("the clouds (cloudWorldCompression / cloudUnitsPerMeter)", cloudScale);
      }
      if (!differ.empty()) {
        const std::string msg = "[Deprecated Config] The clouds, the haze, the fog and local-light glow now share one "
          "scale, rtx.atmosphere.worldScale = " + formatFloat(s.worldScale) + " (relative to unitsPerMeter). Before, " +
          differ + ". Those now follow the shared scale, so they read differently than before; adjust the fog "
          "settings (or worldScale) if needed.";
        log(LogLevel::Warn, msg.c_str());
      }
    }
    s.cloudWorldCompression = defaults.cloudWorldCompression;
    s.cloudUnitsPerMeter = defaults.cloudUnitsPerMeter;
    s.aerialPerspectiveWorldCompression = defaults.aerialPerspectiveWorldCompression;
    s.aerialPerspectiveFroxelUnitsPerMeter = defaults.aerialPerspectiveFroxelUnitsPerMeter;
    s.aerialPerspectiveLocalLightUnitsPerMeter = defaults.aerialPerspectiveLocalLightUnitsPerMeter;
  }
  // These described an earlier aerial perspective volume and have no counterpart.
  {
    static const char* const kRetired[] = {
      "aerialPerspectiveDepthRangeMeters", "aerialPerspectiveNearFadeStartMeters",
      "aerialPerspectiveNearFadeEndMeters", "aerialPerspectiveLutResolution", "aerialPerspectiveLutDepthSlices",
      "aerialPerspectiveLocalLightShadowRangeMeters", "aerialPerspectiveSeparateVisibility",
      "aerialPerspectiveVisibilityUpdatePeriod", "aerialPerspectiveVisibilityPerSlice",
      "aerialPerspectiveIntegrateScan", "aerialPerspectiveSceneShadowDebug", "aerialPerspectiveSceneShadowRangeMeters",
    };
    std::string retired;
    for (const char* name : kRetired) {
      if (isExplicit(e, (std::string("rtx.atmosphere.") + name).c_str())) {
        retired += retired.empty() ? name : std::string(", ") + name;
      }
    }
    if (!retired.empty()) {
      const std::string msg = "[Deprecated Config] The aerial perspective was rebuilt as a froxel near field plus a far-field "
        "LUT; these settings no longer exist and were ignored: " + retired +
        ". The near field is set with rtx.atmosphere.aerialPerspectiveFroxel*.";
      log(LogLevel::Warn, msg.c_str());
    }
  }

  // Both lightning scales are replaced by one physical source.
  for (const char* name : { "lightningFlashIntensity", "lightningSceneLightIntensity" }) {
    if (isExplicit(e, (std::string("rtx.atmosphere.") + name).c_str())) {
      const std::string msg = std::string("[Deprecated Config] rtx.atmosphere.") + name + " no longer exists and was "
        "ignored: the cloud glow and the scene light now share one physical source, "
        "rtx.atmosphere.lightningLuminousIntensity (candela); rtx.atmosphere.lightningSceneLight turns the scene light off.";
      log(LogLevel::Warn, msg.c_str());
    }
  }

  // Not migratable: a vertical-only multiplier has no equivalent once one unit size covers all axes.
  if (s.altitudeScale != 1.0f) {
    const std::string msg = "[Deprecated Config] rtx.atmosphere.altitudeScale = " + formatFloat(s.altitudeScale) +
      " is no longer supported and has been ignored. It scaled the vertical axis alone; the cloud system now assumes "
      "one unit size for all three axes. Set rtx.atmosphere.unitsPerMeter to the true scale instead.";
    log(LogLevel::Warn, msg.c_str());
  }
}

} // namespace numos
