// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Reference numos::IGpu implementation on Vulkan 1.3.
//
// Deliberately simple, aiming at correctness over speed: every image lives in VK_IMAGE_LAYOUT_GENERAL,
// and a full compute/transfer barrier follows every command. Hosts with their own render graph
// should implement IGpu themselves; this one backs the NUMOS viewer and tests.
//
// Frame protocol (host side):
//   gpu.beginFrame(cmd, frameIndex);   // cmd is recording; frame (frameIndex - framesInFlight) has completed
//   renderer.beginFrame(gpu, inputs); ... renderer.dispatchPostDepth(gpu, ...);
//   gpu.endFrame();                    // host submits cmd

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include <vector>

#include "numos/gpu.h"
#include "numos/shaders.h"

namespace numos::vulkan {

struct VulkanGpuCreateInfo {
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice         device = VK_NULL_HANDLE;
  std::string      spirvDirectory;           // build/spirv
  uint32_t         framesInFlight = 2;
  size_t           constantRingBytes = 8u << 20;  // per frame in flight
  bool             rayQuerySupported = false;     // enables the TLAS-traced aerial-perspective variants
  // Initial contents of the pipeline cache: pipelineCacheData() from an earlier run. Data from another
  // device or driver is ignored.
  std::vector<uint8_t> pipelineCacheData;
};

class VulkanGpu final : public IGpu {
public:
  explicit VulkanGpu(const VulkanGpuCreateInfo& info);
  ~VulkanGpu() override;

  void beginFrame(VkCommandBuffer cmd, uint64_t frameIndex);
  void endFrame();

  // GPU time per zone (GpuZone / beginZone), from timestamp queries. collectZoneTimings reads the
  // frame most recently recorded; call it only once that frame's work has completed.
  struct ZoneTiming { std::string name; uint32_t depth; double ms; };
  std::vector<ZoneTiming> collectZoneTimings();

  // Host access to resources created through this IGpu.
  VkImage     image(GpuHandle texture) const;
  VkImageView view(GpuHandle texture, uint32_t mip = ~0u);
  VkBuffer    buffer(GpuHandle buffer) const;
  // Registers a host-owned image (e.g. the viewer's depth target) so NUMOS can bind it. The image
  // must be in VK_IMAGE_LAYOUT_GENERAL whenever NUMOS uses it.
  GpuHandle   importTexture(VkImage image, const TextureDesc& desc);
  // Registers a host compute pass (same table format as NUMOS's, e.g. from tools/gen_pass_table.py)
  // so it can go through dispatch() too. Returns its pass index (>= numos::passCount()).
  uint32_t    registerPass(const PassInfo& info, std::vector<uint32_t> spirv);

  // Creates the pipeline of every pass registered so far, instead of at each pass's first dispatch.
  // With `threads` > 1, threads - 1 worker threads compile while the calling thread runs `progress`
  // (number done, total) about every 15 ms, so a windowed host can keep its window responsive; with 1
  // the calling thread compiles and runs `progress` between pipelines. `progress` returning false
  // stops early; with workers it keeps being called until they finish the pipelines they started.
  // Passes that need ray queries are skipped unless rayQuerySupported is set. No other call on this
  // object may run meanwhile.
  void createPipelines(uint32_t threads, const std::function<bool(uint32_t done, uint32_t total)>& progress = {});
  // The pipeline cache's contents, to save and pass back as VulkanGpuCreateInfo::pipelineCacheData.
  std::vector<uint8_t> pipelineCacheData() const;

  // IGpu
  GpuHandle createTexture(const TextureDesc& desc) override;
  GpuHandle createBuffer(const BufferDesc& desc) override;
  GpuHandle createSampler(const SamplerDesc& desc) override;
  void destroy(GpuHandle resource) override;
  ConstantsRegion uploadConstants(const void* data, size_t size) override;
  void updateBuffer(GpuHandle buffer, size_t offset, size_t size, const void* data) override;
  void copyBuffer(GpuHandle dst, size_t dstOffset, GpuHandle src, size_t srcOffset, size_t size) override;
  void copyTexture(GpuHandle dst, GpuHandle src) override;
  void clearTexture(GpuHandle texture, const float rgba[4]) override;
  void uploadTexture(GpuHandle texture, uint32_t mip, const void* data, size_t size) override;
  void dispatch(const DispatchDesc& desc) override;
  void barrier() override;
  bool readBuffer(GpuHandle readback, void* dst, size_t size) override;
  void beginZone(const char* name) override;
  void endZone() override;

  struct Impl;
private:
  std::unique_ptr<Impl> m;
};

// VkFormat for a numos::Format.
VkFormat toVkFormat(Format format);

} // namespace numos::vulkan
