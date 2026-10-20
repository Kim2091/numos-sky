# Provenance

NUMOS began as the sky, atmosphere and cloud system the author developed in Remix Plus, a fork of
RTX Remix (dxvk-remix). In October 2026 it was extracted into this repository so that it could be maintained
on its own and used by other renderers.

## The extraction

The initial import was a byte-exact copy of the atmosphere code from the author's Remix Plus
runtime:

- the atmosphere and cloud shaders (`src/dxvk/shaders/rtx/pass/atmosphere/` in Remix);
- the Remix shader headers they include, found by walking the `#include` graph: the camera
  helpers, the shader type and math utilities, `common_binding_indices.h` and
  `instance_definitions.h`;
- the Remix integration (`rtx_atmosphere`, `rtx_weather` and `rtx_weather_presets`), kept at first as
  the reference for the port and removed once the port was done.

Precipitation, which is built on Remix's particle system, stayed in Remix.

## Layout changes

The shader tree was then moved under `shaders/numos/` with no content changes other than `#include`
paths; the SPIR-V built after the move was byte-identical to Remix's own build:

| Remix | NUMOS |
| --- | --- |
| `rtx/pass/atmosphere/*.comp.slang` | `shaders/numos/passes/` |
| `rtx/pass/atmosphere/*` (other) | `shaders/numos/lib/` |
| `rtx/utility/*`, `rtx/concept/camera/*`, `rtx/pass/common_binding_indices.h`, `rtx/pass/instance_definitions.h` | `shaders/numos/util/` |

Later, Remix's mip-chain pass (`rtx/pass/image_utils/generate_mipmap.comp.slang` and
`generate_mipmap.h`) was added as `shaders/numos/passes/generate_mipmap.comp.slang` and
`shaders/numos/lib/generate_mipmap.h`, for the cloud reflection table's mip chain.

## The host-independent core

The Remix integration was ported into the library in `src/`: Remix's options became a plain
settings struct (`include/numos/settings.h`, keeping the `rtx.atmosphere.*` keys), the DXVK calls
became the small GPU interface `numos::IGpu`, and Remix's scene, camera and light queries became
per-frame host inputs. A reference Vulkan backend and a headless viewer were added to run the
library without a game. Remix now consumes NUMOS as a submodule.

## Third-party code that came along

- NVIDIA's MIT-licensed Remix shader headers listed above.
- The physical atmosphere's origins in community forks of dxvk-remix: CattaRappa's (sambow23)
  original Hillaire atmosphere, softsoundd's EGSR 2020 corrections, and smaller contributions by
  Owen Krisch, BrunchyChineapple and Jerran Schmidt.
- Blender's MIT-licensed sky model code (Fernando García Liñán and the Blender Authors), adapted in
  the multiple-scattering helpers.
- DXVK's zlib-licensed vector math, re-implemented operation for operation in
  `include/numos/math.h`.

`THIRD_PARTY_NOTICES.md` lists the files, copyrights and licences.
