// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// The settings window: one widget per entry of settingInfos(), grouped by key, with the description
// as tooltip. Widgets write the Settings fields in place (clamped to the setting's range) and mark
// the settings changed; the frame loop then hands them to the renderer.

#include "app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <string_view>

#include "imgui.h"

namespace numos::viewer {

namespace {

constexpr std::string_view kPrefix = "rtx.atmosphere.";

std::string_view shortKey(const SettingInfo& info) {
  std::string_view k = info.key;
  return k.substr(0, kPrefix.size()) == kPrefix ? k.substr(kPrefix.size()) : k;
}

bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

// Groups, in display order. A setting goes to the first group whose rule matches its short key.
const char* const kGroups[] = {
  "World and units", "Sun and time", "Atmosphere", "Clouds", "Cloud layer 2", "Cloud shadows",
  "Cloud quality and history", "Lightning", "Aerial perspective and fog", "Night sky and moon",
  "Moon 0", "Moon 1", "Moon 2", "Moon 3", "Caching and debug",
};

int groupOf(std::string_view k) {
  auto any = [&](std::initializer_list<std::string_view> list) {
    for (std::string_view p : list) {
      if (startsWith(k, p)) return true;
    }
    return false;
  };
  if (startsWith(k, "moon") && k.size() > 5 && k[5] == '.') return 10 + (k[4] - '0');
  if (any({ "unitsPerMeter", "worldScale", "hazeWorldScale", "groundLevel", "groundSmoothing", "cloudVerticalOffset",
            "useCameraWorldOverride", "cameraWorldOverride", "cloudAnchorCut", "seaLevel", "altitudeScale",
            "viewAltitude", "flipUpAxis", "aerialPerspectiveScale", "cloudScale", "cloudWorldCompression",
            "cloudUnitsPerMeter", "aerialPerspectiveWorldCompression" })) return 0;
  if (any({ "sun", "timeCycle", "timeOfDay", "dayLength", "dayOfYear", "calendarYear", "latitude", "northOffset",
            "directionalLight" })) return 1;
  if (any({ "debug", "skyView", "skyLut", "cloudProfiling" })) return 14;
  if (any({ "lightning" })) return 7;
  if (any({ "aerialPerspective", "fog" })) return 8;
  if (any({ "night", "star", "realMoon" })) return 9;
  if (any({ "cloudLayer2" })) return 4;
  if (any({ "cloudShadow", "cloudVoxelShadows", "cloudVoxelGrid", "cloudSkyAmbient" })) return 5;
  if (any({ "cloudTemporal", "cloudHistory", "cloudQuarter", "cloudSixteenth", "cloudSpatial", "cloudMarch",
            "cloudPerformance", "cloudViewSamples", "cloudViewStep", "cloudSecondary", "cloudDetailLod",
            "cloudLightingLod", "nvdfStepScale", "nubis3AdaptiveStep", "nubis3Jitter", "cloudRenderRT" })) return 6;
  if (any({ "cloud", "nvdf", "nubis3" })) return 3;
  return 2;
}

// Colour-like Vector3 settings get a colour picker; the rest (coefficients, illuminance) three drags.
bool isColour(std::string_view k) {
  return k.find("Color") != std::string_view::npos || k.find("Tint") != std::string_view::npos ||
         k.find("Albedo") != std::string_view::npos;
}

bool bounded(float v) { return v != NUMOS_NO_MIN && v != NUMOS_NO_MAX; }

// Settings names a weather snapshot overrides while the weather is on.
const std::set<std::string>& weatherOverridden() {
  static const std::set<std::string> names = [] {
    std::set<std::string> s;
    size_t count = 0;
    const WeatherFieldInfo* fields = weatherFields(count);
    for (size_t i = 0; i < count; ++i) {
      s.insert(fields[i].name);
    }
    s.insert("airTint");
    s.insert("aerosolTint");
    return s;
  }();
  return names;
}

bool differs(const Settings& a, const Settings& b, const SettingInfo& info) {
  const uint8_t* pa = reinterpret_cast<const uint8_t*>(&a) + info.offset;
  const uint8_t* pb = reinterpret_cast<const uint8_t*>(&b) + info.offset;
  switch (info.type) {
    case SettingType::Bool:  return *reinterpret_cast<const bool*>(pa) != *reinterpret_cast<const bool*>(pb);
    case SettingType::Int:   return *reinterpret_cast<const int32_t*>(pa) != *reinterpret_cast<const int32_t*>(pb);
    case SettingType::Uint:  return *reinterpret_cast<const uint32_t*>(pa) != *reinterpret_cast<const uint32_t*>(pb);
    case SettingType::Float: return *reinterpret_cast<const float*>(pa) != *reinterpret_cast<const float*>(pb);
    case SettingType::Vec3:  return *reinterpret_cast<const Vector3*>(pa) != *reinterpret_cast<const Vector3*>(pb);
  }
  return false;
}

void copyValue(Settings& dst, const Settings& src, const SettingInfo& info) {
  const size_t size = info.type == SettingType::Bool ? sizeof(bool)
                    : info.type == SettingType::Vec3 ? sizeof(Vector3) : 4;
  std::memcpy(reinterpret_cast<uint8_t*>(&dst) + info.offset, reinterpret_cast<const uint8_t*>(&src) + info.offset, size);
}

float dragSpeed(float v) { return std::max(std::fabs(v) * 0.005f, 0.001f); }

// The widget for one setting. Returns true when the value changed.
bool settingWidget(Settings& settings, const SettingInfo& info, const char* label) {
  uint8_t* p = reinterpret_cast<uint8_t*>(&settings) + info.offset;
  const bool hasMin = bounded(info.minValue), hasMax = bounded(info.maxValue);
  switch (info.type) {
    case SettingType::Bool:
      return ImGui::Checkbox(label, reinterpret_cast<bool*>(p));
    case SettingType::Int: {
      int32_t& v = *reinterpret_cast<int32_t*>(p);
      const int32_t lo = hasMin ? int32_t(info.minValue) : INT32_MIN / 2;
      const int32_t hi = hasMax ? int32_t(info.maxValue) : INT32_MAX / 2;
      bool c = hasMin && hasMax && hi - lo <= 4096
             ? ImGui::SliderScalar(label, ImGuiDataType_S32, &v, &lo, &hi, nullptr, ImGuiSliderFlags_AlwaysClamp)
             : ImGui::DragScalar(label, ImGuiDataType_S32, &v, 0.2f, &lo, &hi, nullptr, ImGuiSliderFlags_AlwaysClamp);
      return c;
    }
    case SettingType::Uint: {
      uint32_t& v = *reinterpret_cast<uint32_t*>(p);
      const uint32_t lo = hasMin ? uint32_t(std::max(info.minValue, 0.0f)) : 0u;
      const uint32_t hi = hasMax ? uint32_t(info.maxValue) : UINT32_MAX / 2;
      return hasMin && hasMax && hi - lo <= 4096
           ? ImGui::SliderScalar(label, ImGuiDataType_U32, &v, &lo, &hi, nullptr, ImGuiSliderFlags_AlwaysClamp)
           : ImGui::DragScalar(label, ImGuiDataType_U32, &v, 0.2f, &lo, &hi, nullptr, ImGuiSliderFlags_AlwaysClamp);
    }
    case SettingType::Float: {
      float& v = *reinterpret_cast<float*>(p);
      if (hasMin && hasMax) {
        ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
        if (info.minValue > 0.0f && info.maxValue / info.minValue >= 1000.0f) flags |= ImGuiSliderFlags_Logarithmic;
        return ImGui::SliderFloat(label, &v, info.minValue, info.maxValue, "%.5g", flags);
      }
      const float lo = hasMin ? info.minValue : -FLT_MAX;
      const float hi = hasMax ? info.maxValue : FLT_MAX;
      return ImGui::DragFloat(label, &v, dragSpeed(v), lo, hi, "%.5g",
                              hasMin || hasMax ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
    }
    case SettingType::Vec3: {
      Vector3& v = *reinterpret_cast<Vector3*>(p);
      bool c;
      if (isColour(shortKey(info))) {
        c = ImGui::ColorEdit3(label, v.data, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
      } else {
        c = ImGui::DragFloat3(label, v.data, dragSpeed(std::max({ std::fabs(v.x), std::fabs(v.y), std::fabs(v.z) })),
                              0.0f, 0.0f, "%.5g");
      }
      if (c) {
        for (float& x : v.data) {
          if (hasMin) x = std::max(x, info.minValue);
          if (hasMax) x = std::min(x, info.maxValue);
        }
      }
      return c;
    }
  }
  return false;
}

} // namespace

void drawSettingsWindow(AppState& app) {
  static ImGuiTextFilter filter;
  static bool searchDescriptions = false;
  static bool showDeprecated = false;
  static bool changedOnly = false;
  static const Settings kDefaults {};

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - em * 0.5f, vp->WorkPos.y + em * 9.0f),
                          ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(em * 34.0f, std::max(em * 20.0f, vp->WorkSize.y - em * 10.0f)), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Settings")) {
    ImGui::End();
    return;
  }
  filter.Draw("Search", -em * 4.0f);
  ImGui::Checkbox("In descriptions", &searchDescriptions);
  ImGui::SameLine();
  ImGui::Checkbox("Changed only", &changedOnly);
  ImGui::SameLine();
  ImGui::Checkbox("Deprecated", &showDeprecated);
  if (app.useWeather) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "W");
    ImGui::SameLine();
    ImGui::TextWrapped("marks a value the weather overrides while it is on (Weather and lightning panel).");
  }
  ImGui::Separator();

  const std::vector<SettingInfo>& infos = settingInfos();
  const std::set<std::string>& weatherNames = weatherOverridden();
  const bool filtering = filter.IsActive() || changedOnly;
  ImGui::BeginChild("##list");
  for (int g = 0; g < int(std::size(kGroups)); ++g) {
    // Collect the group's visible settings first, so empty groups are skipped.
    std::vector<const SettingInfo*> rows;
    for (const SettingInfo& info : infos) {
      const std::string_view k = shortKey(info);
      if (groupOf(k) != g) continue;
      if (isDeprecated(info) && !showDeprecated) continue;
      if (changedOnly && !differs(app.settings, kDefaults, info)) continue;
      if (filter.IsActive()) {
        const bool hit = filter.PassFilter(info.key.c_str()) ||
                         (searchDescriptions && info.description && filter.PassFilter(info.description));
        if (!hit) continue;
      }
      rows.push_back(&info);
    }
    if (rows.empty()) continue;
    if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    char header[64];
    std::snprintf(header, sizeof(header), "%s (%zu)###%s", kGroups[g], rows.size(), kGroups[g]);
    if (!ImGui::CollapsingHeader(header)) continue;
    if (!ImGui::BeginTable(kGroups[g], 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg)) continue;
    ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    for (const SettingInfo* info : rows) {
      const std::string name(shortKey(*info));
      const bool weather = app.useWeather && weatherNames.count(name) != 0;
      auto describe = [&]() {
        ImGui::SetNextWindowSize(ImVec2(em * 32.0f, 0.0f));
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(info->key.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", info->description ? info->description : "");
        ImGui::TextDisabled("Default: %s", formatSetting(kDefaults, *info).c_str());
        if (isDeprecated(*info)) {
          ImGui::TextDisabled("Deprecated: read from configs and migrated, never saved.");
        }
        if (weather) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "The weather overrides this value while it is on.");
        }
        ImGui::EndTooltip();
      };
      ImGui::PushID(info->key.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool isChanged = differs(app.settings, kDefaults, *info);
      ImGui::BeginDisabled(!isChanged);
      if (ImGui::SmallButton("R")) {
        copyValue(app.settings, kDefaults, *info);
        app.settingsChanged = true;
      }
      ImGui::EndDisabled();
      if (isChanged && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reset to the default, %s", formatSetting(kDefaults, *info).c_str());
      }
      ImGui::TableNextColumn();
      if (weather) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "W");
        ImGui::SameLine();
      }
      if (isChanged) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", name.c_str());
      } else {
        ImGui::TextUnformatted(name.c_str());
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) describe();
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (settingWidget(app.settings, *info, "##value")) {
        app.settingsChanged = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) describe();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
  ImGui::End();
}

} // namespace numos::viewer
