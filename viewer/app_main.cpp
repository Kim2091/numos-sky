// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// The interactive viewer's frame loop. Each frame: UI and camera input, the renderer and the
// viewer's depth and composite passes at the render resolution (the same passes the headless run
// records), a blit of the composite to the swapchain image, then the UI on top.

#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>

#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "numos/log.h"

namespace numos::viewer {

namespace {

constexpr uint32_t kFramesInFlight = 2;

float s_wheel = 0.0f;  // scroll since the last frame

void onScroll(GLFWwindow*, double, double y) { s_wheel += float(y); }

void onImGuiVkResult(VkResult r) {
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "[imgui vulkan] VkResult %d\n", int(r));
  }
}

uint32_t findHostMemory(VkPhysicalDevice pd, uint32_t typeBits) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
      return i;
    }
  }
  throw std::runtime_error("no host-visible memory type");
}

// Copies the composite (RGBA8, render resolution) to the host and writes it as a PPM. Waits for the
// GPU; for screenshots only.
void writeScreenshot(VulkanContext& vk, VkCommandPool pool, vulkan::VulkanGpu& gpu, GpuHandle output,
                     uint32_t width, uint32_t height, const std::string& path) {
  check(vkDeviceWaitIdle(vk.device), "vkDeviceWaitIdle");
  VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  bci.size = VkDeviceSize(width) * height * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer;
  check(vkCreateBuffer(vk.device, &bci, nullptr, &buffer), "vkCreateBuffer");
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(vk.device, buffer, &req);
  VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = findHostMemory(vk.physicalDevice, req.memoryTypeBits);
  VkDeviceMemory memory;
  check(vkAllocateMemory(vk.device, &mai, nullptr, &memory), "vkAllocateMemory");
  check(vkBindBufferMemory(vk.device, buffer, memory, 0), "vkBindBufferMemory");

  VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  check(vkAllocateCommandBuffers(vk.device, &cai, &cmd), "vkAllocateCommandBuffers");
  VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
  VkBufferImageCopy copy {};
  copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  copy.imageExtent = { width, height, 1 };
  vkCmdCopyImageToBuffer(cmd, gpu.image(output), VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
  check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
  VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  check(vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
  check(vkQueueWaitIdle(vk.queue), "vkQueueWaitIdle");
  void* mapped;
  check(vkMapMemory(vk.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
  writePpm(path, width, height, static_cast<const uint8_t*>(mapped));
  vkUnmapMemory(vk.device, memory);
  vkFreeCommandBuffers(vk.device, pool, 1, &cmd);
  vkDestroyBuffer(vk.device, buffer, nullptr);
  vkFreeMemory(vk.device, memory, nullptr);
}

std::string screenshotName(const std::string& dir) {
  const std::time_t now = std::time(nullptr);
  std::tm tm {};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char name[64];
  std::strftime(name, sizeof(name), "numos_%Y%m%d_%H%M%S.ppm", &tm);
  return (dir.empty() ? std::string(".") : dir) + "/" + name;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
  VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void initImGui(VulkanContext& vk) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // window layout is not saved between runs
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
  style.WindowRounding = 4.0f * scale;
  style.Colors[ImGuiCol_WindowBg].w = 0.92f;

  ImGui_ImplGlfw_InitForVulkan(vk.window, true);
  ImGui_ImplVulkan_InitInfo info {};
  info.ApiVersion = VK_API_VERSION_1_3;
  info.Instance = vk.instance;
  info.PhysicalDevice = vk.physicalDevice;
  info.Device = vk.device;
  info.QueueFamily = vk.queueFamily;
  info.Queue = vk.queue;
  info.DescriptorPoolSize = 64;
  info.MinImageCount = vk.minImageCount;
  info.ImageCount = uint32_t(vk.images.size());
  info.UseDynamicRendering = true;
  info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  info.PipelineInfoMain.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
  info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &vk.format;
  info.CheckVkResultFn = onImGuiVkResult;
  if (!ImGui_ImplVulkan_Init(&info)) {
    throw std::runtime_error("ImGui_ImplVulkan_Init failed");
  }
}

// Waits for a fence while keeping the window's events flowing, so Windows does not mark the window
// as not responding while the GPU finishes a long frame.
void waitFence(VkDevice device, VkFence fence) {
  for (;;) {
    const VkResult r = vkWaitForFences(device, 1, &fence, VK_TRUE, 100'000'000ull);
    if (r != VK_TIMEOUT) {
      check(r, "vkWaitForFences");
      return;
    }
    glfwPollEvents();
  }
}

// Command buffers and synchronisation of the frames in flight, shared by the loading screen and the
// frame loop. Each slot's fence is signalled when its last submit completes.
struct FrameSync {
  VkCommandBuffer cmds[kFramesInFlight] = {};
  VkFence fences[kFramesInFlight] = {};
  VkSemaphore imageReady[kFramesInFlight] = {};
};

// Presents a frame of UI only: "Loading NUMOS..." with `line` and, for `fraction` >= 0, a progress
// bar. Startup compiles the cloud passes, which takes minutes when the driver has no cached copy
// (after a shader change); presenting these frames meanwhile keeps the window responsive.
// `presents` counts the frames presented here and picks the slot.
void presentLoadingFrame(VulkanContext& vk, AppState& app, FrameSync& sync, uint64_t& presents,
                         const std::string& line, float fraction) {
  glfwPollEvents();
  int fbWidth = 0, fbHeight = 0;
  glfwGetFramebufferSize(vk.window, &fbWidth, &fbHeight);
  if (fbWidth <= 0 || fbHeight <= 0) {
    return;  // minimised
  }
  if (app.swapchainDirty || uint32_t(fbWidth) != vk.extent.width || uint32_t(fbHeight) != vk.extent.height) {
    if (!createSwapchain(vk, app.vsync)) {
      return;
    }
    ImGui_ImplVulkan_SetMinImageCount(vk.minImageCount);
    app.swapchainDirty = false;
  }

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
  ImGui::TextUnformatted("Loading NUMOS...");
  ImGui::TextUnformatted(line.c_str());
  if (fraction >= 0.0f) {
    ImGui::ProgressBar(fraction, ImVec2(ImGui::GetFontSize() * 24.0f, 0.0f));
  }
  ImGui::End();
  ImGui::Render();

  const uint32_t slot = uint32_t(presents % kFramesInFlight);
  waitFence(vk.device, sync.fences[slot]);
  uint32_t imageIndex = 0;
  const VkResult acquired = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, sync.imageReady[slot],
                                                  VK_NULL_HANDLE, &imageIndex);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    app.swapchainDirty = true;
    return;
  }
  if (acquired == VK_SUBOPTIMAL_KHR) {
    app.swapchainDirty = true;
  } else {
    check(acquired, "vkAcquireNextImageKHR");
  }
  ++presents;
  check(vkResetFences(vk.device, 1, &sync.fences[slot]), "vkResetFences");
  VkCommandBuffer cmd = sync.cmds[slot];
  check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
  VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
  const VkImage swapImage = vk.images[imageIndex];
  imageBarrier(cmd, swapImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo color { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
  color.imageView = vk.views[imageIndex];
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = { { 0.05f, 0.06f, 0.08f, 1.0f } };
  VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
  ri.renderArea = { { 0, 0 }, vk.extent };
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  vkCmdBeginRendering(cmd, &ri);
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
  vkCmdEndRendering(cmd);
  imageBarrier(cmd, swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
  check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
  const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &sync.imageReady[slot];
  si.pWaitDstStageMask = &waitStage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &vk.renderDone[imageIndex];
  check(vkQueueSubmit(vk.queue, 1, &si, sync.fences[slot]), "vkQueueSubmit");
  VkPresentInfoKHR pi { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &vk.renderDone[imageIndex];
  pi.swapchainCount = 1;
  pi.pSwapchains = &vk.swapchain;
  pi.pImageIndices = &imageIndex;
  const VkResult presented = vkQueuePresentKHR(vk.queue, &pi);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
    app.swapchainDirty = true;
  } else {
    check(presented, "vkQueuePresentKHR");
  }
}

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Wraps the sun over the zenith: past 90 degrees of elevation it comes down on the far side.
void wrapSunElevation(Settings& s) {
  float e = std::fmod(s.sunElevation + 180.0f, 360.0f);
  if (e < 0.0f) e += 360.0f;
  e -= 180.0f;  // [-180, 180)
  if (e > 90.0f || e < -90.0f) {
    e = (e > 0.0f ? 180.0f : -180.0f) - e;
    s.sunRotation = std::fmod(s.sunRotation + 180.0f, 360.0f);
  }
  s.sunElevation = e;
}

} // namespace

void setStatus(AppState& app, const std::string& message) {
  app.status = message;
  std::printf("%s\n", message.c_str());
}

int runInteractive(const Options& opt) {
  const auto startTime = std::chrono::steady_clock::now();
  setLogCallback([](LogLevel level, const char* m, void*) {
    std::printf("[numos %s] %s\n", level == LogLevel::Info ? "info" : level == LogLevel::Warn ? "warn" : "error", m);
  }, nullptr);

  AppState app;
  app.opt = opt;
  SeedContents seed;
  app.settings = startSettings(opt, seed);
  app.confPath = opt.conf.empty() ? "numos.conf" : opt.conf;
  app.nightSkyDir = opt.nightSky.empty() ? opt.root + "/assets/nightsky" : opt.nightSky;
  app.exposure = opt.exposure;
  app.ground = !opt.noGround;
  app.secondarySky = opt.secondarySky;
  app.skyAmbientCheck = int(opt.skyAmbientCheck);
  app.accumulate = opt.accumulate > 0;
  if (opt.accumulate > 0) app.accumLimit = opt.accumulate;
  app.gpuTimings = opt.timings;
  app.sunSpeedDegPerSec = opt.sunSpeed;
  app.camera.position = Vector3(0.0f, opt.eyeHeight, 0.0f);
  app.camera.yawDeg = opt.yaw;
  app.camera.pitchDeg = opt.pitch;
  app.camera.fovYDeg = opt.fovY;
  app.weatherSeconds = opt.weatherSeconds;

  VulkanContext vk;
  createContext(vk, opt);
  glfwSetScrollCallback(vk.window, onScroll);
  if (!createSwapchain(vk, app.vsync)) {
    throw std::runtime_error("the window has no area");
  }
  initImGui(vk);

  VkCommandPoolCreateInfo cpi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = vk.queueFamily;
  VkCommandPool pool;
  check(vkCreateCommandPool(vk.device, &cpi, nullptr, &pool), "vkCreateCommandPool");
  FrameSync sync;
  VkCommandBuffer (&cmds)[kFramesInFlight] = sync.cmds;
  VkFence (&fences)[kFramesInFlight] = sync.fences;
  VkSemaphore (&imageReady)[kFramesInFlight] = sync.imageReady;
  VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = kFramesInFlight;
  check(vkAllocateCommandBuffers(vk.device, &cai, cmds), "vkAllocateCommandBuffers");
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(vk.device, &fci, nullptr, &fences[i]), "vkCreateFence");
    VkSemaphoreCreateInfo sci { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    check(vkCreateSemaphore(vk.device, &sci, nullptr, &imageReady[i]), "vkCreateSemaphore");
  }

  // A first frame right away, so the window never shows unpainted.
  uint64_t loadingPresents = 0;
  presentLoadingFrame(vk, app, sync, loadingPresents, "Starting", -1.0f);
  const double windowMs = msSince(startTime);

  {
    vulkan::VulkanGpuCreateInfo gci;
    gci.physicalDevice = vk.physicalDevice;
    gci.device = vk.device;
    gci.spirvDirectory = opt.root + "/build/spirv";
    gci.framesInFlight = kFramesInFlight;
    const std::string cachePath = pipelineCachePath(opt.root);
    const std::vector<uint8_t> loadedCache = readPipelineCache(cachePath);
    gci.pipelineCacheData = loadedCache;
    vulkan::VulkanGpu gpu(gci);
    const ViewerPasses viewerPasses = registerViewerPasses(gpu, opt.root);

    // Every pipeline before the first frame, on worker threads, with loading frames in between. The
    // first frame would otherwise create them one by one inside the frame, with the window frozen.
    const auto pipelinesStart = std::chrono::steady_clock::now();
    uint32_t pipelinesTotal = 0;
    gpu.createPipelines(std::max(2u, std::thread::hardware_concurrency()), [&](uint32_t done, uint32_t total) {
      pipelinesTotal = total;
      const std::string line = "Compiling shaders: " + std::to_string(done) + " of " + std::to_string(total)
                             + " (" + std::to_string(int(msSince(pipelinesStart) / 1000.0)) + " s)";
      presentLoadingFrame(vk, app, sync, loadingPresents, line, total ? float(done) / float(total) : 1.0f);
      return !glfwWindowShouldClose(vk.window);
    });
    const double pipelinesMs = msSince(pipelinesStart);
    writePipelineCache(cachePath, gpu.pipelineCacheData(), loadedCache);
    presentLoadingFrame(vk, app, sync, loadingPresents, "Baking the cloud fields", -1.0f);

    Renderer renderer;
    renderer.setSettings(app.settings);
    // Weather and clocks from the command line, as the headless run takes them.
    WeatherSnapshot weather = weatherFromSettings(app.settings);
    if (!opt.weather.empty()) {
      WeatherPreset preset;
      if (!weatherPresetFromName(opt.weather.c_str(), preset)) {
        throw std::runtime_error("unknown weather preset " + opt.weather);
      }
      weather = weatherPreset(preset, app.settings);
      app.weatherPreset = int(preset);
      app.useWeather = true;
    } else if (seed.hasWeather) {
      weather = seed.weather;
      app.useWeather = true;
    }
    app.blender = WeatherBlender(weather);
    if (!opt.weatherTo.empty()) {
      WeatherPreset preset;
      if (!weatherPresetFromName(opt.weatherTo.c_str(), preset)) {
        throw std::runtime_error("unknown weather preset " + opt.weatherTo);
      }
      app.blender.setTarget(weatherPreset(preset, app.settings), opt.weatherSeconds);
      app.weatherPreset = int(preset);
      app.useWeather = true;
    }
    if (seed.hasSimulation) {
      renderer.setSimulationState(seed.simulation);
    }
    NightSkyFiles nightSky;
    auto reloadNightSky = [&]() {
      nightSky = NightSkyFiles {};
      loadNightSky(app.nightSkyDir, nightSky, renderer);
      uint32_t found = 0;
      for (const std::vector<char>* v : { &nightSky.stars, &nightSky.milkyWay, &nightSky.starlightLow, &nightSky.moon }) {
        found += v->empty() ? 0u : 1u;
      }
      app.nightSkyStatus = std::to_string(found) + " of 4 night sky files found in " + app.nightSkyDir;
    };
    reloadNightSky();

    GpuHandle depth = 0, output = 0, accum = 0;
    uint32_t texWidth = 0, texHeight = 0;
    Mat4 prevView, prevProj;
    uint32_t lastCut = 1;
    uint64_t frame = 0;
    auto lastTime = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(vk.window)) {
      glfwPollEvents();
      int fbWidth = 0, fbHeight = 0;
      glfwGetFramebufferSize(vk.window, &fbWidth, &fbHeight);
      if (fbWidth <= 0 || fbHeight <= 0) {
        glfwWaitEvents();  // minimised
        lastTime = std::chrono::steady_clock::now();
        continue;
      }
      if (app.swapchainDirty || uint32_t(fbWidth) != vk.extent.width || uint32_t(fbHeight) != vk.extent.height) {
        if (!createSwapchain(vk, app.vsync)) {
          continue;
        }
        ImGui_ImplVulkan_SetMinImageCount(vk.minImageCount);
        app.swapchainDirty = false;
      }

      const auto now = std::chrono::steady_clock::now();
      const float dt = std::clamp(std::chrono::duration<float>(now - lastTime).count(), 0.0f, 0.25f);
      lastTime = now;
      app.frameMs = app.frameMs == 0.0f ? dt * 1000.0f : app.frameMs + (dt * 1000.0f - app.frameMs) * 0.05f;

      // ---- UI and input ----
      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();
      ImGuiIO& io = ImGui::GetIO();
      if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) app.showUi = !app.showUi;
      if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) app.screenshotRequest = true;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && app.camera.mouseLook) {
        setMouseLook(app.camera, vk.window, false, false);
      }
      if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        setMouseLook(app.camera, vk.window, !app.camera.mouseLook, true);
      }
      CameraInput ci;
      ci.keyboardFree = !io.WantTextInput && !io.WantCaptureKeyboard;
      ci.mouseFree = !io.WantCaptureMouse;
      ci.wheel = s_wheel;
      s_wheel = 0.0f;
      bool moved = updateCamera(app.camera, vk.window, dt, ci);
      if (app.camera.mouseLook) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
      } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
      }
      const FreeCamera cameraBeforeUi = app.camera;
      if (app.showUi) {
        drawMainPanel(app, renderer);
        drawSettingsWindow(app);
      }
      if (app.showStats) {
        drawStatsOverlay(app);
      }
      ImGui::Render();
      moved = moved || std::memcmp(&cameraBeforeUi.position, &app.camera.position, sizeof(Vector3)) != 0 ||
              cameraBeforeUi.yawDeg != app.camera.yawDeg || cameraBeforeUi.pitchDeg != app.camera.pitchDeg ||
              cameraBeforeUi.fovYDeg != app.camera.fovYDeg;

      const float simDt = app.paused ? 0.0f : dt * app.timeScale;
      if (app.sunSpeedDegPerSec != 0.0f && !app.settings.timeCycleEnable && simDt > 0.0f) {
        app.settings.sunElevation += app.sunSpeedDegPerSec * simDt;
        wrapSunElevation(app.settings);
        app.settingsChanged = true;
      }
      bool restartAccumulation = moved || app.settingsChanged || app.cameraCut;
      if (app.settingsChanged) {
        renderer.setSettings(app.settings);
        app.settingsChanged = false;
      }
      if (app.reloadNightSky) {
        reloadNightSky();
        app.reloadNightSky = false;
      }

      // ---- frame ----
      const uint32_t slot = uint32_t((frame + 1) % kFramesInFlight);
      waitFence(vk.device, fences[slot]);
      uint32_t imageIndex = 0;
      const VkResult acquired = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, imageReady[slot],
                                                      VK_NULL_HANDLE, &imageIndex);
      if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        app.swapchainDirty = true;
        continue;
      }
      if (acquired == VK_SUBOPTIMAL_KHR) {
        app.swapchainDirty = true;
      } else {
        check(acquired, "vkAcquireNextImageKHR");
      }
      ++frame;
      check(vkResetFences(vk.device, 1, &fences[slot]), "vkResetFences");
      VkCommandBuffer cmd = cmds[slot];
      check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
      VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
      gpu.beginFrame(cmd, frame);

      const uint32_t width = std::max(1u, uint32_t(std::lround(float(vk.extent.width) * app.renderScale)));
      const uint32_t height = std::max(1u, uint32_t(std::lround(float(vk.extent.height) * app.renderScale)));
      if (width != texWidth || height != texHeight) {
        gpu.destroy(depth);
        gpu.destroy(output);
        gpu.destroy(accum);
        TextureDesc td;
        td.format = Format::R32Float;
        td.width = width;
        td.height = height;
        td.debugName = "viewer depth";
        depth = gpu.createTexture(td);
        td.format = Format::RGBA8Unorm;
        td.debugName = "viewer output";
        output = gpu.createTexture(td);
        td.format = Format::RGBA32Float;
        td.debugName = "viewer accumulation";
        accum = gpu.createTexture(td);
        texWidth = width;
        texHeight = height;
        restartAccumulation = true;
      }
      app.renderWidth = width;
      app.renderHeight = height;

      const bool cut = frame == 1 || app.cameraCut;
      app.cameraCut = false;
      if (cut) lastCut = uint32_t(frame);
      const Vector3 eye = app.camera.position;
      const Vector3 forward = app.camera.forward();
      const Mat4 view = lookTo(eye, forward, Vector3(0.0f, 1.0f, 0.0f));
      const Mat4 proj = perspective(app.camera.fovYDeg * kDegreesToRadians, float(width) / float(height), 0.1f, 100000.0f);
      if (cut) {
        prevView = view;
        prevProj = proj;
      }
      const Camera camera = makeCamera(view.m, proj.m, prevView.m, prevProj.m, 0.0f, 0.0f, width, height, 0.1f, false);
      prevView = view;
      prevProj = proj;

      FrameInputs in;
      in.frameIndex = uint32_t(frame);
      in.deltaSeconds = simDt;
      in.cameraCut = cut;
      in.lastCameraCutFrame = lastCut;
      in.camera = camera;
      in.cameraPosition = eye;
      in.cameraPositionGame = eye;
      in.cameraForward = forward;
      std::memcpy(in.viewToWorld, &camera.viewToWorld, sizeof(in.viewToWorld));
      std::memcpy(in.projectionToView, &camera.projectionToView, sizeof(in.projectionToView));
      in.zUp = false;
      in.hostUnitsPerMeter = 1.0f;  // viewer world units are metres
      if (app.blender.transitioning()) restartAccumulation = true;
      app.blender.update(frame > 1 ? simDt : 0.0f);
      in.weather = app.useWeather ? &app.blender.current() : nullptr;
      in.renderWidth = width;
      in.renderHeight = height;
      in.localLights = opt.lights.empty() ? nullptr : opt.lights.data();
      in.localLightCount = uint32_t(opt.lights.size());
      in.lightningStrike = app.lightningRequest;
      app.lightningRequest = false;
      renderer.beginFrame(gpu, in);

      const FrameOutputs& outs0 = renderer.outputs();
      if (app.accumulate) {
        app.accumFrames = restartAccumulation ? 1u : std::min(app.accumFrames + 1u, app.accumLimit);
      } else {
        app.accumFrames = 0;
      }
      ViewerFrameParams vp;
      vp.noGround = !app.ground;
      vp.exposure = app.exposure;
      vp.frameIdx = uint32_t(frame);
      vp.width = width;
      vp.height = height;
      vp.secondarySky = app.secondarySky;
      vp.skyAmbientCheck = uint32_t(app.skyAmbientCheck);
      vp.accumFrames = app.accumFrames;
      vp.groundAlbedo = app.groundAlbedo;
      const ViewerArgs va = makeViewerArgs(outs0, vp);

      // Values for the UI.
      {
        const EmittedLight& sun = outs0.lights[FrameOutputs::kSunLight];
        app.sunElevationDeg = std::asin(std::clamp(-sun.direction.y, -1.0f, 1.0f)) / kDegreesToRadians;
        app.sunAzimuthDeg = std::fmod(std::atan2(-sun.direction.x, -sun.direction.z) / kDegreesToRadians + 360.0f, 360.0f);
        app.radianceScale = outs0.radianceScale;
        app.groundLevel = outs0.groundLevelWorldUnits;
        app.timeOfDayHours = renderer.simulationState().timeOfDayHours;
        const AtmosphereArgs& a = outs0.args;
        if (a.worldUnitsPerKm > 0.0f && a.cloudThickness > 0.0f) {
          // The camera's height in the cloud frame ties the cloud heights to world metres.
          app.cloudBaseY = eye.y + (a.cloudAltitude - a.cameraWorldPosYUpKm.y) * a.worldUnitsPerKm;
          app.cloudTopY = app.cloudBaseY + a.cloudThickness * a.worldUnitsPerKm;
          app.cloudHeightsValid = true;
        } else {
          app.cloudHeightsValid = false;
        }
      }

      dispatchViewerDepth(gpu, viewerPasses, va, camera, depth, width, height);
      renderer.dispatchPostDepth(gpu, depth, kMissLinearViewZ);
      dispatchViewerComposite(gpu, viewerPasses, va, camera, renderer.outputs(), depth, output, accum, width, height);
      gpu.endFrame();

      // ---- present: blit the composite, then the UI ----
      const VkImage swapImage = vk.images[imageIndex];
      VkMemoryBarrier toTransfer { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
      toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           1, &toTransfer, 0, nullptr, 0, nullptr);
      imageBarrier(cmd, swapImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkImageBlit blit {};
      blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      blit.srcOffsets[1] = { int32_t(width), int32_t(height), 1 };
      blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      blit.dstOffsets[1] = { int32_t(vk.extent.width), int32_t(vk.extent.height), 1 };
      vkCmdBlitImage(cmd, gpu.image(output), VK_IMAGE_LAYOUT_GENERAL, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_LINEAR);
      // The next frame's passes write the composite again only after the blit has read it.
      VkMemoryBarrier afterBlit { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
      afterBlit.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      afterBlit.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           1, &afterBlit, 0, nullptr, 0, nullptr);
      imageBarrier(cmd, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      VkRenderingAttachmentInfo color { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
      color.imageView = vk.views[imageIndex];
      color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo ri { VK_STRUCTURE_TYPE_RENDERING_INFO };
      ri.renderArea = { { 0, 0 }, vk.extent };
      ri.layerCount = 1;
      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = &color;
      vkCmdBeginRendering(cmd, &ri);
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
      vkCmdEndRendering(cmd);
      imageBarrier(cmd, swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
      check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

      const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
      si.waitSemaphoreCount = 1;
      si.pWaitSemaphores = &imageReady[slot];
      si.pWaitDstStageMask = &waitStage;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cmd;
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &vk.renderDone[imageIndex];
      check(vkQueueSubmit(vk.queue, 1, &si, fences[slot]), "vkQueueSubmit");
      if (frame == 1) {
        // The first frame bakes the cloud fields; its time ends the startup report.
        waitFence(vk.device, fences[slot]);
        char report[192];
        std::snprintf(report, sizeof(report),
                      "startup %.1f s: window %.0f ms, %u pipelines %.1f s, first frame %.0f ms",
                      msSince(startTime) / 1000.0, windowMs, pipelinesTotal, pipelinesMs / 1000.0,
                      msSince(pipelinesStart) - pipelinesMs);
        setStatus(app, report);
      }

      // GPU timings: twice a second, wait for this frame and read its timestamps.
      if (app.gpuTimings && frame % 30 == 0) {
        waitFence(vk.device, fences[slot]);
        app.timings = gpu.collectZoneTimings();
      }
      if (app.screenshotRequest) {
        app.screenshotRequest = false;
        const std::string path = screenshotName(app.screenshotDir);
        writeScreenshot(vk, pool, gpu, output, width, height, path);
        setStatus(app, "wrote " + path + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
      }

      VkPresentInfoKHR pi { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
      pi.waitSemaphoreCount = 1;
      pi.pWaitSemaphores = &vk.renderDone[imageIndex];
      pi.swapchainCount = 1;
      pi.pSwapchains = &vk.swapchain;
      pi.pImageIndices = &imageIndex;
      const VkResult presented = vkQueuePresentKHR(vk.queue, &pi);
      if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        app.swapchainDirty = true;
      } else {
        check(presented, "vkQueuePresentKHR");
      }
      app.frameIndex = frame;
    }

    check(vkDeviceWaitIdle(vk.device), "vkDeviceWaitIdle");
    renderer.releaseGpuResources(gpu);
  }

  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    vkDestroyFence(vk.device, fences[i], nullptr);
    vkDestroySemaphore(vk.device, imageReady[i], nullptr);
  }
  vkDestroyCommandPool(vk.device, pool, nullptr);
  destroyContext(vk);
  return 0;
}

} // namespace numos::viewer
