// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Seed: one short text string that describes an entire NUMOS configuration, to copy, paste and share.
// It carries every setting that differs from its default (settings missing from a seed are defaults,
// so a seed replaces a configuration rather than patching it), optionally the weather in force, and
// optionally the renderer's simulation state (cloud drift and evolution, the running time of day, the
// animation clock), which together with the settings and the weather fix the exact sky.
//
// Format: "NUMOS1-" + base64url of a little-endian binary record ending in a CRC-32, so a mistyped
// seed is rejected instead of half-applied. Settings are tagged by a hash of their config key and
// weather fields by a hash of their name, so a seed survives settings added, reordered or removed
// later: entries a build does not know are skipped and counted.

#include <cstdint>
#include <string>
#include <string_view>

#include "numos/renderer.h"
#include "numos/settings.h"
#include "numos/weather.h"

namespace numos {

  // `weather` / `simulation` may be null (not carried).
  std::string encodeSeed(const Settings& settings, const WeatherSnapshot* weather = nullptr,
                         const SimulationState* simulation = nullptr);

  struct SeedContents {
    Settings        settings;
    bool            hasWeather = false;
    WeatherSnapshot weather;
    bool            hasSimulation = false;
    SimulationState simulation;
    uint32_t        unknownEntries = 0;  // settings or weather fields this build does not have
  };

  // False (with `error` set when given) for a string that is not a valid seed; `out` is then untouched.
  bool decodeSeed(std::string_view seed, SeedContents& out, std::string* error = nullptr);

} // namespace numos
