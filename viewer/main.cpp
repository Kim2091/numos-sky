// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// numos_viewer: renders the NUMOS sky, clouds and aerial perspective over a flat ground plane with
// the reference Vulkan backend, headless, and writes the last frame to a PPM (tools/run_viewer.py
// converts it to PNG). This is the standalone check that the library runs and looks right.
// With --interactive it opens a window instead (app_main.cpp): a free camera and every setting live.
//
//   numos_viewer [--conf rtx.conf] [--sun-elevation deg] [--sun-azimuth deg] [--yaw deg] [--pitch deg]
//                [--eye-height m] [--width w] [--height h] [--frames n] [--exposure e]
//                [--set key=value]... [--fov-y DEG] [--accumulate n] [--secondary-sky] [--timings] [--out file.ppm]
//                [--night-sky dir] [--no-ground] [--sky-ambient-check n]
//                [--light x,y,z,r,g,b,radius]...   (sphere light in metres, radiance in viewer units)
//                [--weather Preset] [--lightning-at frame]... [--lightning-pos x,y,z]  (strike there, metres)
//                [--sequence-dir dir] [--sequence-every n]  (also write every nth frame as dir/fNNNNN.ppm)
//                [--seed NUMOS1-...] (the whole configuration from a seed; --set still applies over it)
//                [--print-seed]      (print the seed of the configuration, weather and clocks at the end)
//                [--sun-speed deg/s] (sun elevation changes by this much per second of 60 fps frames)
//                [--interactive]     (window, free camera and settings UI; needs NUMOS_BUILD_VIEWER_UI)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "numos/log.h"
#include "numos/renderer.h"
#include "numos/seed.h"
#include "numos/settings.h"
#include "numos/weather.h"
#include "numos_vulkan.h"
#include "viewer_shared.h"
#if NUMOS_VIEWER_UI
#include "app.h"
#endif

using namespace numos;
using namespace numos::viewer;

int main(int argc, char** argv) try {
  const Options opt = parse(argc, argv);
  if (opt.interactive) {
#if NUMOS_VIEWER_UI
    return runInteractive(opt);
#else
    throw std::runtime_error("--interactive: this numos_viewer was built with NUMOS_BUILD_VIEWER_UI off");
#endif
  }
  setLogCallback([](LogLevel level, const char* m, void*) {
    std::printf("[numos %s] %s\n", level == LogLevel::Info ? "info" : level == LogLevel::Warn ? "warn" : "error", m);
  }, nullptr);

  // ---- settings ----
  SeedContents seed;
  Settings settings = startSettings(opt, seed);

  // ---- Vulkan ----
  VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "numos_viewer";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ici.pApplicationInfo = &app;
  const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
  const char* extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
  if (opt.validate) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = extensions;
  }
  VkInstance instance;
  check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
  static uint32_t s_validationMessages = 0;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  if (opt.validate) {
    VkDebugUtilsMessengerCreateInfoEXT mi { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    mi.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 {
      if (s_validationMessages++ < 40) {
        std::printf("[validation] %s\n", data->pMessage);
      }
      return VK_FALSE;
    };
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    check(create(instance, &mi, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");
    std::printf("validation enabled\n");
  }

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> gpus(count);
  vkEnumeratePhysicalDevices(instance, &count, gpus.data());
  VkPhysicalDevice pd = gpus.at(0);
  for (VkPhysicalDevice g : gpus) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      pd = g;
      std::printf("GPU: %s\n", p.deviceName);
      break;
    }
  }

  vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());
  uint32_t family = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      family = i;
      break;
    }
  }
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures features {};
  features.shaderStorageImageExtendedFormats = VK_TRUE;
  features.shaderStorageImageReadWithoutFormat = VK_TRUE;
  features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
  VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &features;
  VkDevice device;
  check(vkCreateDevice(pd, &dci, nullptr, &device), "vkCreateDevice");
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);

  constexpr uint32_t kFramesInFlight = 2;
  VkCommandPoolCreateInfo cpi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = family;
  VkCommandPool pool;
  check(vkCreateCommandPool(device, &cpi, nullptr, &pool), "vkCreateCommandPool");
  VkCommandBuffer cmds[kFramesInFlight];
  VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = kFramesInFlight;
  check(vkAllocateCommandBuffers(device, &cai, cmds), "vkAllocateCommandBuffers");
  VkFence fences[kFramesInFlight];
  for (VkFence& f : fences) {
    VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device, &fci, nullptr, &f), "vkCreateFence");
  }

  {
    vulkan::VulkanGpuCreateInfo gci;
    gci.physicalDevice = pd;
    gci.device = device;
    gci.spirvDirectory = opt.root + "/build/spirv";
    gci.framesInFlight = kFramesInFlight;
    const std::string cachePath = pipelineCachePath(opt.root);
    const std::vector<uint8_t> loadedCache = readPipelineCache(cachePath);
    gci.pipelineCacheData = loadedCache;
    vulkan::VulkanGpu gpu(gci);
    const ViewerPasses viewerPasses = registerViewerPasses(gpu, opt.root);

    Renderer renderer;
    renderer.setSettings(settings);
    WeatherSnapshot weather;
    bool useWeather = false;
    if (!opt.weather.empty()) {
      WeatherPreset preset;
      if (!weatherPresetFromName(opt.weather.c_str(), preset)) {
        throw std::runtime_error("unknown weather preset " + opt.weather);
      }
      weather = weatherPreset(preset, settings);
      useWeather = true;
    } else if (seed.hasWeather) {
      weather = seed.weather;
      useWeather = true;
    }
    if (seed.hasSimulation) {
      renderer.setSimulationState(seed.simulation);
    }
    // A forced strike makes the render repeatable: no automatic strikes besides it.
    if (!opt.lightningAt.empty()) {
      settings.lightningStrikesPerMinute = 0.0f;
      weather.lightningStrikesPerMinute = 0.0f;
      renderer.setSettings(settings);
    }
    WeatherBlender blender(useWeather ? weather : weatherFromSettings(settings));
    if (!opt.weatherTo.empty()) {
      WeatherPreset preset;
      if (!weatherPresetFromName(opt.weatherTo.c_str(), preset)) {
        throw std::runtime_error("unknown weather preset " + opt.weatherTo);
      }
      WeatherSnapshot target = weatherPreset(preset, settings);
      if (!opt.lightningAt.empty()) {
        target.lightningStrikesPerMinute = 0.0f;
      }
      blender.setTarget(target, opt.weatherSeconds);
      useWeather = true;
    }
    // The night sky's data, from the repository's assets (built by tools/build_nightsky_assets.py).
    NightSkyFiles nightSky;
    loadNightSky(opt.nightSky.empty() ? opt.root + "/assets/nightsky" : opt.nightSky, nightSky, renderer);

    // ---- camera ----
    const float yaw = opt.yaw * kDegreesToRadians;
    const float pitch = opt.pitch * kDegreesToRadians;
    const Vector3 eye0(0.0f, opt.eyeHeight, 0.0f);
    const Vector3 forward(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    const Mat4 proj = perspective(opt.fovY * kDegreesToRadians, float(opt.width) / float(opt.height), 0.1f, 100000.0f);
    // --fly-speed moves the eye along the view direction each frame, so the previous-frame
    // matrices differ from the current ones the way a host's do.
    auto eyeAt = [&](uint32_t f) {
      const float d = opt.flySpeed * float(f) / 60.0f;
      return Vector3(eye0.x + forward.x * d, eye0.y + forward.y * d, eye0.z + forward.z * d);
    };

    GpuHandle depth = 0, output = 0, accum = 0;

    float lastRadianceScale = 1.0f;
    uint32_t flashFrames = 0, flashes = 0;  // frames with the lightning light on, and its onsets
    bool flashOn = false;
    // --sequence-dir: after the frame's submit, wait for the GPU and copy the output into a mapped
    // buffer (one, reused). Stalls every written frame; for offline demo strips only.
    VkBuffer seqBuffer = VK_NULL_HANDLE;
    VkDeviceMemory seqMemory = VK_NULL_HANDLE;
    auto writeSequenceFrame = [&](uint32_t frame) {
      const VkDeviceSize bytes = VkDeviceSize(opt.width) * opt.height * 4;
      if (!seqBuffer) {
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(device, &bci, nullptr, &seqBuffer), "vkCreateBuffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, seqBuffer, &req);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
          if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
            type = i;
            break;
          }
        }
        VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &mai, nullptr, &seqMemory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, seqBuffer, seqMemory, 0), "vkBindBufferMemory");
      }
      check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
      VkCommandBuffer c = cmds[frame % kFramesInFlight];
      check(vkResetCommandBuffer(c, 0), "vkResetCommandBuffer");
      VkCommandBufferBeginInfo cbi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      check(vkBeginCommandBuffer(c, &cbi), "vkBeginCommandBuffer");
      VkBufferImageCopy copy {};
      copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      copy.imageExtent = { opt.width, opt.height, 1 };
      vkCmdCopyImageToBuffer(c, gpu.image(output), VK_IMAGE_LAYOUT_GENERAL, seqBuffer, 1, &copy);
      check(vkEndCommandBuffer(c), "vkEndCommandBuffer");
      VkSubmitInfo s { VK_STRUCTURE_TYPE_SUBMIT_INFO };
      s.commandBufferCount = 1;
      s.pCommandBuffers = &c;
      check(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE), "vkQueueSubmit");
      check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
      void* mapped;
      check(vkMapMemory(device, seqMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
      char name[32];
      std::snprintf(name, sizeof(name), "/f%05u.ppm", frame);
      writePpm(opt.sequenceDir + name, opt.width, opt.height, static_cast<const uint8_t*>(mapped));
      vkUnmapMemory(device, seqMemory);
    };
    for (uint32_t frame = 1; frame <= opt.frames; ++frame) {
      const uint32_t slot = frame % kFramesInFlight;
      check(vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX), "vkWaitForFences");
      check(vkResetFences(device, 1, &fences[slot]), "vkResetFences");
      VkCommandBuffer cmd = cmds[slot];
      check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
      VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
      gpu.beginFrame(cmd, frame);

      if (!depth) {
        TextureDesc td;
        td.format = Format::R32Float;
        td.width = opt.width;
        td.height = opt.height;
        td.debugName = "viewer depth";
        depth = gpu.createTexture(td);
        td.format = Format::RGBA8Unorm;
        td.debugName = "viewer output";
        output = gpu.createTexture(td);
        td.format = Format::RGBA32Float;
        td.debugName = "viewer accumulation";
        accum = gpu.createTexture(td);
      }

      const Vector3 eye = eyeAt(frame);
      const Mat4 view = lookTo(eye, forward, Vector3(0.0f, 1.0f, 0.0f));
      const Mat4 prevView = lookTo(eyeAt(frame > 1 ? frame - 1 : frame), forward, Vector3(0.0f, 1.0f, 0.0f));
      const Camera camera = makeCamera(view.m, proj.m, prevView.m, proj.m, 0.0f, 0.0f, opt.width, opt.height, 0.1f, false);
      Mat4 viewToWorld = Mat4::identity();
      std::memcpy(viewToWorld.m, &camera.viewToWorld, sizeof(viewToWorld.m));
      Mat4 projectionToView;
      std::memcpy(projectionToView.m, &camera.projectionToView, sizeof(projectionToView.m));

      FrameInputs in;
      in.frameIndex = frame;
      in.deltaSeconds = 1.0f / 60.0f;
      in.cameraCut = frame == 1;
      in.lastCameraCutFrame = 1;
      in.camera = camera;
      in.cameraPosition = eye;
      in.cameraPositionGame = eye;
      in.cameraForward = forward;
      std::memcpy(in.viewToWorld, viewToWorld.m, sizeof(in.viewToWorld));
      std::memcpy(in.projectionToView, projectionToView.m, sizeof(in.projectionToView));
      in.zUp = false;
      in.hostUnitsPerMeter = 1.0f;  // viewer world units are metres
      blender.update(frame > 1 ? 1.0f / 60.0f : 0.0f);
      in.weather = useWeather ? &blender.current() : nullptr;
      in.renderWidth = opt.width;
      in.renderHeight = opt.height;
      in.localLights = opt.lights.empty() ? nullptr : opt.lights.data();
      in.localLightCount = uint32_t(opt.lights.size());
      in.lightningStrike = std::find(opt.lightningAt.begin(), opt.lightningAt.end(), frame) != opt.lightningAt.end();
      in.lightningStrikeAtPosition = opt.lightningAtPosition;
      in.lightningStrikePosition = opt.lightningPosition;
      if (opt.sunSpeed != 0.0f) {
        settings.sunElevation = opt.sunElevation + opt.sunSpeed * float(frame - 1) / 60.0f;
        renderer.setSettings(settings);
      }
      renderer.beginFrame(gpu, in);

      const FrameOutputs& outs0 = renderer.outputs();
      ViewerFrameParams vp;
      vp.noGround = opt.noGround;
      vp.exposure = opt.exposure;
      vp.frameIdx = frame;
      vp.width = opt.width;
      vp.height = opt.height;
      vp.secondarySky = opt.secondarySky;
      vp.skyAmbientCheck = opt.skyAmbientCheck;
      {
        const uint32_t firstAccumFrame = opt.frames - std::min(opt.accumulate, opt.frames) + 1;
        vp.accumFrames = opt.accumulate && frame >= firstAccumFrame ? frame - firstAccumFrame + 1 : 0;
      }
      const ViewerArgs va = makeViewerArgs(outs0, vp);
      lastRadianceScale = outs0.radianceScale;
      {
        const EmittedLight& flash = outs0.lights[FrameOutputs::kLightningLight];
        flashes += flash.active && !flashOn ? 1u : 0u;
        flashFrames += flash.active ? 1u : 0u;
        flashOn = flash.active;
      }

      dispatchViewerDepth(gpu, viewerPasses, va, camera, depth, opt.width, opt.height);
      renderer.dispatchPostDepth(gpu, depth, kMissLinearViewZ);
      dispatchViewerComposite(gpu, viewerPasses, va, camera, renderer.outputs(), depth, output, accum,
                              opt.width, opt.height);

      gpu.endFrame();
      check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
      VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cmd;
      check(vkQueueSubmit(queue, 1, &si, fences[slot]), "vkQueueSubmit");
      if (!opt.sequenceDir.empty() && frame % opt.sequenceEvery == 0) {
        writeSequenceFrame(frame);
      }
    }
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    writePipelineCache(cachePath, gpu.pipelineCacheData(), loadedCache);
    if (seqBuffer) {
      vkDestroyBuffer(device, seqBuffer, nullptr);
      vkFreeMemory(device, seqMemory, nullptr);
    }
    if (opt.timings) {
      // GPU time of the last frame's NUMOS zones (timestamp queries).
      for (const auto& z : gpu.collectZoneTimings()) {
        std::printf("gpu %*s%-48s %7.3f ms\n", int(z.depth * 2), "", z.name.c_str(), z.ms);
      }
    }

    // ---- read back the last frame ----
    VkBufferCreateInfo rbi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    const VkDeviceSize pixels = VkDeviceSize(opt.width) * opt.height;
    const VkDeviceSize hdrOffset = (pixels * 4 + 15) & ~VkDeviceSize(15);
    const bool wantHdr = !opt.hdrOut.empty() && accum;
    rbi.size = wantHdr ? hdrOffset + pixels * 16 : pixels * 4;
    rbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rb;
    check(vkCreateBuffer(device, &rbi, nullptr, &rb), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, rb, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags &
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        type = i;
        break;
      }
    }
    VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory rbm;
    check(vkAllocateMemory(device, &mai, nullptr, &rbm), "vkAllocateMemory");
    check(vkBindBufferMemory(device, rb, rbm, 0), "vkBindBufferMemory");
    VkCommandBuffer cmd = cmds[0];
    check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
    VkBufferImageCopy copy {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { opt.width, opt.height, 1 };
    vkCmdCopyImageToBuffer(cmd, gpu.image(output), VK_IMAGE_LAYOUT_GENERAL, rb, 1, &copy);
    if (wantHdr) {
      copy.bufferOffset = hdrOffset;
      vkCmdCopyImageToBuffer(cmd, gpu.image(accum), VK_IMAGE_LAYOUT_GENERAL, rb, 1, &copy);
    }
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
    void* mapped;
    check(vkMapMemory(device, rbm, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    writePpm(opt.out, opt.width, opt.height, static_cast<const uint8_t*>(mapped));
    if (wantHdr) {
      // PFM: little-endian float RGB, rows bottom to top. For an offline denoiser (OIDN).
      std::ofstream pfm(opt.hdrOut, std::ios::binary);
      pfm << "PF\n" << opt.width << " " << opt.height << "\n-1.0\n";
      const float* hdr = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped) + hdrOffset);
      std::vector<float> row(size_t(opt.width) * 3);
      for (uint32_t y = opt.height; y-- > 0;) {
        for (uint32_t x = 0; x < opt.width; ++x) {
          for (int c = 0; c < 3; ++c) {
            row[x * 3 + c] = hdr[(size_t(y) * opt.width + x) * 4 + c] * (opt.exposure / lastRadianceScale);
          }
        }
        pfm.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size() * sizeof(float)));
      }
    }
    vkUnmapMemory(device, rbm);
    vkDestroyBuffer(device, rb, nullptr);
    vkFreeMemory(device, rbm, nullptr);
    {
      const AtmosphereArgs& a = renderer.outputs().args;
      std::printf("args: apFroxels=%ux%ux%u apRange=%g apFar=%ux%ux%u "
                  "fogExtinctionPerKm=%g worldUnitsPerKm=%g cloudRT=%u voxelShadows=%u apLocalCount=%u\n",
                  a.apFroxelSizeX, a.apFroxelSizeY, a.apFroxelSlices, a.apFroxelRange,
                  a.apFarSizeX, a.apFarSizeY, a.apFarSlices, a.fogExtinctionPerKm,
                  a.worldUnitsPerKm, a.cloudRenderRTEnable, a.cloudVoxelShadowsEnable, a.apLocalLightCount);
      const EmittedLight& flash = renderer.outputs().lights[FrameOutputs::kLightningLight];
      if (flashes > 0u) {
        std::printf("lightning: %u flashes, lit %u of %u frames\n", flashes, flashFrames, opt.frames);
      }
      if (flash.active) {
        std::printf("lightning: envelope %g strike %g %g %g km; light at %g %g %g radius %g radiance %g %g %g "
                    "(radianceScale %g)\n", a.lightningEnvelope, a.lightningStrikePosKm.x, a.lightningStrikePosKm.y,
                    a.lightningStrikePosKm.z, flash.position.x, flash.position.y, flash.position.z, flash.radius,
                    flash.radiance.x, flash.radiance.y, flash.radiance.z, renderer.outputs().radianceScale);
      }
    }
    std::printf("wrote %s (%ux%u, %u frames); sun radiance %.3f %.3f %.3f\n", opt.out.c_str(), opt.width, opt.height,
                opt.frames, renderer.outputs().lights[0].radiance.x, renderer.outputs().lights[0].radiance.y,
                renderer.outputs().lights[0].radiance.z);
    if (opt.printSeed) {
      const SimulationState sim = renderer.simulationState();
      std::printf("seed: %s\n", encodeSeed(renderer.settings(), useWeather ? &blender.current() : nullptr, &sim).c_str());
    }
    renderer.releaseGpuResources(gpu);
  }

  for (VkFence f : fences) vkDestroyFence(device, f, nullptr);
  vkDestroyCommandPool(device, pool, nullptr);
  vkDestroyDevice(device, nullptr);
  if (messenger) {
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    destroyMessenger(instance, messenger, nullptr);
    std::printf("validation messages: %u\n", s_validationMessages);
  }
  vkDestroyInstance(instance, nullptr);
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "numos_viewer: %s\n", e.what());
  return 1;
}
