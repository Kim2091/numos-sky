// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "numos/shaders.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include "numos/log.h"

// A host that post-processes the SPIR-V (an optimiser pass, stripped debug names) defines
// NUMOS_PASS_TABLE_FILE as the quoted path of a table generated from its own SPIR-V, so the
// hashes and variable names match the files it ships. The pass list itself must not differ.
#ifndef NUMOS_PASS_TABLE_FILE
#define NUMOS_PASS_TABLE_FILE "generated/passes.inc"
#endif

namespace numos {

namespace {

const PassBindingInfo kBindings[] = {
#define NUMOS_PASS_BINDING(slot, set, kind, dim, fmt, var) { slot, set, BindingKind::kind, BindingDim::dim, fmt, var },
#include NUMOS_PASS_TABLE_FILE
};

const PassInfo kPasses[] = {
#define NUMOS_PASS(name, file, hash, lx, ly, lz, push, bBegin, bCount) \
  { #name, file, hash, { lx, ly, lz }, push, kBindings + (bBegin), bCount },
#include NUMOS_PASS_TABLE_FILE
};

static_assert(sizeof(kPasses) / sizeof(kPasses[0]) == size_t(PassId::Count), "pass table and PassId disagree");

uint64_t fnv1a(const unsigned char* data, size_t size) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < size; ++i) {
    h = (h ^ data[i]) * 0x100000001b3ull;
  }
  return h;
}

} // namespace

const PassInfo& passInfo(PassId pass) {
  return kPasses[uint32_t(pass)];
}

std::vector<uint32_t> loadPassSpirv(PassId pass, const std::string& spirvDirectory) {
  const PassInfo& info = passInfo(pass);
  // An empty directory means the working directory, not the drive root ("/file.spv").
  const std::string path = spirvDirectory.empty() ? std::string(info.spirvFile) : spirvDirectory + "/" + info.spirvFile;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log(LogLevel::Error, ("NUMOS: missing shader " + path).c_str());
    return {};
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.size() % 4 != 0 || fnv1a(bytes.data(), bytes.size()) != info.spirvHash) {
    log(LogLevel::Error, ("NUMOS: shader " + path + " does not match this build of the library (rebuild shaders "
                          "and the pass table together)").c_str());
    return {};
  }
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), bytes.size());
  return words;
}

} // namespace numos
