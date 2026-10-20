// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "app.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include <GLFW/glfw3.h>

namespace numos::viewer {

namespace {

uint32_t s_validationMessages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onValidation(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (s_validationMessages++ < 40) {
    std::printf("[validation] %s\n", data->pMessage);
  }
  return VK_FALSE;
}

} // namespace

void createContext(VulkanContext& vk, const Options& opt) {
  glfwSetErrorCallback([](int code, const char* text) { std::fprintf(stderr, "[glfw %d] %s\n", code, text); });
  if (!glfwInit()) {
    throw std::runtime_error("glfwInit failed");
  }
  if (!glfwVulkanSupported()) {
    throw std::runtime_error("GLFW found no Vulkan loader");
  }

  VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "numos_viewer";
  app.apiVersion = VK_API_VERSION_1_3;
  uint32_t glfwExtensionCount = 0;
  const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
  std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
  const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
  if (opt.validate) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }
  VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(extensions.size());
  ici.ppEnabledExtensionNames = extensions.data();
  if (opt.validate) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
  }
  check(vkCreateInstance(&ici, nullptr, &vk.instance), "vkCreateInstance");
  if (opt.validate) {
    VkDebugUtilsMessengerCreateInfoEXT mi { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    mi.pfnUserCallback = onValidation;
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(vk.instance, "vkCreateDebugUtilsMessengerEXT"));
    check(create(vk.instance, &mi, nullptr, &vk.messenger), "vkCreateDebugUtilsMessengerEXT");
    std::printf("validation enabled\n");
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  vk.window = glfwCreateWindow(int(opt.width), int(opt.height), "NUMOS viewer", nullptr, nullptr);
  if (!vk.window) {
    throw std::runtime_error("glfwCreateWindow failed");
  }
  check(glfwCreateWindowSurface(vk.instance, vk.window, nullptr, &vk.surface), "glfwCreateWindowSurface");

  // The first discrete GPU, as the headless viewer picks, else the first one.
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(vk.instance, &count, nullptr);
  std::vector<VkPhysicalDevice> gpus(count);
  vkEnumeratePhysicalDevices(vk.instance, &count, gpus.data());
  if (gpus.empty()) {
    throw std::runtime_error("no Vulkan device");
  }
  vk.physicalDevice = gpus[0];
  for (VkPhysicalDevice g : gpus) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      vk.physicalDevice = g;
      break;
    }
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(vk.physicalDevice, &props);
  vk.deviceName = props.deviceName;
  std::printf("GPU: %s\n", props.deviceName);

  vkGetPhysicalDeviceQueueFamilyProperties(vk.physicalDevice, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(vk.physicalDevice, &count, families.data());
  bool found = false;
  for (uint32_t i = 0; i < count && !found; ++i) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vk.physicalDevice, i, vk.surface, &present);
    const VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if ((families[i].queueFlags & want) == want && present) {
      vk.queueFamily = i;
      found = true;
    }
  }
  if (!found) {
    throw std::runtime_error("no queue family with graphics, compute and present");
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  qci.queueFamilyIndex = vk.queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures features {};
  features.shaderStorageImageExtendedFormats = VK_TRUE;
  features.shaderStorageImageReadWithoutFormat = VK_TRUE;
  features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
  VkPhysicalDeviceVulkan13Features features13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
  features13.dynamicRendering = VK_TRUE;
  const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
  VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  dci.pNext = &features13;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.pEnabledFeatures = &features;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = deviceExtensions;
  check(vkCreateDevice(vk.physicalDevice, &dci, nullptr, &vk.device), "vkCreateDevice");
  vkGetDeviceQueue(vk.device, vk.queueFamily, 0, &vk.queue);
}

bool createSwapchain(VulkanContext& vk, bool vsync) {
  int fbWidth = 0, fbHeight = 0;
  glfwGetFramebufferSize(vk.window, &fbWidth, &fbHeight);
  if (fbWidth <= 0 || fbHeight <= 0) {
    return false;
  }
  check(vkDeviceWaitIdle(vk.device), "vkDeviceWaitIdle");

  VkSurfaceCapabilitiesKHR caps;
  check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physicalDevice, vk.surface, &caps),
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    throw std::runtime_error("the surface does not support transfer writes to its images");
  }
  if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
    return false;
  }

  // The composite writes display-encoded RGBA8, so the swapchain is UNORM (no second encode).
  uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physicalDevice, vk.surface, &count, formats.data());
  VkSurfaceFormatKHR chosen = formats.at(0);
  for (const VkSurfaceFormatKHR& f : formats) {
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = f;
      break;
    }
  }

  vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physicalDevice, vk.surface, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physicalDevice, vk.surface, &count, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync) {
    for (VkPresentModeKHR want : { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR }) {
      if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
        mode = want;
        break;
      }
    }
  }

  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {
    extent.width = std::clamp(uint32_t(fbWidth), caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(uint32_t(fbHeight), caps.minImageExtent.height, caps.maxImageExtent.height);
  }
  uint32_t imageCount = std::max(caps.minImageCount + 1, 2u);
  if (caps.maxImageCount > 0) {
    imageCount = std::min(imageCount, caps.maxImageCount);
  }

  VkSwapchainCreateInfoKHR sci { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
  sci.surface = vk.surface;
  sci.minImageCount = imageCount;
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = vk.swapchain;
  VkSwapchainKHR swapchain;
  check(vkCreateSwapchainKHR(vk.device, &sci, nullptr, &swapchain), "vkCreateSwapchainKHR");
  destroySwapchain(vk);
  vk.swapchain = swapchain;
  vk.format = chosen.format;
  vk.extent = extent;
  vk.presentMode = mode;
  vk.minImageCount = std::max(caps.minImageCount, 2u);

  vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &count, nullptr);
  vk.images.resize(count);
  vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &count, vk.images.data());
  for (VkImage image : vk.images) {
    VkImageViewCreateInfo vci { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vk.format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view;
    check(vkCreateImageView(vk.device, &vci, nullptr, &view), "vkCreateImageView");
    vk.views.push_back(view);
    VkSemaphoreCreateInfo semi { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore done;
    check(vkCreateSemaphore(vk.device, &semi, nullptr, &done), "vkCreateSemaphore");
    vk.renderDone.push_back(done);
  }
  return true;
}

void destroySwapchain(VulkanContext& vk) {
  for (VkImageView v : vk.views) vkDestroyImageView(vk.device, v, nullptr);
  for (VkSemaphore s : vk.renderDone) vkDestroySemaphore(vk.device, s, nullptr);
  vk.views.clear();
  vk.renderDone.clear();
  vk.images.clear();
  if (vk.swapchain) {
    vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
    vk.swapchain = VK_NULL_HANDLE;
  }
}

void destroyContext(VulkanContext& vk) {
  if (vk.device) {
    vkDeviceWaitIdle(vk.device);
    destroySwapchain(vk);
    vkDestroyDevice(vk.device, nullptr);
  }
  if (vk.surface) vkDestroySurfaceKHR(vk.instance, vk.surface, nullptr);
  if (vk.messenger) {
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(vk.instance, "vkDestroyDebugUtilsMessengerEXT"));
    destroyMessenger(vk.instance, vk.messenger, nullptr);
    std::printf("validation messages: %u\n", s_validationMessages);
  }
  if (vk.instance) vkDestroyInstance(vk.instance, nullptr);
  if (vk.window) glfwDestroyWindow(vk.window);
  glfwTerminate();
  vk = VulkanContext {};
}

} // namespace numos::viewer
