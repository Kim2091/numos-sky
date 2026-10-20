# NUMOS

<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/f50aebe1-f2a0-459b-8be9-786827d1c671" />

NUMOS is a physically based sky, atmosphere and volumetric cloud renderer. It is a C++17 library with
Slang compute shaders, built to be embedded in other renderers. RTX Remix uses it as its sky
provider.

## Features

- **Atmosphere**: Hillaire's model (EGSR 2020) with transmittance, multiple scattering and sky-view
  lookup tables, Mie absorption, an ozone layer and planet shadow.
- **Clouds**: volumetric clouds in the style of Guerrilla's Nubis and Nubis Cubed (a voxel density
  field with a signed distance field for empty-space skipping, multiple-scattering approximation),
  a second cloud layer for stratus decks and ground fog, cloud shadows on the ground and in the air,
  clouds in reflections, temporal and spatial filtering, and lightning.
- **Aerial perspective**: a camera-aligned froxel grid near the camera and a camera-centred
  far-field table beyond it, with height fog, glow around local lights and optional scene
  visibility from the host's acceleration structure.
- **Weather**: weather states expressed in the physical model's own settings, presets, and smooth
  blending between them.
- **Night sky**: catalogue stars, the Milky Way, the Moon with its phase and surface, airglow and
  zodiacal light, all lit through the same atmosphere as daylight; sun and moon positions for a
  date and place; support for extra moons and custom star fields.
- **Integration aids**: one world-scale transform between host units and NUMOS units, an automatic
  ground level measured from the scene, settings with user-facing descriptions, and seeds (one
  string that captures a whole configuration).

## Status

NUMOS is in active development. It runs in RTX Remix games through Remix's integration. The public
C++ and shader interfaces may still change between versions. DXIL output and hosts other than
Remix and the reference Vulkan viewer have not been tested.

## Repository layout

```
include/numos/        public C++ API (renderer.h, settings.h, gpu.h, shaders.h, weather.h, seed.h)
src/                  CPU core and pass orchestration
shaders/numos/        Slang shaders: passes/, lib/, util/, and host/ (the host shader API)
backends/vulkan/      reference implementation of the GPU interface (numos::IGpu) on Vulkan 1.3
viewer/               test viewer: headless, or interactive with --interactive
external/             git submodules for the interactive viewer (Dear ImGui, GLFW)
tests/                unit tests
tools/                shader build, generators, night sky data builders
assets/nightsky/      night sky data (see assets/nightsky/CREDITS.md)
docs/                 design notes
```

`docs/DESIGN.md` describes the library's structure, the per-frame contract and the unit system.

## Building

### Requirements

- Python 3 (the shader build and generators use only the standard library; the night sky builders
  and `tools/run_viewer.py` also need numpy and Pillow).
- `slangc`. The committed pass table (`src/generated/passes.inc`) and layout checks were generated
  with slangc 2025.10.4, the version packaged with RTX Remix. Other versions also compile the
  shaders, but produce different SPIR-V, so run `python tools/regen.py` afterwards to regenerate
  the tables.
- CMake 3.20 or later and a C++17 compiler.
- For the Vulkan backend and the viewer: the Vulkan SDK (found by CMake's `find_package(Vulkan)`).
  Configure with `-DNUMOS_BUILD_VULKAN=OFF` to build only the core library and tests.
- For the interactive viewer: the `external/imgui` and `external/glfw` submodules
  (`git submodule update --init`). Without them, or with `-DNUMOS_BUILD_VIEWER_UI=OFF`, the viewer
  is built headless only. The NUMOS library itself never uses them.

### Shaders

```
python tools/build_shaders.py [--slangc PATH]
```

This compiles every pass variant to `build/spirv/`. `slangc` is taken from `--slangc`, then the
`NUMOS_SLANGC` environment variable, then `git config numos.slangc`, then `PATH`.

`python tools/regen.py` rebuilds the shaders and every file generated from them (the layout checks,
the pass tables and the viewer's shaders). `python tools/regen.py --check` fails if a committed
generated file is out of date.

### C++ library, tests and viewer

```
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

On Windows, `tools\build.cmd` does the same with the newest installed Visual Studio toolchain.
Build the shaders first: some tests load the SPIR-V from `build/spirv`.

### Viewer

```
python tools/run_viewer.py build/shots/sunset.png --sun-elevation 4
```

The viewer renders the sky, clouds and aerial perspective over a flat ground plane, headless, and
writes a PNG. It needs `build/spirv`, `build/viewer_spirv` (both made by `tools/regen.py`) and the
built `numos_viewer`. `tools/run_viewer.py` lists its options.

### Interactive viewer

```
build/cmake/numos_viewer --interactive [--conf rtx.conf] [--seed NUMOS1-...] [--weather Storm] ...
```

Run it from the repository root (or pass `--root`). It opens a window that shows the same composite
the headless viewer writes, with a free camera and a UI for every setting. It takes the headless
viewer's options for the starting state (`--conf`, `--seed`, `--set`, `--weather`, `--weather-to`,
the sun, camera and exposure options, `--width`/`--height` for the window size, `--light`,
`--night-sky`, `--validate`); `--frames`, `--out` and the other output options do not apply.

Controls:

| Input | Action |
| --- | --- |
| W A S D | move forward, left, back, right (along the view direction) |
| Q / E | move down / up |
| right mouse button (held) | look around |
| M | toggle mouse look without holding the button |
| Esc | release the mouse |
| mouse wheel | movement speed (0.1 m/s to 100 km/s) |
| Shift / Ctrl (held) | move 10 times faster / slower |
| F1 | hide or show the UI |
| F12 | save the render (without the UI) as a PPM, named by date and time |

The main panel has the view (exposure, vsync, render scale, ground plane, accumulation), the camera
(position, field of view, speed, and buttons that jump to the ground, the cloud base, inside the
clouds or above them), the sun and the clock (sun angles, time cycle, date and latitude, pausing
and speeding up every clock), the weather (presets with a timed blend, each weather value, a
lightning strike on demand), the night sky data folder, config files and seeds. Load replaces every
setting with the file's; Save writes the changed NUMOS settings into the file and keeps its other
lines, so it can update a full `rtx.conf`. Copy seed and Paste seed use the clipboard.

The settings window lists every setting from `settingInfos()`, grouped by kind, with its description
as a tooltip. It has a search box, a filter for changed settings, a reset button per setting, and
hides deprecated settings unless asked. Edits reach the renderer in the next frame. While the
weather is on, the settings it overrides are marked W; edit those in the weather panel instead.
The stats overlay shows the frame rate and, with GPU timings on, the time of each NUMOS pass.

## Use in RTX Remix

Remix includes NUMOS as a git submodule. Remix's build compiles the NUMOS passes with its own
`slangc`, builds the core library into the runtime, and installs the SPIR-V and the night sky data next to the
runtime. Remix implements `numos::IGpu` on its own renderer and calls the host shader API
(`shaders/numos/host/numos_host.slangh`) from its ray tracing passes: sky radiance on ray misses,
cloud shadows on lights, and the aerial perspective and cloud composite. Settings keep their
`rtx.atmosphere.*` names, so existing `rtx.conf` files load unchanged.

Another renderer can embed NUMOS the same way: implement `numos::IGpu` (see
`backends/vulkan/` for a reference), drive `numos::Renderer` each frame, and call the host shader API
from its own shaders. `include/numos/renderer.h` and `include/numos/gpu.h` document the contract.

## Licence

NUMOS is licensed under the Apache License, Version 2.0; see `LICENSE`. The `NOTICE` file must be
included with any redistribution, as section 4(d) of the licence requires.

NUMOS includes small third-party portions (code from NVIDIA's RTX Remix and from community forks of
it, Blender and DXVK, under the MIT and zlib licences). They are listed in `THIRD_PARTY_NOTICES.md`
and keep their own licences and copyright headers; the licence texts are in `licenses/`.

The night sky data in `assets/nightsky/` comes from ESA, CDS and NASA sources under their own terms,
listed in `assets/nightsky/CREDITS.md`. Two of the files, `milkyway.bin` and `starlight_low.bin`, are
derived from Gaia data and are licensed under CC BY-SA 3.0 IGO; that licence applies to those two
data files only.

## Author

NUMOS is designed and developed by Kim2091 (Sparkles). It grew from community atmosphere work in
dxvk-remix forks (CattaRappa, softsoundd and others; see `THIRD_PARTY_NOTICES.md`). The volumetric
clouds, weather, night sky, aerial perspective, the host-independent renderer library, the Vulkan
backend, the viewer and the tools are Kim2091's own work.

- Website: https://www.kim2091.art/
- GitHub: https://github.com/Kim2091
- Ko-fi: https://ko-fi.com/kim20913944

## Crediting NUMOS

Under the Apache License 2.0, redistributions of NUMOS, in source or binary form, must carry the
`NOTICE` file. If you ship NUMOS in a game, mod or tool, please also credit it as:

> Sky and clouds: NUMOS by Kim2091 (Sparkles)
