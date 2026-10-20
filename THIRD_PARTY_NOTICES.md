# Third-party notices

NUMOS is licensed under the Apache License 2.0 (`LICENSE`, `NOTICE`). It includes the third-party
portions listed below. Each keeps its own licence; the copyright and licence headers in the files
are part of these notices and must be kept. Full licence texts are in `licenses/`.

## Code

### NVIDIA RTX Remix (dxvk-remix)

- Source: https://github.com/NVIDIAGameWorks/dxvk-remix
- Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
- Licence: MIT (`licenses/LICENSE-MIT-NVIDIA.txt`, and the header in each file)

| File | State |
| --- | --- |
| `src/blue_noise_128x128x64.h` | unmodified |
| `shaders/numos/util/instance_definitions.h` | unmodified |
| `shaders/numos/util/camera.h` | include path changed, wrapped in `NUMOS_SHARED_BEGIN/END` |
| `shaders/numos/lib/generate_mipmap.h` | include path changed, wrapped in `NUMOS_SHARED_BEGIN/END` |
| `shaders/numos/passes/generate_mipmap.comp.slang` | include paths changed |
| `shaders/numos/util/camera.slangh`, `math.slangh`, `shader_types.h`, `shader_types.slangh` | reduced to what NUMOS uses and modified |
| `shaders/numos/util/common_binding_indices.h` | modified: NUMOS binding slots added |

Modified files carry a "Modifications Copyright 2026 Kim2091 (Sparkles)" line above the NVIDIA
header.

`shaders/numos/lib/aerial_perspective_lights.slangh` reimplements the spot-cone falloff of Remix's
`light_shaping.slangh` so that volume glow matches Remix's lights; no code is copied from it.

### Community dxvk-remix forks: physical atmosphere

NUMOS grew out of the physical atmosphere in community forks of dxvk-remix. Code by the
contributors below still remains in NUMOS. They contributed it to those forks, which are MIT
licensed; NUMOS keeps it under the MIT licence (`licenses/LICENSE-MIT-contributors.txt`). Each file
that contains such code names the contributors in its header ("Portions Copyright") and is marked
`SPDX-License-Identifier: Apache-2.0 AND MIT`.

The list is based on the history of those forks: code lines (comments excluded) that a
contributor's commit introduced and that are still present in NUMOS. Lines that are only
declarations shared by every pass (constant-buffer and texture bindings) are not counted.

| Contributor | Contribution | Original repository | Files |
| --- | --- | --- | --- |
| CattaRappa (sambow23) | the original Hillaire atmosphere implementation (atmosphere parameters, density and phase functions, transmittance, multiple scattering and sky-view LUTs, the host integration), 2025 | https://github.com/sambow23/dxvk-remix-gmod | `shaders/numos/lib/atmosphere_common.slangh`, `atmosphere_args.h`, `atmosphere_bindings.slangh`; `shaders/numos/passes/transmittance_lut.comp.slang`, `multiscattering_lut.comp.slang`, `sky_view_lut.comp.slang`; `src/renderer.cpp`, `src/renderer_impl.h` |
| softsoundd | the atmosphere's corrections to Hillaire, EGSR 2020 (multiple scattering, Mie absorption, ozone profile, planet shadow, LUT parameterisation), 2026 | https://github.com/softsoundd/dxvk-remix-mirrorsedge | `shaders/numos/lib/atmosphere_common.slangh`; `shaders/numos/passes/transmittance_lut.comp.slang`, `multiscattering_lut.comp.slang`; `src/renderer.cpp` |
| Owen Krisch | the sky-view LUT's non-linear elevation encoding and LUT-driven sky radiance, 2026 (via TheGreatHMMMM's fork) | https://github.com/RemixProjGroup/dxvk-remix | `shaders/numos/lib/atmosphere_common.slangh`; `shaders/numos/passes/sky_view_lut.comp.slang` |
| BrunchyChineapple | the cloud-shell intersection and cloud ground shadow helpers of the first cloud layer, 2026 | https://github.com/RemixProjGroup/dxvk-remix | `shaders/numos/lib/atmosphere_common.slangh` |
| Jerran Schmidt | the per-pass binding index headers and the blue-noise lookup, 2026 | https://github.com/RemixProjGroup/dxvk-remix | `shaders/numos/lib/multiscattering_lut_binding_indices.h`, `sky_view_lut_binding_indices.h`, `transmittance_lut_binding_indices.h`; `shaders/numos/lib/atmosphere_common.slangh` |

These forks copied NVIDIA's MIT header template onto their new files. NVIDIA did not write this
code (upstream dxvk-remix has no atmosphere), so NUMOS carries the contributors' names instead of
that template. The NVIDIA header stays only on the files that come from upstream dxvk-remix,
listed in the previous section.

The world-origin handling (`rtx.worldOrigin.enable`) follows an idea from BetaRT
(https://github.com/sambow23/dxvk-remix-gmod); no code from it is included.

### Blender sky model

- Source: Blender, `intern/sky/source/sky_multiple_scattering.cpp`
  (https://projects.blender.org/blender/blender/src/branch/main/intern/sky/source/sky_multiple_scattering.cpp)
- Copyright 2022 Fernando García Liñán; 2011-2025 Blender Authors
- Licence: MIT (`licenses/LICENSE-MIT-Blender-sky.txt`; the full notice is also in
  `shaders/numos/passes/multiscattering_lut.comp.slang`)
- Used in: `shaders/numos/lib/atmosphere_common.slangh` (`computeGroundReflectionAnalytical` and the
  analytical transmittance branch), adapted.

### DXVK

- Source: https://github.com/doitsujin/dxvk (`src/util/util_vector.h`, `src/util/util_math.h`)
- Copyright (c) 2017-2021 Philip Rebohle; Copyright (c) 2019-2021 Joshua Ashton
- Licence: zlib/libpng (`licenses/LICENSE-DXVK-zlib.txt`)
- Used in: `include/numos/math.h`, an altered version of the vector and math helpers.

### Viewer window and UI (git submodules)

The interactive viewer (`numos_viewer --interactive`) uses the two libraries below. They are git
submodules under `external/`, pinned to release tags and unmodified. They are linked into
`numos_viewer` only, and only when `NUMOS_BUILD_VIEWER_UI` is on; the NUMOS library does not use them.

#### Dear ImGui

- Source: https://github.com/ocornut/imgui, tag `v1.93.0` (`external/imgui`)
- Copyright (c) 2014-2026 Omar Cornut
- Licence: MIT (`licenses/LICENSE-MIT-Dear-ImGui.txt`, also `external/imgui/LICENSE.txt`)
- Used: `imgui*.cpp` and the GLFW and Vulkan backends (`backends/imgui_impl_glfw.cpp`,
  `backends/imgui_impl_vulkan.cpp`).
- It includes, under their own terms stated in the files: the stb libraries by Sean Barrett
  (`imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h`; MIT or public domain), the embedded
  ProggyClean font (Copyright (c) 2004, 2005 Tristan Grimmer; MIT) and ProggyForever font
  (Copyright (c) 2026 Disco Hello, Copyright (c) 2019, 2023 Tristan Grimmer; MIT), and in
  `imgui_draw.cpp` a list of Japanese kanji code points that its comment states is available under
  CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/; sourced from the Agency for Cultural
  Affairs and the Ministry of Justice of Japan, generated by https://github.com/vaiorabbit/everyday_use_kanji).

#### GLFW

- Source: https://github.com/glfw/glfw, tag `3.5.1` (`external/glfw`)
- Copyright (c) 2002-2006 Marcus Geelnard; Copyright (c) 2006-2019 Camilla Löwy
- Licence: zlib/libpng (`licenses/LICENSE-GLFW-zlib.txt`, also `external/glfw/LICENSE.md`)
- Used: the library (`src/`), built from the submodule.
- Its gamepad mappings (`src/mappings.h`) come from SDL_GameControllerDB, Copyright (C) 1997-2013
  Sam Lantinga, under the zlib licence stated in that file.

## Data

The night sky data in `assets/nightsky/` is built by `tools/build_nightsky_assets.py` from public
sources. Its credits and licences are in `assets/nightsky/CREDITS.md`; in short:

| File | Source | Licence |
| --- | --- | --- |
| `stars.bin` | Hipparcos main catalogue (ESA 1997, ESA SP-1200), via CDS VizieR I/239 | free use with acknowledgement (ESA / CDS) |
| `milkyway.bin`, `starlight_low.bin` | NASA SVS "Deep Star Maps 2020" (NASA/Goddard Space Flight Center Scientific Visualization Studio; Gaia DR2: ESA/Gaia/DPAC) | CC BY-SA 3.0 IGO, https://creativecommons.org/licenses/by-sa/3.0/igo/ |
| `moon.bin` | NASA SVS "CGI Moon Kit" (LRO LROC WAC colour mosaic, LOLA elevation) | NASA media, not copyrighted; credit requested |

`milkyway.bin` and `starlight_low.bin` must be redistributed with their attribution and under
CC BY-SA 3.0 IGO. That licence covers those two data files only, not the code that reads them.

## Published scientific data and methods

The following are cited in the source and implemented from the publications; no code from them is
included: Hillaire (2016, 2020), Bruneton and Neyret (2008), Schneider (2015, 2017, 2023),
Wrenninge (2013), Jensen et al. (2001), the zodiacal light table of Leinert et al. (1998,
`shaders/numos/lib/zodiacal_light_table.slangh`), the low-precision solar and lunar formulae of the
Astronomical Almanac (`src/celestial.cpp`), Henyey and Greenstein (1941), Kasten and Young (1989), and
the colour-matching fit of Wyman, Sloan and Shirley (2013, `tools/airglow_colour.py`).
