// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Seeds (seed.h): a whole configuration as one string.
#include "numos/seed.h"

#include <cstring>
#include <vector>

namespace numos {

namespace {

constexpr char     kPrefix[] = "NUMOS1-";
constexpr uint8_t  kVersion = 1;
constexpr uint8_t  kHasWeather = 1u << 0;
constexpr uint8_t  kHasSimulation = 1u << 1;

uint32_t hashName(std::string_view s) {  // FNV-1a
  uint32_t h = 2166136261u;
  for (char c : s) {
    h = (h ^ uint8_t(c)) * 16777619u;
  }
  return h;
}

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) {
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

size_t valueSize(SettingType t) {
  switch (t) {
  case SettingType::Bool: return 1;
  case SettingType::Vec3: return 12;
  default:                return 4;
  }
}

struct Writer {
  std::vector<uint8_t> b;
  void raw(const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { raw(&v, 2); }  // little-endian hosts only (x86/ARM)
  void u32(uint32_t v) { raw(&v, 4); }
  void f32(float v) { raw(&v, 4); }
  void f64(double v) { raw(&v, 8); }
};

struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool raw(void* out, size_t k) {
    if (!ok || at + k > n) {
      ok = false;
      return false;
    }
    std::memcpy(out, p + at, k);
    at += k;
    return true;
  }
  template <class T> T get() { T v {}; raw(&v, sizeof(T)); return v; }
};

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url(const std::vector<uint8_t>& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0u)
                     | (i + 2 < in.size() ? uint32_t(in[i + 2]) : 0u);
    const size_t chars = i + 2 < in.size() ? 4 : i + 1 < in.size() ? 3 : 2;
    for (size_t k = 0; k < chars; ++k) {
      out += kAlphabet[(v >> (18 - 6 * k)) & 63u];
    }
  }
  return out;
}

bool unbase64url(std::string_view in, std::vector<uint8_t>& out) {
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    const char* at = std::strchr(kAlphabet, c);
    if (c == '\0' || !at) {
      return false;
    }
    acc = (acc << 6) | uint32_t(at - kAlphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t(acc >> bits));
    }
  }
  return true;
}

} // namespace

std::string encodeSeed(const Settings& settings, const WeatherSnapshot* weather, const SimulationState* simulation) {
  Writer w;
  w.u8(kVersion);
  w.u8(uint8_t((weather ? kHasWeather : 0u) | (simulation ? kHasSimulation : 0u)));
  if (simulation) {
    w.f32(simulation->cloudAdvectOffset[0]);
    w.f32(simulation->cloudAdvectOffset[1]);
    w.f32(simulation->cloudEvolutionOffset[0]);
    w.f32(simulation->cloudEvolutionOffset[1]);
    w.f32(simulation->timeOfDayHours);
    w.f64(simulation->animationSeconds);
  }
  if (weather) {
    // Only the fields that differ from the weather the settings themselves describe.
    const WeatherSnapshot own = weatherFromSettings(settings);
    size_t count = 0;
    const WeatherFieldInfo* fields = weatherFields(count);
    std::vector<size_t> changed;
    for (size_t i = 0; i < count; ++i) {
      if (weather->*fields[i].member != own.*fields[i].member) {
        changed.push_back(i);
      }
    }
    w.u8(uint8_t(changed.size()));
    for (size_t i : changed) {
      w.u32(hashName(fields[i].name));
      w.f32(weather->*fields[i].member);
    }
  }
  const Settings defaults;
  const uint8_t* now = reinterpret_cast<const uint8_t*>(&settings);
  const uint8_t* def = reinterpret_cast<const uint8_t*>(&defaults);
  std::vector<const SettingInfo*> changed;
  for (const SettingInfo& info : settingInfos()) {
    if ((info.flags & NUMOS_SETTING_NO_SAVE) == 0
        && std::memcmp(now + info.offset, def + info.offset, valueSize(info.type)) != 0) {
      changed.push_back(&info);
    }
  }
  w.u16(uint16_t(changed.size()));
  for (const SettingInfo* info : changed) {
    w.u32(hashName(info->key));
    w.u8(uint8_t(info->type));
    w.raw(now + info->offset, valueSize(info->type));
  }
  w.u32(crc32(w.b.data(), w.b.size()));
  return kPrefix + base64url(w.b);
}

bool decodeSeed(std::string_view seed, SeedContents& out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) {
      *error = why;
    }
    return false;
  };
  while (!seed.empty() && (seed.front() == ' ' || seed.front() == '\t')) seed.remove_prefix(1);
  while (!seed.empty() && (seed.back() == ' ' || seed.back() == '\t' || seed.back() == '\r' || seed.back() == '\n')) {
    seed.remove_suffix(1);
  }
  const size_t prefixLength = sizeof(kPrefix) - 1;
  if (seed.substr(0, prefixLength) != kPrefix) {
    return fail("not a NUMOS seed (it starts with NUMOS1-)");
  }
  std::vector<uint8_t> b;
  if (!unbase64url(seed.substr(prefixLength), b) || b.size() < 8) {
    return fail("the seed is damaged (bad characters or too short)");
  }
  uint32_t storedCrc;
  std::memcpy(&storedCrc, b.data() + b.size() - 4, 4);
  if (crc32(b.data(), b.size() - 4) != storedCrc) {
    return fail("the seed is damaged (checksum mismatch)");
  }
  Reader r { b.data(), b.size() - 4 };
  if (r.get<uint8_t>() != kVersion) {
    return fail("the seed comes from a newer NUMOS");
  }
  const uint8_t flags = r.get<uint8_t>();
  SeedContents c;
  if (flags & kHasSimulation) {
    c.hasSimulation = true;
    c.simulation.cloudAdvectOffset[0] = r.get<float>();
    c.simulation.cloudAdvectOffset[1] = r.get<float>();
    c.simulation.cloudEvolutionOffset[0] = r.get<float>();
    c.simulation.cloudEvolutionOffset[1] = r.get<float>();
    c.simulation.timeOfDayHours = r.get<float>();
    c.simulation.animationSeconds = r.get<double>();
  }
  std::vector<std::pair<uint32_t, float>> weatherEntries;
  if (flags & kHasWeather) {
    c.hasWeather = true;
    const uint8_t n = r.get<uint8_t>();
    for (uint8_t i = 0; i < n && r.ok; ++i) {
      const uint32_t h = r.get<uint32_t>();
      weatherEntries.emplace_back(h, r.get<float>());
    }
  }
  const uint16_t n = r.get<uint16_t>();
  uint8_t* dst = reinterpret_cast<uint8_t*>(&c.settings);
  for (uint16_t i = 0; i < n && r.ok; ++i) {
    const uint32_t h = r.get<uint32_t>();
    const SettingType type = SettingType(r.get<uint8_t>());
    uint8_t value[12];
    if (!r.raw(value, valueSize(type))) {
      break;
    }
    const SettingInfo* hit = nullptr;
    for (const SettingInfo& info : settingInfos()) {
      if (hashName(info.key) == h) {
        hit = &info;
        break;
      }
    }
    if (!hit || hit->type != type) {
      ++c.unknownEntries;  // removed, renamed or retyped since the seed was made
      continue;
    }
    std::memcpy(dst + hit->offset, value, valueSize(type));
  }
  if (!r.ok || r.at != r.n) {
    return fail("the seed is damaged (truncated)");
  }
  if (c.hasWeather) {
    c.weather = weatherFromSettings(c.settings);
    size_t count = 0;
    const WeatherFieldInfo* fields = weatherFields(count);
    for (const auto& [h, v] : weatherEntries) {
      bool found = false;
      for (size_t i = 0; i < count; ++i) {
        if (hashName(fields[i].name) == h) {
          c.weather.*fields[i].member = v;
          found = true;
          break;
        }
      }
      c.unknownEntries += found ? 0u : 1u;
    }
  }
  out = c;
  return true;
}

} // namespace numos
