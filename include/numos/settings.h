// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "numos/math.h"

namespace numos {

constexpr float    NUMOS_NO_MIN = -3.402823466e+38f;
constexpr float    NUMOS_NO_MAX = 3.402823466e+38f;
// Deprecated setting: still read from configs (and migrated), never written back.
constexpr uint32_t NUMOS_SETTING_NO_SAVE = 1u << 0;

constexpr uint32_t kMaxMoons = 4;

struct MoonSettings {
#define NUMOS_MOON_SETTING(type, name, def, key, mn, mx, flags, desc) type name = def;
#include "numos/settings_fields.inl"
};

// Every NUMOS setting as a plain value, with its default. The list lives in settings_fields.inl.
struct Settings {
#define NUMOS_SETTING(type, name, def, key, mn, mx, flags, desc) type name = def;
#include "numos/settings_fields.inl"
  MoonSettings moons[kMaxMoons];
};

enum class SettingType : uint8_t { Bool, Int, Uint, Float, Vec3 };

struct SettingInfo {
  std::string key;          // e.g. "rtx.atmosphere.sunSize", "rtx.atmosphere.moon2.phase"
  SettingType type;
  float       minValue;     // NUMOS_NO_MIN when unbounded
  float       maxValue;     // NUMOS_NO_MAX when unbounded
  uint32_t    flags;        // NUMOS_SETTING_*
  const char* description;
  size_t      offset;       // byte offset of the field inside Settings
};

// A deprecated setting: read so old configs load, consumed by migrateDeprecated, never saved.
inline bool isDeprecated(const SettingInfo& info) { return (info.flags & NUMOS_SETTING_NO_SAVE) != 0; }

// Every function here works on caller-owned values and immutable tables: thread-safe for distinct
// Settings objects. Messages go through the log callback (log.h).

// All settings, the four moons expanded. Stable order: scalars as listed, then moon0..moon3.
const std::vector<SettingInfo>& settingInfos();
// nullptr when the key is unknown.
const SettingInfo* findSetting(std::string_view key);

// Sets one value from config syntax ("1.5", "True", "0.1, 0.2, 0.3"). Clamps to the setting's
// range. Returns false (setting unchanged) for an unknown key or unparsable value. Accepted:
// floats in C syntax with an optional '+' and 'f' suffix, finite only (NaN/inf are rejected);
// integers in decimal (no fraction; a negative value for an unsigned setting is rejected);
// bools as true/false in any case, or 1/0; Vector3 as three comma-separated floats, each clamped.
// The parse is locale-independent.
bool setSetting(Settings& settings, std::string_view key, std::string_view value);
// The value in config syntax.
std::string formatSetting(const Settings& settings, const SettingInfo& info);

// Which settings a config explicitly set, indexed like settingInfos().
using ExplicitSettings = std::vector<bool>;

// Applies "key = value" lines (Remix rtx.conf syntax; '#' comments). Keys that are not NUMOS
// settings are ignored, so a whole rtx.conf can be passed. Marks each applied key in `explicitlySet`
// (resized as needed). Returns the number of values applied.
// LF or CRLF line ends, a leading UTF-8 BOM and surrounding whitespace are fine; '#' starts a comment
// anywhere on a line. A key given twice: the last line wins. A NUMOS key whose value does not parse
// is skipped with a warning through the log callback.
size_t applyConfig(Settings& settings, std::string_view text, ExplicitSettings* explicitlySet = nullptr);

// Writes every non-default, saveable setting as config lines (floats in their shortest exact form,
// locale-independent), in settingInfos() order.
std::string writeConfig(const Settings& settings);

// Moves values from deprecated settings into their replacements. `explicitlySet` tells which
// replacements a config already set (those are kept). `legacySceneScale` is the host's world-scale
// value (Remix: rtx.sceneScale), used to convert the deprecated km-denominated datums and, when
// unitsPerMeter is 0 (inherit), as the measurement the deprecated per-system unit scales are
// expressed against (100 x it, the host's units per metre); pass 0 when the host has none.
// Call it once per load, after every applyConfig of that load, with the same `explicitlySet`. It is
// not idempotent: a second call with the same flags migrates the (now reset) deprecated defaults
// again, overwriting what the first call migrated. On a reload start from fresh Settings and
// ExplicitSettings.
// "Explicitly set" means present in a config, never "differs from its default": a replacement set to
// its default value (rtx.atmosphere.worldScale = 1) is a choice and must win over the deprecated keys.
// A host whose settings persist across loads (Remix's option layers) should run this once per load
// on what its configs hold, write the changed replacements back into its own settings and remove the
// deprecated keys it consumed (isDeprecated and explicitly set), so a save drops them and the
// migration never runs on its own output; afterwards the result is a fixed point (no deprecated key
// set: nothing to migrate).
void migrateDeprecated(Settings& settings, const ExplicitSettings& explicitlySet, float legacySceneScale);

} // namespace numos
