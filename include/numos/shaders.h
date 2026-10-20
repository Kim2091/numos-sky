// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The NUMOS compute passes: one entry per shader variant, generated from the built SPIR-V
// (tools/gen_pass_table.py). A host creates one compute pipeline per pass from its SPIR-V and
// binding list, then IGpu::dispatch receives the pass index.

#include <cstdint>
#include <string>
#include <vector>

#include "numos/gpu.h"

namespace numos {

enum class PassId : uint32_t {
#define NUMOS_PASS(name, file, hash, lx, ly, lz, push, bBegin, bCount) name,
#include "../../src/generated/passes.inc"
  Count
};

// Texture dimensionality a binding declares (None for buffers, samplers, acceleration structures).
enum class BindingDim : uint8_t { None, Tex2D, Tex3D, Tex2DArray };

struct PassBindingInfo {
  uint32_t    slot;
  uint32_t    set;
  BindingKind kind;
  BindingDim  dim;
  uint32_t    spirvImageFormat;  // SPIR-V ImageFormat of a storage image (0 = unknown/none)
  const char* shaderVariable;
};

struct PassInfo {
  const char*            name;
  const char*            spirvFile;      // file name inside the SPIR-V directory
  uint64_t               spirvHash;      // FNV-1a 64 of the file, to detect stale builds
  uint32_t               localSize[3];
  uint32_t               pushConstantSize;
  const PassBindingInfo* bindings;       // declaration order; DispatchDesc::bindings follows it 1:1
  uint32_t               bindingCount;
};

const PassInfo& passInfo(PassId pass);
// `pass` must be < passCount() (not checked). The table is static and immutable: any thread.
inline const PassInfo& passInfo(uint32_t pass) { return passInfo(PassId(pass)); }
constexpr uint32_t passCount() { return uint32_t(PassId::Count); }

// Loads a pass's SPIR-V from `spirvDirectory` and checks its hash. Returns an empty vector (and
// logs) when the file is missing or does not match the table, i.e. shaders and library disagree.
// `spirvDirectory` is a narrow (on Windows: ANSI code page) path without a trailing separator;
// empty means the working directory. Thread-safe; it only reads the file.
std::vector<uint32_t> loadPassSpirv(PassId pass, const std::string& spirvDirectory);

} // namespace numos
