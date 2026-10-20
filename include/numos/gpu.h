// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The GPU interface a host implements for NUMOS (DESIGN.md: "GPU access").
//
// NUMOS only needs compute: textures (2D/3D, with mips), buffers, samplers, constant uploads,
// compute dispatches and barriers. The host maps these onto its API. backends/vulkan has a
// reference implementation; Remix maps them onto DxvkContext.
//
// Ordering contract: calls are recorded in order into the host's current command stream.
// `barrier()` is a full compute-to-compute memory barrier. Hosts that track hazards automatically
// (DXVK) may also rely on the declared accesses; hosts that do not must at least honour
// `barrier()` and insert transitions for the declared accesses.
//  - Access is implied by BindingKind: StorageTexture and RWStructuredBuffer may write (and read);
//    every other kind is read-only. There are no separate access flags.
//  - Between two dispatches NUMOS emits barrier() for every read-after-write. Back-to-back
//    dispatches of cloud_shadow_map write disjoint rows/slices of the same textures without a
//    barrier in between.
//  - NUMOS emits no barrier around updateBuffer, copyBuffer, copyTexture, clearTexture and
//    uploadTexture: the host orders each of them against the dispatches before and after it.
//  - The same texture can be sampled by one dispatch and storage-written by the next; the host owns
//    image layouts (the reference backend keeps every image in GENERAL).
//
// Threading: NUMOS calls an IGpu only from inside Renderer::beginFrame / dispatchPostDepth /
// releaseGpuResources, synchronously on the thread that called them; it never calls from a thread of
// its own. An IGpu needs no locking of its own unless the host shares it between threads.

#include <cstddef>
#include <cstdint>

namespace numos {

// Opaque host handle. 0 is "none". NUMOS never interprets the value.
using GpuHandle = uint64_t;

enum class Format : uint8_t {
  R8Unorm,
  RGBA8Unorm,
  R16Float,
  RGBA16Float,
  R32Uint,
  R32Float,
  RG32Float,
  RGBA32Float,
  RG16Float,
};

enum class TextureType : uint8_t { Tex2D, Tex3D, Tex2DArray };  // Tex2DArray: `depth` = layer count

struct TextureDesc {
  TextureType type = TextureType::Tex2D;
  Format      format = Format::RGBA16Float;
  uint32_t    width = 1;
  uint32_t    height = 1;
  uint32_t    depth = 1;          // 1 for 2D
  uint32_t    mipLevels = 1;
  const char* debugName = nullptr;
  // Always sampled and storage-writable by compute. Hosts may add more usage.
};

enum class BufferUsage : uint8_t {
  Constant,     // uniform/constant buffer, updated with updateBuffer
  Structured,   // read-only or read-write structured buffer
  Readback,     // GPU writes via copyBuffer, CPU reads via readBuffer
};

struct BufferDesc {
  BufferUsage usage = BufferUsage::Constant;
  size_t      size = 0;
  const char* debugName = nullptr;
};

enum class Filter : uint8_t { Nearest, Linear };
enum class AddressMode : uint8_t { Repeat, ClampToEdge };

struct SamplerDesc {
  Filter      filter = Filter::Linear;       // min and mag
  Filter      mipFilter = Filter::Nearest;
  AddressMode addressU = AddressMode::ClampToEdge;
  AddressMode addressV = AddressMode::ClampToEdge;
  AddressMode addressW = AddressMode::ClampToEdge;
  // Highest mip the sampler may read. 0 matches a zero-initialised DXVK sampler (mip 0 only);
  // kLodClampNone allows the whole chain (VK_LOD_CLAMP_NONE).
  float       maxLod = 0.0f;
  static constexpr float kLodClampNone = 1000.0f;
};

enum class BindingKind : uint8_t {
  ConstantBuffer,
  SampledTexture,     // Texture2D/Texture3D (read)
  StorageTexture,     // RWTexture2D/RWTexture3D (write or read-write)
  Sampler,
  CombinedTextureSampler, // Sampler2D: `resource` is the texture, `sampler` the sampler
  StructuredBuffer,   // read
  RWStructuredBuffer, // write or read-write
  AccelerationStructure,
};

// One resource bound for a dispatch, at the slot the pass's shader declares.
struct Binding {
  uint32_t    slot = 0;
  BindingKind kind = BindingKind::SampledTexture;
  // Texture, buffer, sampler or acceleration structure. 0 = unbound: the pass declares it but does
  // not read it in this configuration; the host binds a placeholder (a zeroed texture of the declared
  // BindingDim / storage format, a small zeroed buffer, a default sampler), as the reference backend
  // does. AccelerationStructure is never 0: NUMOS dispatches the TLAS-tracing passes only with
  // FrameInputs::opaqueTlas set, and binds that host handle as given.
  GpuHandle   resource = 0;
  GpuHandle   sampler = 0;      // CombinedTextureSampler only
  uint32_t    mipLevel = 0;     // textures: the single mip to view (StorageTexture always one mip)
  uint32_t    mipCount = ~0u;   // SampledTexture: mips visible from mipLevel (NUMOS uses 1, or ~0u = all)
  size_t      offset = 0;       // buffers
  size_t      size = ~size_t(0);// buffers (~0 = to the end)
};

struct DispatchDesc {
  uint32_t       pass = 0;            // pass index, < passCount() (see passInfo() in shaders.h)
  // One binding per declaration of the pass, in passInfo(pass).bindings order (bindingCount equal
  // to the pass's). Valid only during the dispatch call.
  const Binding* bindings = nullptr;
  uint32_t       bindingCount = 0;
  // Only for a pass with PassInfo::pushConstantSize > 0. The size can differ from the pass's: copy
  // min(both) bytes and zero the rest, as the reference backend does.
  const void*    pushConstants = nullptr;
  uint32_t       pushConstantSize = 0;
  uint32_t       groupsX = 1;
  uint32_t       groupsY = 1;
  uint32_t       groupsZ = 1;
};

class IGpu {
public:
  virtual ~IGpu() = default;

  virtual GpuHandle createTexture(const TextureDesc& desc) = 0;
  virtual GpuHandle createBuffer(const BufferDesc& desc) = 0;
  virtual GpuHandle createSampler(const SamplerDesc& desc) = 0;
  // Deferred destruction is the host's job: the resource may still be in flight, or bound by a
  // dispatch recorded earlier in this frame. NUMOS destroys only handles it created, each once, never 0.
  virtual void destroy(GpuHandle resource) = 0;

  // Per-dispatch constants. Copies `size` bytes into host-managed transient memory and returns a
  // region valid until the end of the current frame; each call gets its own region, so several
  // dispatches in one frame can each see different constants (a linear ring buffer is enough).
  // Bind the result with BindingKind::ConstantBuffer (resource/offset/size from the region). The
  // offset must meet the host's constant-buffer alignment. `buffer` is the host's: NUMOS never
  // destroys it.
  struct ConstantsRegion {
    GpuHandle buffer = 0;
    size_t    offset = 0;
    size_t    size = 0;
  };
  virtual ConstantsRegion uploadConstants(const void* data, size_t size) = 0;

  // Records a buffer update into the command stream, ordered with dispatches. Later dispatches see
  // the new contents; the host must keep earlier, still-recorded dispatches correct (e.g. by a
  // barrier before the copy, as for any write-after-read). `data` is only valid during the call;
  // offset and size need not be multiples of 4, and size can reach 1 MB (night sky streaming).
  virtual void updateBuffer(GpuHandle buffer, size_t offset, size_t size, const void* data) = 0;
  virtual void copyBuffer(GpuHandle dst, size_t dstOffset, GpuHandle src, size_t srcOffset, size_t size) = 0;
  // Whole-texture copy (same desc): every mip and layer. NUMOS does not call it at present.
  virtual void copyTexture(GpuHandle dst, GpuHandle src) = 0;
  // Every mip and layer. NUMOS clears each texture it creates to zero right after creating it.
  virtual void clearTexture(GpuHandle texture, const float rgba[4]) = 0;
  // Uploads tightly packed texel data for one mip (all layers / depth slices), recorded in order.
  // `data` is only valid during the call. Single uploads reach a few MB (night sky maps).
  virtual void uploadTexture(GpuHandle texture, uint32_t mip, const void* data, size_t size) = 0;

  virtual void dispatch(const DispatchDesc& desc) = 0;
  // Full compute-to-compute memory barrier.
  virtual void barrier() = 0;

  // Readback buffers: returns true and copies `size` bytes once the GPU finished the copy recorded
  // by the most recent copyBuffer into it; false while still in flight (never blocks).
  virtual bool readBuffer(GpuHandle readback, void* dst, size_t size) = 0;

  // Optional profiling markers.
  virtual void beginZone(const char* /*name*/) {}
  virtual void endZone() {}
};

// RAII profiling zone.
class GpuZone {
public:
  GpuZone(IGpu& gpu, const char* name) : m_gpu(gpu) { m_gpu.beginZone(name); }
  ~GpuZone() { m_gpu.endZone(); }
  GpuZone(const GpuZone&) = delete;
  GpuZone& operator=(const GpuZone&) = delete;
private:
  IGpu& m_gpu;
};

} // namespace numos
