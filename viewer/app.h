// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// The interactive viewer (numos_viewer --interactive): a window with the viewer's composite, a free
// camera and a UI for every setting. Built only with NUMOS_BUILD_VIEWER_UI.
//
//   app_main.cpp          the frame loop: renderer, viewer passes, blit to the swapchain, UI
//   app_window.cpp        GLFW window, Vulkan instance, device and swapchain
//   app_camera.cpp        the free camera and its keyboard and mouse controls
//   app_ui.cpp            scene, weather, camera, config, seed and stats panels
//   app_settings_ui.cpp   the settings window generated from settingInfos()

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "numos/renderer.h"
#include "numos/settings.h"
#include "numos/weather.h"
#include "numos_vulkan.h"
#include "viewer_shared.h"

struct GLFWwindow;

namespace numos::viewer {

int runInteractive(const Options& opt);

// ---- window and Vulkan (app_window.cpp) ----

struct VulkanContext {
  GLFWwindow*              window = nullptr;
  VkInstance               instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  VkSurfaceKHR             surface = VK_NULL_HANDLE;
  VkPhysicalDevice         physicalDevice = VK_NULL_HANDLE;
  VkDevice                 device = VK_NULL_HANDLE;
  uint32_t                 queueFamily = 0;
  VkQueue                  queue = VK_NULL_HANDLE;
  std::string              deviceName;

  VkSwapchainKHR           swapchain = VK_NULL_HANDLE;
  VkFormat                 format = VK_FORMAT_UNDEFINED;
  VkExtent2D               extent {};
  uint32_t                 minImageCount = 2;
  VkPresentModeKHR         presentMode = VK_PRESENT_MODE_FIFO_KHR;
  std::vector<VkImage>     images;
  std::vector<VkImageView> views;
  std::vector<VkSemaphore> renderDone;  // one per swapchain image, signalled by the frame's submit
};

// Opens the window and creates the instance, surface and device (graphics, compute and present on
// one queue; dynamic rendering for the UI).
void createContext(VulkanContext& vk, const Options& opt);
// (Re)creates the swapchain at the window's framebuffer size. False while the window has no area
// (minimised).
bool createSwapchain(VulkanContext& vk, bool vsync);
void destroySwapchain(VulkanContext& vk);
void destroyContext(VulkanContext& vk);

// ---- free camera (app_camera.cpp) ----

struct FreeCamera {
  Vector3 position = Vector3(0.0f, 2.0f, 0.0f);  // metres, Y up; the ground plane is at 0
  float   yawDeg = 135.0f;    // 0 looks along +Z, 90 along +X
  float   pitchDeg = 8.0f;
  float   fovYDeg = 70.0f;
  float   speed = 10.0f;      // m/s before the Shift and Ctrl factors
  float   lookDegPerPixel = 0.12f;
  bool    mouseLook = false;  // the cursor is captured and turns the camera
  bool    lookToggled = false;  // captured by the toggle key rather than a held right button

  Vector3 forward() const;
  Vector3 right() const;
};

struct CameraInput {
  bool  keyboardFree = true;  // the UI is not taking keyboard input
  bool  mouseFree = true;     // the cursor is not over the UI
  float wheel = 0.0f;         // scroll this frame
};

// Applies this frame's keys and mouse. Returns true when the camera moved or turned.
bool updateCamera(FreeCamera& cam, GLFWwindow* window, float dt, const CameraInput& input);
void setMouseLook(FreeCamera& cam, GLFWwindow* window, bool on, bool toggled);

// ---- application state shared by the loop and the UI ----

struct AppState {
  Options  opt;
  Settings settings;
  bool     settingsChanged = false;  // set by the UI; the loop hands the settings to the renderer

  // Weather. Off: the settings alone describe the sky.
  bool            useWeather = false;
  WeatherBlender  blender;
  int             weatherPreset = int(WeatherPreset::PartlyCloudy);
  float           weatherSeconds = 10.0f;

  // Clocks.
  bool  paused = false;
  float timeScale = 1.0f;      // multiplies the frame time the renderer's clocks advance by
  float sunSpeedDegPerSec = 0.0f;  // moves the sun elevation while the time cycle is off

  // View.
  float    exposure = 0.12f;
  bool     ground = true;
  Vector3  groundAlbedo = Vector3(0.18f, 0.17f, 0.15f);
  bool     secondarySky = false;
  int      skyAmbientCheck = 0;
  bool     accumulate = false;
  uint32_t accumFrames = 0;     // frames averaged so far
  uint32_t accumLimit = 1024;
  bool     vsync = true;
  bool     swapchainDirty = false;
  float    renderScale = 1.0f;

  FreeCamera camera;
  bool       cameraCut = false;  // the next frame discards temporal history

  // Requests from the UI and keys, consumed by the loop.
  bool lightningRequest = false;
  bool screenshotRequest = false;
  bool reloadNightSky = false;

  // UI.
  bool        showUi = true;
  bool        showStats = true;
  bool        gpuTimings = false;
  std::string confPath;
  std::string nightSkyDir;
  std::string nightSkyStatus;
  std::string screenshotDir = ".";
  std::string status;  // last message shown in the main panel
  std::string changedText;  // "print changed settings" output
  bool        showChanged = false;

  // Frame results for the UI.
  uint64_t frameIndex = 0;
  uint32_t renderWidth = 0, renderHeight = 0;
  float    frameMs = 0.0f;  // smoothed CPU frame time
  std::vector<vulkan::VulkanGpu::ZoneTiming> timings;
  float    sunElevationDeg = 0.0f, sunAzimuthDeg = 0.0f;  // in force (time cycle included)
  float    timeOfDayHours = 12.0f;  // the running clock
  float    radianceScale = 1.0f;
  float    groundLevel = 0.0f;
  // Layer 1 cloud base and top in world metres, from the last frame's arguments.
  float    cloudBaseY = 0.0f, cloudTopY = 0.0f;
  bool     cloudHeightsValid = false;
};

void setStatus(AppState& app, const std::string& message);

// ---- UI (app_ui.cpp, app_settings_ui.cpp) ----

void drawMainPanel(AppState& app, Renderer& renderer);
void drawStatsOverlay(AppState& app);
void drawSettingsWindow(AppState& app);

// Config files: Load replaces every setting with the file's (defaults for the rest); Save writes the
// changed settings into the file, keeping its other lines (non-NUMOS rtx.conf options).
bool loadConfigFile(AppState& app, const std::string& path);
bool saveConfigFile(AppState& app, const std::string& path);

} // namespace numos::viewer
