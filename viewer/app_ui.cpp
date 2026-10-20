// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// The main panel (view, camera, sun and time, weather, night sky, config and seed) and the stats
// overlay.

#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "imgui.h"
#include "numos/seed.h"

namespace numos::viewer {

namespace {

// ImGui text input on a std::string, with a fixed-size buffer.
bool inputString(const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
  char buffer[1024];
  std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
  if (ImGui::InputText(label, buffer, sizeof(buffer), flags)) {
    value = buffer;
    return true;
  }
  return false;
}

void tooltip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f));
    ImGui::BeginTooltip();
    ImGui::TextWrapped("%s", text);
    ImGui::EndTooltip();
  }
}

std::string formatClock(float hours) {
  const int minutes = int(std::floor(std::fmod(std::fmod(hours, 24.0f) + 24.0f, 24.0f) * 60.0f));
  char text[16];
  std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
  return text;
}

void changed(AppState& app) { app.settingsChanged = true; }

void jumpTo(AppState& app, float y) {
  app.camera.position.y = y;
  app.cameraCut = true;
}

void drawViewSection(AppState& app) {
  float ev = std::log2(app.exposure);
  if (ImGui::SliderFloat("Exposure (EV)", &ev, -16.0f, 8.0f, "%.2f")) {
    app.exposure = std::exp2(ev);
  }
  tooltip("Physical exposure as a power of two. The renderer's pre-exposure (radianceScale) is divided out, "
          "so night needs a much higher value than day.");
  ImGui::SameLine();
  if (ImGui::SmallButton("Day")) app.exposure = 0.12f;
  ImGui::SameLine();
  if (ImGui::SmallButton("Night")) app.exposure = 0.12f * 4096.0f;
  if (ImGui::Checkbox("Vsync", &app.vsync)) app.swapchainDirty = true;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
  ImGui::SliderFloat("Render scale", &app.renderScale, 0.25f, 2.0f, "%.2f");
  tooltip("The render resolution as a fraction of the window. The composite is scaled to the window with a linear filter.");
  ImGui::Checkbox("Ground plane", &app.ground);
  ImGui::SameLine();
  ImGui::ColorEdit3("Ground albedo", &app.groundAlbedo.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
  ImGui::Checkbox("Sky through the secondary-ray path", &app.secondarySky);
  tooltip("Draws the sky as reflections see it: the cloud dome LUT instead of the screen-space clouds.");
  ImGui::Checkbox("Accumulate", &app.accumulate);
  tooltip("Averages frames while the camera, the settings and the weather stay still. Pause the clocks to let "
          "moving clouds converge.");
  if (app.accumulate) {
    ImGui::SameLine();
    ImGui::Text("%u / %u frames", app.accumFrames, app.accumLimit);
  }
  const char* checks[] = { "Off", "1", "2", "3" };
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
  ImGui::Combo("Sky ambient check", &app.skyAmbientCheck, checks, 4);
  tooltip("Diagnostic outputs of the composite (viewer_composite.comp.slang, --sky-ambient-check).");
  ImGui::Text("Screenshots (F12) go to");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0f);
  inputString("##shotdir", app.screenshotDir);
}

void drawCameraSection(AppState& app) {
  FreeCamera& cam = app.camera;
  ImGui::Text("Altitude %.1f m above the ground plane, heading %.1f deg", cam.position.y - app.groundLevel, cam.yawDeg);
  ImGui::DragFloat3("Position (m)", &cam.position.x, std::max(1.0f, cam.speed * 0.05f), 0.0f, 0.0f, "%.1f");
  ImGui::DragFloat("Yaw", &cam.yawDeg, 0.5f, 0.0f, 360.0f, "%.1f deg", ImGuiSliderFlags_WrapAround);
  ImGui::DragFloat("Pitch", &cam.pitchDeg, 0.5f, -89.0f, 89.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
  ImGui::SliderFloat("Field of view", &cam.fovYDeg, 10.0f, 120.0f, "%.0f deg (vertical)");
  ImGui::SliderFloat("Speed", &cam.speed, 0.1f, 100000.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
  tooltip("Mouse wheel changes it. Shift: x10, Ctrl: x0.1.");
  ImGui::SliderFloat("Look sensitivity", &cam.lookDegPerPixel, 0.01f, 0.5f, "%.2f deg/px");
  if (ImGui::Button("Reset camera")) {
    cam.position = Vector3(0.0f, app.opt.eyeHeight, 0.0f);
    cam.yawDeg = app.opt.yaw;
    cam.pitchDeg = app.opt.pitch;
    cam.fovYDeg = app.opt.fovY;
    app.cameraCut = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Ground")) jumpTo(app, app.groundLevel + app.opt.eyeHeight);
  ImGui::BeginDisabled(!app.cloudHeightsValid);
  ImGui::SameLine();
  if (ImGui::Button("Cloud base")) jumpTo(app, app.cloudBaseY - 50.0f);
  ImGui::SameLine();
  if (ImGui::Button("In clouds")) jumpTo(app, 0.5f * (app.cloudBaseY + app.cloudTopY));
  ImGui::SameLine();
  if (ImGui::Button("Above clouds")) jumpTo(app, app.cloudTopY + 500.0f);
  ImGui::EndDisabled();
  if (app.cloudHeightsValid) {
    ImGui::TextDisabled("Cloud layer: %.0f to %.0f m", app.cloudBaseY, app.cloudTopY);
  }
  ImGui::TextDisabled("WASD move, Q/E down/up, right mouse button look, M toggles look, Esc releases the mouse");
}

void drawSunSection(AppState& app, Renderer& renderer) {
  Settings& s = app.settings;
  ImGui::Text("Sun in force: elevation %.2f deg, azimuth %.1f deg", app.sunElevationDeg, app.sunAzimuthDeg);
  ImGui::Text("Clock %s (day %d, %d)", formatClock(app.timeOfDayHours).c_str(), s.dayOfYear, s.calendarYear);
  if (ImGui::Checkbox("Time cycle", &s.timeCycleEnable)) changed(app);
  tooltip(findSetting("rtx.atmosphere.timeCycleEnable")->description);
  if (!s.timeCycleEnable) {
    if (ImGui::SliderFloat("Sun elevation", &s.sunElevation, -90.0f, 90.0f, "%.2f deg")) changed(app);
    if (ImGui::SliderFloat("Sun azimuth", &s.sunRotation, 0.0f, 360.0f, "%.1f deg")) changed(app);
    ImGui::SliderFloat("Sun speed", &app.sunSpeedDegPerSec, -10.0f, 10.0f, "%.2f deg/s");
    tooltip("Moves the sun elevation over time, over the zenith and below the horizon (--sun-speed).");
  } else {
    if (ImGui::SliderFloat("Day length", &s.dayLengthMinutes, 0.1f, 1440.0f, "%.1f min", ImGuiSliderFlags_Logarithmic)) changed(app);
    tooltip(findSetting("rtx.atmosphere.dayLengthMinutes")->description);
  }
  float hours = app.timeOfDayHours;
  if (ImGui::SliderFloat("Time of day", &hours, 0.0f, 24.0f, "%.2f h")) {
    s.timeOfDayHours = hours;
    changed(app);
  }
  tooltip(findSetting("rtx.atmosphere.timeOfDayHours")->description);
  if (ImGui::SliderInt("Day of year", &s.dayOfYear, 1, 365)) changed(app);
  if (ImGui::InputInt("Year", &s.calendarYear)) changed(app);
  if (ImGui::SliderFloat("Latitude", &s.latitudeDegrees, -90.0f, 90.0f, "%.2f deg")) changed(app);
  if (ImGui::SliderFloat("North offset", &s.northOffsetDegrees, -180.0f, 180.0f, "%.1f deg")) changed(app);
  ImGui::Separator();
  ImGui::Checkbox("Pause clocks", &app.paused);
  tooltip("Stops every clock the renderer runs: wind, cloud evolution, the time cycle, lightning and the weather blend.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
  ImGui::SliderFloat("Time scale", &app.timeScale, 0.0f, 100.0f, "x%.2f", ImGuiSliderFlags_Logarithmic);
  if (ImGui::Button("Reset clocks")) {
    SimulationState sim;
    sim.timeOfDayHours = s.timeOfDayHours;
    renderer.setSimulationState(sim);
    app.cameraCut = true;
  }
  tooltip("Restarts the cloud drift and evolution and the animation clock, and sets the clock to Time of Day.");
}

void drawWeatherSection(AppState& app) {
  if (ImGui::Checkbox("Use weather", &app.useWeather) && app.useWeather) {
    app.blender = WeatherBlender(weatherFromSettings(app.settings));
  }
  tooltip("Off: the settings alone describe the sky. On: the weather below overrides the settings it covers "
          "(marked W in the settings window).");
  ImGui::BeginDisabled(!app.useWeather);
  const char* current = weatherPresetName(WeatherPreset(app.weatherPreset));
  if (ImGui::BeginCombo("Preset", current)) {
    for (int i = 0; i < int(WeatherPreset::Count); ++i) {
      if (ImGui::Selectable(weatherPresetName(WeatherPreset(i)), i == app.weatherPreset)) {
        app.weatherPreset = i;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SliderFloat("Transition", &app.weatherSeconds, 0.0f, 120.0f, "%.1f s");
  if (ImGui::Button("Go")) {
    app.blender.setTarget(weatherPreset(WeatherPreset(app.weatherPreset), app.settings), app.weatherSeconds);
  }
  ImGui::SameLine();
  if (ImGui::Button("Jump")) {
    app.blender.setTarget(weatherPreset(WeatherPreset(app.weatherPreset), app.settings), 0.0f);
  }
  ImGui::SameLine();
  if (ImGui::Button("From settings")) {
    app.blender.setTarget(weatherFromSettings(app.settings), app.weatherSeconds);
  }
  tooltip("Blends to the weather the current settings describe.");
  if (app.blender.transitioning()) {
    ImGui::SameLine();
    ImGui::TextUnformatted("blending...");
  }
  if (ImGui::TreeNode("Current weather")) {
    WeatherSnapshot w = app.blender.current();
    bool edited = false;
    size_t count = 0;
    const WeatherFieldInfo* fields = weatherFields(count);
    for (size_t i = 0; i < count; ++i) {
      float& v = w.*(fields[i].member);
      if (fields[i].kind == WeatherFieldKind::Flag) {
        bool on = v >= 0.5f;
        if (ImGui::Checkbox(fields[i].label, &on)) {
          v = on ? 1.0f : 0.0f;
          edited = true;
        }
      } else {
        edited |= ImGui::DragFloat(fields[i].label, &v, std::max(std::fabs(v) * 0.005f, 0.001f), 0.0f, 0.0f, "%.4g");
      }
    }
    if (edited) {
      app.blender.setTarget(w, 0.0f);
    }
    ImGui::TreePop();
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  Settings& s = app.settings;
  if (ImGui::Checkbox("Lightning", &s.lightningEnable)) changed(app);
  ImGui::SameLine();
  ImGui::BeginDisabled(!s.lightningEnable);
  if (ImGui::Button("Strike now")) app.lightningRequest = true;
  ImGui::EndDisabled();
  if (ImGui::SliderFloat("Strikes per minute", &s.lightningStrikesPerMinute, 0.0f, 60.0f, "%.1f")) changed(app);
  tooltip("Weather overrides this while it is on.");
}

void drawNightSkySection(AppState& app) {
  Settings& s = app.settings;
  if (ImGui::Checkbox("Stars and Milky Way", &s.nightSkyStars)) changed(app);
  ImGui::SameLine();
  if (ImGui::Checkbox("Real moon", &s.realMoonEnabled)) changed(app);
  if (ImGui::SliderFloat("Night gain", &s.nightGainStops, 0.0f, 12.0f, "%.1f stops")) changed(app);
  tooltip(findSetting("rtx.atmosphere.nightGainStops")->description);
  ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 5.0f);
  inputString("##nightdir", app.nightSkyDir);
  ImGui::SameLine();
  if (ImGui::Button("Reload")) app.reloadNightSky = true;
  ImGui::TextDisabled("%s", app.nightSkyStatus.c_str());
}

void drawConfigSection(AppState& app, Renderer& renderer) {
  ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 8.0f);
  inputString("##conf", app.confPath);
  ImGui::SameLine();
  if (ImGui::Button("Load")) loadConfigFile(app, app.confPath);
  ImGui::SameLine();
  if (ImGui::Button("Save")) saveConfigFile(app, app.confPath);
  tooltip("Save writes every changed NUMOS setting into the file and keeps its other lines, so a full rtx.conf "
          "keeps its non-NUMOS options.");
  if (ImGui::Button("Print changed settings")) {
    app.changedText = writeConfig(app.settings);
    std::printf("---- changed NUMOS settings ----\n%s--------------------------------\n", app.changedText.c_str());
    app.showChanged = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset all settings")) {
    app.settings = Settings {};
    changed(app);
    app.cameraCut = true;
    setStatus(app, "every setting reset to its default");
  }

  ImGui::Separator();
  if (ImGui::Button("Copy seed")) {
    const SimulationState sim = renderer.simulationState();
    const std::string seed = encodeSeed(app.settings, app.useWeather ? &app.blender.current() : nullptr, &sim);
    ImGui::SetClipboardText(seed.c_str());
    setStatus(app, "seed copied (" + std::to_string(seed.size()) + " characters): " + seed);
  }
  tooltip("The settings, the weather (when on) and the clocks as one string.");
  ImGui::SameLine();
  if (ImGui::Button("Paste seed")) {
    const char* text = ImGui::GetClipboardText();
    std::string seedText = text ? text : "";
    seedText.erase(0, seedText.find_first_not_of(" \t\r\n"));
    seedText.erase(seedText.find_last_not_of(" \t\r\n") + 1);
    SeedContents seed;
    std::string error;
    if (decodeSeed(seedText, seed, &error)) {
      app.settings = seed.settings;
      if (seed.hasWeather) {
        app.useWeather = true;
        app.blender = WeatherBlender(seed.weather);
      }
      if (seed.hasSimulation) {
        renderer.setSimulationState(seed.simulation);
      }
      changed(app);
      app.cameraCut = true;
      setStatus(app, std::string("seed applied: settings") + (seed.hasWeather ? ", weather" : "") +
                (seed.hasSimulation ? ", clocks" : "") + (seed.unknownEntries ? ", " + std::to_string(seed.unknownEntries) + " unknown entries skipped" : ""));
    } else {
      setStatus(app, "the clipboard holds no valid seed: " + error);
    }
  }
}

} // namespace

bool loadConfigFile(AppState& app, const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    setStatus(app, "cannot open " + path);
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  Settings settings;
  ExplicitSettings explicitlySet;
  const size_t n = applyConfig(settings, ss.str(), &explicitlySet);
  migrateDeprecated(settings, explicitlySet, 0.0f);
  app.settings = settings;
  app.settingsChanged = true;
  app.cameraCut = true;
  setStatus(app, "loaded " + std::to_string(n) + " NUMOS settings from " + path);
  return true;
}

bool saveConfigFile(AppState& app, const std::string& path) {
  // Keep every line of an existing file that does not set a NUMOS key.
  std::string kept;
  {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (f && std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const size_t eq = line.find('=');
      const size_t hash = line.find('#');
      if (eq != std::string::npos && (hash == std::string::npos || hash > eq)) {
        std::string key = line.substr(0, eq);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (findSetting(key)) {
          continue;
        }
      }
      kept += line + "\n";
    }
  }
  const std::string numos = writeConfig(app.settings);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    setStatus(app, "cannot write " + path);
    return false;
  }
  std::string text = kept;
  if (!text.empty() && text.size() >= 2 && text.compare(text.size() - 2, 2, "\n\n") != 0) {
    text += "\n";
  }
  text += numos;
  // rtx.conf files are CRLF on Windows.
  std::string crlf;
  for (char c : text) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  out << crlf;
  setStatus(app, "saved the changed NUMOS settings to " + path);
  return true;
}

void drawMainPanel(AppState& app, Renderer& renderer) {
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImVec2(em * 0.5f, em * 0.5f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(em * 30.0f, em * 44.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("NUMOS viewer")) {
    if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) drawViewSection(app);
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) drawCameraSection(app);
    if (ImGui::CollapsingHeader("Sun and time", ImGuiTreeNodeFlags_DefaultOpen)) drawSunSection(app, renderer);
    if (ImGui::CollapsingHeader("Weather and lightning")) drawWeatherSection(app);
    if (ImGui::CollapsingHeader("Night sky")) drawNightSkySection(app);
    if (ImGui::CollapsingHeader("Config and seed", ImGuiTreeNodeFlags_DefaultOpen)) drawConfigSection(app, renderer);
    ImGui::Checkbox("Stats", &app.showStats);
    ImGui::SameLine();
    ImGui::Checkbox("GPU timings", &app.gpuTimings);
    tooltip("Reads the GPU timestamps twice a second, which waits for that frame to finish.");
    if (!app.status.empty()) {
      ImGui::Separator();
      ImGui::TextWrapped("%s", app.status.c_str());
    }
    ImGui::TextDisabled("F1 hides the UI, F12 saves a screenshot");
  }
  ImGui::End();

  if (app.showChanged) {
    ImGui::SetNextWindowSize(ImVec2(em * 36.0f, em * 20.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Changed settings", &app.showChanged)) {
      if (ImGui::Button("Copy")) ImGui::SetClipboardText(app.changedText.c_str());
      ImGui::SameLine();
      if (ImGui::Button("Refresh")) app.changedText = writeConfig(app.settings);
      ImGui::InputTextMultiline("##changed", app.changedText.data(), app.changedText.size() + 1,
                                ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::End();
  }
}

void drawStatsOverlay(AppState& app) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - em * 0.5f, vp->WorkPos.y + em * 0.5f),
                          ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.6f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##stats", nullptr, flags)) {
    ImGui::Text("%.1f fps  %.2f ms", app.frameMs > 0.0f ? 1000.0f / app.frameMs : 0.0f, app.frameMs);
    ImGui::Text("%ux%u  frame %llu", app.renderWidth, app.renderHeight, static_cast<unsigned long long>(app.frameIndex));
    ImGui::Text("alt %.1f m  heading %.0f  %.1f m/s", app.camera.position.y - app.groundLevel, app.camera.yawDeg,
                app.camera.speed);
    ImGui::Text("sun %.2f / %.1f deg  %s", app.sunElevationDeg, app.sunAzimuthDeg, formatClock(app.timeOfDayHours).c_str());
    if (app.radianceScale != 1.0f) {
      ImGui::Text("radiance scale %g", app.radianceScale);
    }
    if (app.paused) {
      ImGui::TextUnformatted("clocks paused");
    }
    if (app.gpuTimings && !app.timings.empty()) {
      ImGui::Separator();
      double total = 0.0;
      for (const auto& z : app.timings) {
        if (z.depth == 0) total += z.ms;
      }
      ImGui::Text("GPU %.3f ms (NUMOS zones)", total);
      for (const auto& z : app.timings) {
        ImGui::Text("%*s%-40s %7.3f", int(z.depth * 2), "", z.name.c_str(), z.ms);
      }
    }
  }
  ImGui::End();
}

} // namespace numos::viewer
