// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "numos_vulkan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "numos/log.h"
#include "numos/shaders.h"

namespace numos::vulkan {

namespace {

void check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) {
    throw std::runtime_error(std::string("NUMOS Vulkan: ") + what + " failed (" + std::to_string(int(r)) + ")");
  }
}

VkFormat formatFromSpirv(uint32_t spirvFormat) {
  switch (spirvFormat) {
  case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;  // Rgba32f
  case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;  // Rgba16f
  case 3: return VK_FORMAT_R32_SFLOAT;           // R32f
  case 4: return VK_FORMAT_R8G8B8A8_UNORM;       // Rgba8
  case 6: return VK_FORMAT_R32G32_SFLOAT;        // Rg32f
  case 9: return VK_FORMAT_R16_SFLOAT;           // R16f
  case 15: return VK_FORMAT_R8_UNORM;            // R8
  case 33: return VK_FORMAT_R32_UINT;            // R32ui
  default: return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

VkDescriptorType descriptorType(BindingKind kind) {
  switch (kind) {
  case BindingKind::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  case BindingKind::SampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  case BindingKind::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  case BindingKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
  case BindingKind::CombinedTextureSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  case BindingKind::StructuredBuffer:
  case BindingKind::RWStructuredBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  case BindingKind::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  }
  return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}

} // namespace

VkFormat toVkFormat(Format format) {
  switch (format) {
  case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
  case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
  case Format::R16Float: return VK_FORMAT_R16_SFLOAT;
  case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
  case Format::R32Uint: return VK_FORMAT_R32_UINT;
  case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
  case Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
  case Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
  case Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
  }
  return VK_FORMAT_R16G16B16A16_SFLOAT;
}

struct VulkanGpu::Impl {
  enum class Kind { Texture, Buffer, Sampler };

  struct Resource {
    Kind kind = Kind::Texture;
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    TextureDesc tdesc {};
    BufferDesc bdesc {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool imported = false;
    bool isArray = false;
    uint64_t copyFrame = 0;  // readback: frame of the last copy into it (0 = none)
    std::unordered_map<uint64_t, VkImageView> views;  // key: (mip << 1) | arrayFlag
  };

  struct Pipeline {
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool failed = false;
  };

  struct Frame {
    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ringMemory = VK_NULL_HANDLE;
    uint8_t* ringMapped = nullptr;
    GpuHandle ringHandle = 0;
    size_t ringOffset = 0;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<GpuHandle> pendingDestroy;
    // Zone timing: two timestamps per zone, recorded in begin order.
    VkQueryPool queries = VK_NULL_HANDLE;
    struct Zone { std::string name; uint32_t depth; uint32_t begin; uint32_t end; };
    std::vector<Zone> zones;
    uint32_t queryCount = 0;
  };
  static constexpr uint32_t kMaxZoneQueries = 512;
  std::vector<uint32_t> zoneStack;  // indices into frame->zones
  double timestampPeriodNs = 1.0;

  VulkanGpuCreateInfo info;
  VkPhysicalDeviceMemoryProperties memProps {};
  VkDeviceSize uboAlign = 256;
  std::unordered_map<GpuHandle, Resource> resources;
  GpuHandle nextHandle = 1;
  std::vector<Pipeline> pipelines;
  VkPipelineCache pipelineCache = VK_NULL_HANDLE;
  struct ExternalPass { PassInfo info; std::vector<uint32_t> spirv; };
  std::vector<ExternalPass> external;

  const PassInfo& info_(uint32_t pass) const {
    return pass < passCount() ? passInfo(pass) : external[pass - passCount()].info;
  }
  std::vector<Frame> frames;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  uint64_t frameIndex = 0;
  Frame* frame = nullptr;
  std::unordered_map<uint64_t, GpuHandle> dummies;  // placeholder per (dim, storage, format)
  GpuHandle dummyBuffer = 0;
  GpuHandle dummySampler = 0;

  VkDevice dev() const { return info.device; }

  uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & flags) == flags) {
        return i;
      }
    }
    throw std::runtime_error("NUMOS Vulkan: no suitable memory type");
  }

  VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
    VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req.memoryTypeBits, flags);
    VkDeviceMemory mem;
    check(vkAllocateMemory(dev(), &ai, nullptr, &mem), "vkAllocateMemory");
    return mem;
  }

  GpuHandle add(Resource r) {
    const GpuHandle h = nextHandle++;
    resources.emplace(h, std::move(r));
    return h;
  }

  Resource& get(GpuHandle h) {
    auto it = resources.find(h);
    if (it == resources.end()) {
      throw std::runtime_error("NUMOS Vulkan: unknown handle");
    }
    return it->second;
  }

  void fullBarrier() {
    VkMemoryBarrier mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
                     | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(cmd, stages, stages | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  }

  VkBuffer makeBuffer(size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, VkDeviceMemory& mem, void** mapped) {
    VkBufferCreateInfo bi { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = std::max<size_t>(size, 16);
    bi.usage = usage;
    VkBuffer b;
    check(vkCreateBuffer(dev(), &bi, nullptr, &b), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev(), b, &req);
    mem = allocate(req, flags);
    check(vkBindBufferMemory(dev(), b, mem, 0), "vkBindBufferMemory");
    if (mapped) {
      check(vkMapMemory(dev(), mem, 0, VK_WHOLE_SIZE, 0, mapped), "vkMapMemory");
    }
    return b;
  }

  // mip == ~0u: every mip; otherwise a single-level view of that mip.
  VkImageView viewFor(GpuHandle h, uint32_t mip, bool forceArray) {
    Resource& r = get(h);
    const bool single = mip != ~0u;
    const uint32_t baseMip = single ? mip : 0u;
    const uint64_t key = (uint64_t(single ? baseMip + 1 : 0) << 1) | (forceArray || r.isArray ? 1u : 0u);
    auto it = r.views.find(key);
    if (it != r.views.end()) {
      return it->second;
    }
    VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = r.image;
    vi.format = r.format;
    vi.viewType = r.tdesc.type == TextureType::Tex3D ? VK_IMAGE_VIEW_TYPE_3D
                : (forceArray || r.isArray) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = baseMip;
    vi.subresourceRange.levelCount = single ? 1u : r.tdesc.mipLevels;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = r.isArray ? r.tdesc.depth : 1u;
    VkImageView v;
    check(vkCreateImageView(dev(), &vi, nullptr, &v), "vkCreateImageView");
    r.views.emplace(key, v);
    return v;
  }

  void transitionToGeneral(VkImage image, uint32_t mips, uint32_t layers) {
    VkImageMemoryBarrier ib { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image;
    ib.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers };
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
  }

  GpuHandle dummyTexture(BindingDim dim, bool storage, uint32_t spirvFormat) {
    const uint64_t key = (uint64_t(dim) << 40) | (uint64_t(storage) << 32) | spirvFormat;
    auto it = dummies.find(key);
    if (it != dummies.end()) {
      return it->second;
    }
    Resource r;
    r.kind = Kind::Texture;
    r.tdesc.type = dim == BindingDim::Tex3D ? TextureType::Tex3D : TextureType::Tex2D;
    r.tdesc.debugName = "NUMOS placeholder";
    r.isArray = dim == BindingDim::Tex2DArray;
    r.format = storage ? formatFromSpirv(spirvFormat) : VK_FORMAT_R16G16B16A16_SFLOAT;
    createImage(r);
    const GpuHandle h = add(std::move(r));
    const float zero[4] = {};
    clear(h, zero);
    dummies.emplace(key, h);
    return h;
  }

  void createImage(Resource& r) {
    VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = r.tdesc.type == TextureType::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = r.format;
    ci.extent = { r.tdesc.width, r.tdesc.height, r.tdesc.type == TextureType::Tex3D ? r.tdesc.depth : 1u };
    ci.mipLevels = r.tdesc.mipLevels;
    ci.arrayLayers = r.isArray ? r.tdesc.depth : 1u;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
             | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(dev(), &ci, nullptr, &r.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev(), r.image, &req);
    r.memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkBindImageMemory(dev(), r.image, r.memory, 0), "vkBindImageMemory");
    transitionToGeneral(r.image, r.tdesc.mipLevels, ci.arrayLayers);
  }

  void clear(GpuHandle h, const float rgba[4]) {
    Resource& r = get(h);
    VkClearColorValue c {};
    std::memcpy(c.float32, rgba, sizeof(float) * 4);
    const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, r.tdesc.mipLevels, 0,
                                          r.isArray ? r.tdesc.depth : 1u };
    vkCmdClearColorImage(cmd, r.image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    fullBarrier();
  }

  void release(Resource& r) {
    for (auto& v : r.views) {
      vkDestroyImageView(dev(), v.second, nullptr);
    }
    if (!r.imported) {
      if (r.image) vkDestroyImage(dev(), r.image, nullptr);
      if (r.buffer) vkDestroyBuffer(dev(), r.buffer, nullptr);
      if (r.sampler) vkDestroySampler(dev(), r.sampler, nullptr);
      if (r.memory) vkFreeMemory(dev(), r.memory, nullptr);
    }
  }

  Pipeline& pipelineFor(uint32_t pass) {
    Pipeline& p = pipelines[pass];
    if (p.pipeline || p.failed) {
      return p;
    }
    const PassInfo& pi = info_(pass);
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (uint32_t i = 0; i < pi.bindingCount; ++i) {
      VkDescriptorSetLayoutBinding b {};
      b.binding = pi.bindings[i].slot;
      b.descriptorType = descriptorType(pi.bindings[i].kind);
      b.descriptorCount = 1;
      b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      lb.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo dl { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.bindingCount = uint32_t(lb.size());
    dl.pBindings = lb.data();
    check(vkCreateDescriptorSetLayout(dev(), &dl, nullptr, &p.setLayout), "vkCreateDescriptorSetLayout");
    VkPushConstantRange pr { VK_SHADER_STAGE_COMPUTE_BIT, 0, pi.pushConstantSize };
    VkPipelineLayoutCreateInfo pl { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p.setLayout;
    pl.pushConstantRangeCount = pi.pushConstantSize ? 1u : 0u;
    pl.pPushConstantRanges = &pr;
    check(vkCreatePipelineLayout(dev(), &pl, nullptr, &p.layout), "vkCreatePipelineLayout");

    const std::vector<uint32_t> code = pass < passCount()
      ? loadPassSpirv(PassId(pass), info.spirvDirectory) : external[pass - passCount()].spirv;
    if (code.empty()) {
      p.failed = true;
      return p;
    }
    VkShaderModuleCreateInfo sm { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    sm.codeSize = code.size() * sizeof(uint32_t);
    sm.pCode = code.data();
    VkShaderModule module;
    check(vkCreateShaderModule(dev(), &sm, nullptr, &module), "vkCreateShaderModule");
    VkComputePipelineCreateInfo cp { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cp.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    cp.layout = p.layout;
    const VkResult r = vkCreateComputePipelines(dev(), pipelineCache, 1, &cp, nullptr, &p.pipeline);
    vkDestroyShaderModule(dev(), module, nullptr);
    if (r != VK_SUCCESS) {
      log(LogLevel::Error, (std::string("NUMOS Vulkan: pipeline creation failed for ") + pi.name).c_str());
      p.failed = true;
    }
    return p;
  }
};

VulkanGpu::VulkanGpu(const VulkanGpuCreateInfo& info) : m(std::make_unique<Impl>()) {
  m->info = info;
  vkGetPhysicalDeviceMemoryProperties(info.physicalDevice, &m->memProps);
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(info.physicalDevice, &props);
  m->uboAlign = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
  m->pipelines.resize(passCount());
  // The cache's header names the device it was made on (Vulkan spec, "Pipeline Cache"). Data from
  // another device or driver is dropped here rather than left to the driver to reject.
  std::vector<uint8_t> cacheData = info.pipelineCacheData;
  if (cacheData.size() >= 16 + VK_UUID_SIZE) {
    uint32_t header[4];
    std::memcpy(header, cacheData.data(), sizeof(header));
    const bool sameDevice = header[1] == uint32_t(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
                         && header[2] == props.vendorID && header[3] == props.deviceID
                         && std::memcmp(cacheData.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    if (!sameDevice) {
      cacheData.clear();
    }
  } else {
    cacheData.clear();
  }
  VkPipelineCacheCreateInfo pcci { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
  pcci.initialDataSize = cacheData.size();
  pcci.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
  check(vkCreatePipelineCache(m->dev(), &pcci, nullptr, &m->pipelineCache), "vkCreatePipelineCache");
  m->frames.resize(std::max(info.framesInFlight, 1u));
  for (Impl::Frame& f : m->frames) {
    void* mapped = nullptr;
    f.ring = m->makeBuffer(info.constantRingBytes,
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, f.ringMemory, &mapped);
    f.ringMapped = static_cast<uint8_t*>(mapped);
    Impl::Resource r;
    r.kind = Impl::Kind::Buffer;
    r.buffer = f.ring;
    r.bdesc.size = info.constantRingBytes;
    r.imported = true;  // released explicitly in the destructor
    f.ringHandle = m->add(std::move(r));

    const VkDescriptorPoolSize sizes[] = {
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 },
      { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 4096 },
      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048 },
      { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 256 },
    };
    VkDescriptorPoolCreateInfo pci { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2048;
    pci.poolSizeCount = info.rayQuerySupported ? 7u : 6u;
    pci.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(m->dev(), &pci, nullptr, &f.pool), "vkCreateDescriptorPool");
  }
}

VulkanGpu::~VulkanGpu() {
  vkDeviceWaitIdle(m->dev());
  for (auto& p : m->pipelines) {
    if (p.pipeline) vkDestroyPipeline(m->dev(), p.pipeline, nullptr);
    if (p.layout) vkDestroyPipelineLayout(m->dev(), p.layout, nullptr);
    if (p.setLayout) vkDestroyDescriptorSetLayout(m->dev(), p.setLayout, nullptr);
  }
  vkDestroyPipelineCache(m->dev(), m->pipelineCache, nullptr);
  for (auto& f : m->frames) {
    vkDestroyDescriptorPool(m->dev(), f.pool, nullptr);
    if (f.queries != VK_NULL_HANDLE) {
      vkDestroyQueryPool(m->dev(), f.queries, nullptr);
    }
    vkDestroyBuffer(m->dev(), f.ring, nullptr);
    vkFreeMemory(m->dev(), f.ringMemory, nullptr);
  }
  for (auto& kv : m->resources) {
    m->release(kv.second);
  }
}

void VulkanGpu::beginFrame(VkCommandBuffer cmd, uint64_t frameIndex) {
  m->cmd = cmd;
  m->frameIndex = frameIndex;
  m->frame = &m->frames[frameIndex % m->frames.size()];
  // The host guarantees this slot's previous frame has completed.
  for (GpuHandle h : m->frame->pendingDestroy) {
    auto it = m->resources.find(h);
    if (it != m->resources.end()) {
      m->release(it->second);
      m->resources.erase(it);
    }
  }
  m->frame->pendingDestroy.clear();
  m->frame->ringOffset = 0;
  vkResetDescriptorPool(m->dev(), m->frame->pool, 0);
  if (m->frame->queries == VK_NULL_HANDLE) {
    VkQueryPoolCreateInfo qi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = Impl::kMaxZoneQueries;
    vkCreateQueryPool(m->dev(), &qi, nullptr, &m->frame->queries);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m->info.physicalDevice, &props);
    m->timestampPeriodNs = props.limits.timestampPeriod;
  }
  vkCmdResetQueryPool(cmd, m->frame->queries, 0, Impl::kMaxZoneQueries);
  m->frame->zones.clear();
  m->frame->queryCount = 0;
  m->zoneStack.clear();
}

void VulkanGpu::endFrame() {
  m->cmd = VK_NULL_HANDLE;
}

void VulkanGpu::beginZone(const char* name) {
  Impl::Frame& f = *m->frame;
  if (!m->cmd || f.queryCount + 2 > Impl::kMaxZoneQueries) {
    m->zoneStack.push_back(~0u);
    return;
  }
  f.zones.push_back({ name, uint32_t(m->zoneStack.size()), f.queryCount, 0 });
  vkCmdWriteTimestamp(m->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.queries, f.queryCount++);
  m->zoneStack.push_back(uint32_t(f.zones.size() - 1));
}

void VulkanGpu::endZone() {
  if (m->zoneStack.empty()) {
    return;
  }
  const uint32_t idx = m->zoneStack.back();
  m->zoneStack.pop_back();
  if (idx == ~0u) {
    return;
  }
  Impl::Frame& f = *m->frame;
  f.zones[idx].end = f.queryCount;
  vkCmdWriteTimestamp(m->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.queries, f.queryCount++);
}

std::vector<VulkanGpu::ZoneTiming> VulkanGpu::collectZoneTimings() {
  std::vector<ZoneTiming> out;
  Impl::Frame& f = *m->frame;
  if (f.queryCount == 0) {
    return out;
  }
  std::vector<uint64_t> ticks(f.queryCount);
  if (vkGetQueryPoolResults(m->dev(), f.queries, 0, f.queryCount, ticks.size() * sizeof(uint64_t),
                            ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return out;
  }
  for (const auto& z : f.zones) {
    if (z.end > z.begin) {
      out.push_back({ z.name, z.depth, double(ticks[z.end] - ticks[z.begin]) * m->timestampPeriodNs * 1e-6 });
    }
  }
  return out;
}

VkImage VulkanGpu::image(GpuHandle texture) const { return m->resources.at(texture).image; }
VkBuffer VulkanGpu::buffer(GpuHandle b) const { return m->resources.at(b).buffer; }
VkImageView VulkanGpu::view(GpuHandle texture, uint32_t mip) {
  return m->viewFor(texture, mip, false);
}

GpuHandle VulkanGpu::importTexture(VkImage image, const TextureDesc& desc) {
  Impl::Resource r;
  r.kind = Impl::Kind::Texture;
  r.image = image;
  r.tdesc = desc;
  r.isArray = desc.type == TextureType::Tex2DArray;
  r.format = toVkFormat(desc.format);
  r.imported = true;
  return m->add(std::move(r));
}

uint32_t VulkanGpu::registerPass(const PassInfo& info, std::vector<uint32_t> spirv) {
  m->external.push_back({ info, std::move(spirv) });
  m->pipelines.emplace_back();
  return passCount() + uint32_t(m->external.size()) - 1u;
}

void VulkanGpu::createPipelines(uint32_t threads,
                                const std::function<bool(uint32_t done, uint32_t total)>& progress) {
  std::vector<uint32_t> passes;
  for (uint32_t pass = 0; pass < uint32_t(m->pipelines.size()); ++pass) {
    const PassInfo& pi = m->info_(pass);
    bool needsRayQuery = false;
    for (uint32_t i = 0; i < pi.bindingCount; ++i) {
      needsRayQuery = needsRayQuery || pi.bindings[i].kind == BindingKind::AccelerationStructure;
    }
    if (!needsRayQuery || m->info.rayQuerySupported) {
      passes.push_back(pass);
    }
  }
  const uint32_t total = uint32_t(passes.size());
  std::atomic<uint32_t> next { 0 };
  std::atomic<uint32_t> done { 0 };
  std::atomic<bool> stop { false };
  std::exception_ptr error;
  std::mutex errorMutex;
  auto work = [&]() {
    try {
      for (uint32_t i = next++; i < total && !stop; i = next++) {
        m->pipelineFor(passes[i]);
        ++done;
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(errorMutex);
      if (!error) error = std::current_exception();
      stop = true;
    }
  };
  const uint32_t workerCount = std::min(std::max(threads, 1u) - 1u, total);
  std::atomic<uint32_t> running { workerCount };
  std::vector<std::thread> workers;
  for (uint32_t t = 0; t < workerCount; ++t) {
    workers.emplace_back([&]() {
      work();
      --running;
    });
  }
  if (workers.empty()) {
    // One thread: this one, with `progress` between pipelines.
    for (uint32_t i = 0; i < total && !stop; ++i) {
      if (progress && !progress(i, total)) {
        stop = true;
        break;
      }
      m->pipelineFor(passes[i]);
    }
  } else {
    // The workers compile; this thread reports progress until they have all finished, also after a
    // stop, while they complete the pipelines they started.
    while (running > 0) {
      if (progress && !progress(done, total)) {
        stop = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    for (std::thread& w : workers) w.join();
    if (error) std::rethrow_exception(error);
  }
  if (progress && !stop) {
    progress(total, total);
  }
}

std::vector<uint8_t> VulkanGpu::pipelineCacheData() const {
  size_t size = 0;
  if (vkGetPipelineCacheData(m->dev(), m->pipelineCache, &size, nullptr) != VK_SUCCESS) {
    return {};
  }
  std::vector<uint8_t> data(size);
  if (vkGetPipelineCacheData(m->dev(), m->pipelineCache, &size, data.data()) != VK_SUCCESS) {
    return {};
  }
  data.resize(size);
  return data;
}

GpuHandle VulkanGpu::createTexture(const TextureDesc& desc) {
  Impl::Resource r;
  r.kind = Impl::Kind::Texture;
  r.tdesc = desc;
  r.isArray = desc.type == TextureType::Tex2DArray;
  r.format = toVkFormat(desc.format);
  m->createImage(r);
  return m->add(std::move(r));
}

GpuHandle VulkanGpu::createBuffer(const BufferDesc& desc) {
  Impl::Resource r;
  r.kind = Impl::Kind::Buffer;
  r.bdesc = desc;
  void* mapped = nullptr;
  if (desc.usage == BufferUsage::Readback) {
    r.buffer = m->makeBuffer(desc.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, r.memory, &mapped);
  } else {
    r.buffer = m->makeBuffer(desc.size,
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, r.memory, nullptr);
  }
  r.mapped = mapped;
  return m->add(std::move(r));
}

GpuHandle VulkanGpu::createSampler(const SamplerDesc& desc) {
  auto address = [](AddressMode a) {
    return a == AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  };
  VkSamplerCreateInfo si { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
  si.magFilter = si.minFilter = desc.filter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.mipmapMode = desc.mipFilter == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = address(desc.addressU);
  si.addressModeV = address(desc.addressV);
  si.addressModeW = address(desc.addressW);
  si.maxLod = desc.maxLod >= SamplerDesc::kLodClampNone ? VK_LOD_CLAMP_NONE : desc.maxLod;
  Impl::Resource r;
  r.kind = Impl::Kind::Sampler;
  check(vkCreateSampler(m->dev(), &si, nullptr, &r.sampler), "vkCreateSampler");
  return m->add(std::move(r));
}

void VulkanGpu::destroy(GpuHandle resource) {
  if (resource == 0) {
    return;
  }
  if (m->frame) {
    m->frame->pendingDestroy.push_back(resource);
  } else {
    auto it = m->resources.find(resource);
    if (it != m->resources.end()) {
      m->release(it->second);
      m->resources.erase(it);
    }
  }
}

IGpu::ConstantsRegion VulkanGpu::uploadConstants(const void* data, size_t size) {
  Impl::Frame& f = *m->frame;
  const size_t offset = (f.ringOffset + m->uboAlign - 1) & ~(size_t(m->uboAlign) - 1);
  if (offset + size > m->info.constantRingBytes) {
    throw std::runtime_error("NUMOS Vulkan: constant ring exhausted (raise constantRingBytes)");
  }
  std::memcpy(f.ringMapped + offset, data, size);
  f.ringOffset = offset + size;
  return { f.ringHandle, offset, size };
}

void VulkanGpu::updateBuffer(GpuHandle buffer, size_t offset, size_t size, const void* data) {
  Impl::Resource& r = m->get(buffer);
  m->fullBarrier();
  if (size <= 65536 && size % 4 == 0) {
    vkCmdUpdateBuffer(m->cmd, r.buffer, offset, size, data);
  } else {
    const ConstantsRegion staging = uploadConstants(data, size);
    const VkBufferCopy copy { staging.offset, offset, size };
    vkCmdCopyBuffer(m->cmd, m->frame->ring, r.buffer, 1, &copy);
  }
  m->fullBarrier();
}

void VulkanGpu::copyBuffer(GpuHandle dst, size_t dstOffset, GpuHandle src, size_t srcOffset, size_t size) {
  Impl::Resource& d = m->get(dst);
  const VkBufferCopy copy { srcOffset, dstOffset, size };
  vkCmdCopyBuffer(m->cmd, m->get(src).buffer, d.buffer, 1, &copy);
  d.copyFrame = m->frameIndex + 1;
  m->fullBarrier();
}

void VulkanGpu::copyTexture(GpuHandle dst, GpuHandle src) {
  Impl::Resource& s = m->get(src);
  Impl::Resource& d = m->get(dst);
  std::vector<VkImageCopy> regions;
  for (uint32_t mip = 0; mip < s.tdesc.mipLevels; ++mip) {
    VkImageCopy c {};
    c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, s.isArray ? s.tdesc.depth : 1u };  // every layer
    c.dstSubresource = c.srcSubresource;
    c.extent = { std::max(s.tdesc.width >> mip, 1u), std::max(s.tdesc.height >> mip, 1u),
                 s.tdesc.type == TextureType::Tex3D ? std::max(s.tdesc.depth >> mip, 1u) : 1u };
    regions.push_back(c);
  }
  vkCmdCopyImage(m->cmd, s.image, VK_IMAGE_LAYOUT_GENERAL, d.image, VK_IMAGE_LAYOUT_GENERAL,
                 uint32_t(regions.size()), regions.data());
  m->fullBarrier();
}

void VulkanGpu::clearTexture(GpuHandle texture, const float rgba[4]) {
  m->clear(texture, rgba);
}

void VulkanGpu::uploadTexture(GpuHandle texture, uint32_t mip, const void* data, size_t size) {
  Impl::Resource& r = m->get(texture);
  const ConstantsRegion staging = uploadConstants(data, size);
  VkBufferImageCopy copy {};
  copy.bufferOffset = staging.offset;
  copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, r.isArray ? r.tdesc.depth : 1u };
  copy.imageExtent = { std::max(r.tdesc.width >> mip, 1u), std::max(r.tdesc.height >> mip, 1u),
                       r.tdesc.type == TextureType::Tex3D ? std::max(r.tdesc.depth >> mip, 1u) : 1u };
  m->fullBarrier();
  vkCmdCopyBufferToImage(m->cmd, m->frame->ring, r.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
  m->fullBarrier();
}

void VulkanGpu::dispatch(const DispatchDesc& desc) {
  Impl::Pipeline& p = m->pipelineFor(desc.pass);
  if (p.failed) {
    return;
  }
  const PassInfo& pi = m->info_(desc.pass);

  VkDescriptorSetAllocateInfo ai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
  ai.descriptorPool = m->frame->pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &p.setLayout;
  VkDescriptorSet set;
  check(vkAllocateDescriptorSets(m->dev(), &ai, &set), "vkAllocateDescriptorSets");

  std::vector<VkWriteDescriptorSet> writes(desc.bindingCount);
  std::vector<VkDescriptorImageInfo> images(desc.bindingCount);
  std::vector<VkDescriptorBufferInfo> buffers(desc.bindingCount);
  std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accels(desc.bindingCount);
  std::vector<VkAccelerationStructureKHR> accelHandles(desc.bindingCount);
  uint32_t count = 0;
  for (uint32_t i = 0; i < desc.bindingCount; ++i) {
    const Binding& b = desc.bindings[i];
    const PassBindingInfo& decl = pi.bindings[i];
    VkWriteDescriptorSet& w = writes[count];
    w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = b.slot;
    w.descriptorCount = 1;
    w.descriptorType = descriptorType(b.kind);
    switch (b.kind) {
    case BindingKind::ConstantBuffer:
    case BindingKind::StructuredBuffer:
    case BindingKind::RWStructuredBuffer: {
      if (b.resource == 0) {
        if (!m->dummyBuffer) {
          BufferDesc bd;
          bd.usage = BufferUsage::Structured;
          bd.size = 65536;
          m->dummyBuffer = createBuffer(bd);
        }
      }
      const GpuHandle h = b.resource ? b.resource : m->dummyBuffer;
      Impl::Resource& r = m->get(h);
      const size_t size = b.size == ~size_t(0) ? (r.bdesc.size - b.offset) : b.size;
      buffers[i] = { r.buffer, b.offset, b.resource ? size : VK_WHOLE_SIZE };
      w.pBufferInfo = &buffers[i];
      break;
    }
    case BindingKind::Sampler: {
      if (!m->dummySampler) {
        m->dummySampler = createSampler(SamplerDesc {});
      }
      images[i].sampler = m->get(b.resource ? b.resource : m->dummySampler).sampler;
      w.pImageInfo = &images[i];
      break;
    }
    case BindingKind::SampledTexture:
    case BindingKind::StorageTexture:
    case BindingKind::CombinedTextureSampler: {
      const bool storage = b.kind == BindingKind::StorageTexture;
      const GpuHandle h = b.resource ? b.resource : m->dummyTexture(decl.dim, storage, decl.spirvImageFormat);
      const bool single = storage || b.mipCount == 1;
      images[i].imageView = m->viewFor(h, single ? b.mipLevel : ~0u, decl.dim == BindingDim::Tex2DArray);
      images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (b.kind == BindingKind::CombinedTextureSampler) {
        if (!m->dummySampler) {
          m->dummySampler = createSampler(SamplerDesc {});
        }
        images[i].sampler = m->get(b.sampler ? b.sampler : m->dummySampler).sampler;
      }
      w.pImageInfo = &images[i];
      break;
    }
    case BindingKind::AccelerationStructure: {
      accelHandles[i] = reinterpret_cast<VkAccelerationStructureKHR>(b.resource);
      accels[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
      accels[i].accelerationStructureCount = 1;
      accels[i].pAccelerationStructures = &accelHandles[i];
      w.pNext = &accels[i];
      break;
    }
    }
    ++count;
  }
  vkUpdateDescriptorSets(m->dev(), count, writes.data(), 0, nullptr);

  vkCmdBindPipeline(m->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
  vkCmdBindDescriptorSets(m->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
  if (pi.pushConstantSize) {
    std::vector<uint8_t> push(pi.pushConstantSize, 0);
    if (desc.pushConstants) {
      std::memcpy(push.data(), desc.pushConstants, std::min(desc.pushConstantSize, pi.pushConstantSize));
    }
    vkCmdPushConstants(m->cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pi.pushConstantSize, push.data());
  }
  vkCmdDispatch(m->cmd, desc.groupsX, desc.groupsY, desc.groupsZ);
  m->fullBarrier();
}

void VulkanGpu::barrier() {
  m->fullBarrier();
}

bool VulkanGpu::readBuffer(GpuHandle readback, void* dst, size_t size) {
  Impl::Resource& r = m->get(readback);
  // copyFrame is frameIndex + 1 of the copy; that frame has completed once framesInFlight newer
  // frames have begun (the host waited on its fence before reusing its slot).
  if (r.copyFrame == 0 || m->frameIndex < r.copyFrame - 1 + m->frames.size()) {
    return false;
  }
  std::memcpy(dst, r.mapped, std::min(size, r.bdesc.size));
  return true;
}

} // namespace numos::vulkan
