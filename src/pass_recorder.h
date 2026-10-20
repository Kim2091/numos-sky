// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Internal: a slot-based binding model on top of numos::IGpu. Resources are bound by slot and stay
// bound until changed; setPass picks the pass, and dispatch resolves the pass's declared bindings
// from the current slots.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "numos/gpu.h"
#include "numos/shaders.h"

namespace numos {

// A texture NUMOS owns.
struct Texture {
  GpuHandle   handle = 0;
  TextureDesc desc {};
  bool isValid() const { return handle != 0; }
};

// A buffer NUMOS owns.
struct Buffer {
  GpuHandle  handle = 0;
  BufferDesc desc {};
  bool isValid() const { return handle != 0; }
};

class PassRecorder {
public:
  explicit PassRecorder(IGpu& gpu) : m_gpu(gpu) {}

  IGpu& gpu() { return m_gpu; }

  // Texture at a slot: sampled or storage, decided by the pass's declaration at dispatch.
  // `mip` selects a single-level view (storage writes, mip-chain passes); ~0u means all mips.
  void bindTexture(uint32_t slot, GpuHandle texture, uint32_t mip = ~0u) {
    Slot& s = m_slots[slot];
    s.resource = texture;
    s.mip = mip;
  }
  void bindTexture(uint32_t slot, const Texture& texture, uint32_t mip = ~0u) {
    bindTexture(slot, texture.handle, mip);
  }
  void bindSampler(uint32_t slot, GpuHandle sampler) { m_slots[slot].sampler = sampler; }
  void bindBuffer(uint32_t slot, GpuHandle buffer, size_t offset = 0, size_t size = ~size_t(0)) {
    Slot& s = m_slots[slot];
    s.resource = buffer;
    s.offset = offset;
    s.size = size;
  }
  void bindBuffer(uint32_t slot, const Buffer& buffer) { bindBuffer(slot, buffer.handle); }
  void bindAccelerationStructure(uint32_t slot, GpuHandle tlas) { m_slots[slot].resource = tlas; }
  // Uploads per-dispatch constants and binds them.
  void bindConstants(uint32_t slot, const void* data, size_t size) {
    const IGpu::ConstantsRegion r = m_gpu.uploadConstants(data, size);
    bindBuffer(slot, r.buffer, r.offset, r.size);
  }
  void unbind(uint32_t slot) { m_slots[slot] = Slot {}; }

  void setPass(PassId pass) { m_pass = pass; }
  void pushConstants(const void* data, uint32_t size) {
    m_push.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  }

  void dispatch(uint32_t x, uint32_t y, uint32_t z) {
    const PassInfo& info = passInfo(m_pass);
    m_bindings.clear();
    for (uint32_t i = 0; i < info.bindingCount; ++i) {
      const PassBindingInfo& decl = info.bindings[i];
      const auto it = m_slots.find(decl.slot);
      const Slot s = it == m_slots.end() ? Slot {} : it->second;
      Binding b;
      b.slot = decl.slot;
      b.kind = decl.kind;
      if (decl.kind == BindingKind::Sampler) {
        b.resource = s.sampler;
      } else {
        b.resource = s.resource;
        b.sampler = decl.kind == BindingKind::CombinedTextureSampler ? s.sampler : 0;
      }
      if (s.mip != ~0u) {
        b.mipLevel = s.mip;
        b.mipCount = 1;
      }
      b.offset = s.offset;
      b.size = s.size;
      m_bindings.push_back(b);
    }
    DispatchDesc d;
    d.pass = uint32_t(m_pass);
    d.bindings = m_bindings.data();
    d.bindingCount = uint32_t(m_bindings.size());
    d.pushConstants = m_push.empty() ? nullptr : m_push.data();
    d.pushConstantSize = info.pushConstantSize ? uint32_t(m_push.size()) : 0u;
    d.groupsX = x;
    d.groupsY = y;
    d.groupsZ = z;
    m_gpu.dispatch(d);
  }

  void barrier() { m_gpu.barrier(); }

private:
  struct Slot {
    GpuHandle resource = 0;
    GpuHandle sampler = 0;
    uint32_t  mip = ~0u;
    size_t    offset = 0;
    size_t    size = ~size_t(0);
  };

  IGpu& m_gpu;
  std::unordered_map<uint32_t, Slot> m_slots;
  PassId m_pass = PassId(0);
  std::vector<uint8_t> m_push;
  std::vector<Binding> m_bindings;
};

} // namespace numos
