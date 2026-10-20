# NUMOS library design

Goal: a sky, atmosphere and cloud renderer that any renderer can embed. Remix is one host among many, not a special case.

## What a host gets

| Piece | Form |
| --- | --- |
| CPU core | C++17 static library `numos`, no third-party dependencies. Namespace `numos`. |
| Shaders | Slang source plus prebuilt SPIR-V for every pass variant, with a generated binding table per variant. DXIL is a later target (Slang emits it). |
| GPU access | `numos::IGpu`, a small interface the host implements over its own API: textures, buffers, samplers, constant uploads, compute dispatch with declared resource access. A reference Vulkan implementation ships in `backends/vulkan`. |
| Host shader API | `shaders/numos/host/numos_host.slangh`: the functions a host's own shaders call (sky radiance for a ray, cloud shadow at a point, aerial perspective and cloud composite). They take their resources as parameters, so the host binds them wherever it likes. |
| Settings | `numos::Settings`, a plain struct with defaults, plus a key table (string key, type, range, description) for config files and generated UI. Keys keep the `rtx.atmosphere.*` names, so existing Remix configs load unchanged. |

## Frame flow

```
host                                   numos
----                                   -----
setSettings(settings)        ───▶      (any time; applied at next beginFrame)
beginFrame(gpu, FrameInputs) ───▶      CPU: time of day, anchors, wind, lightning, weather blend, args;
                                       GPU: sky LUTs, cloud bakes, shadow maps, secondary LUT,
                                       aerial perspective (uses the host TLAS if given)
  ... host renders its scene, producing linear view depth ...
dispatchPostDepth(gpu, depth)───▶      screen-space cloud pass
outputs()                    ◀───      textures, samplers, args buffer, sun/moon light descriptions
  ... host shaders call numos_host functions with those outputs; host composites ...
```

`FrameInputs` carries everything NUMOS used to read from Remix:

- frame index, delta time, camera cut;
- camera: the Camera constant block (makeCamera builds it from plain matrices), position in render space, the game's own camera position when a free camera is active;
- conventions: up axis, units per metre fallback, world origin (float);
- render extent (the cloud screen pass runs at it);
- optional: an opaque TLAS, local lights for aerial perspective glow, "host sky override active" (dome light), blue noise, a blended weather snapshot.

The full per-frame contract (call order, when outputs are valid, ownership, barriers, threading) is in the comments of `include/numos/renderer.h` and `include/numos/gpu.h`.

## Units and scale

The host's world and NUMOS's own differ by **one similarity transform**, applied where values cross
between them and nowhere else:

```
NUMOS km = axes(render-space vector) / worldUnitsPerKm        worldUnitsPerKm = 1000 x unitsPerMeter x worldScale
```

- `rtx.atmosphere.unitsPerMeter` is the measurement: what one game unit is (0 inherits
  `FrameInputs::hostUnitsPerMeter`).
- `rtx.atmosphere.worldScale` ("World Scale", default 1) is the one artistic choice: the size of
  NUMOS's world relative to the game's. 0.1 makes the whole sky a tenth of the size. It is a scale
  rather than a compression so that it multiplies straight into the measurement.
- `rtx.atmosphere.hazeWorldScale` ("Haze World Scale", default 1) is the same choice for the aerial
  perspective alone (the near and the far field, the height fog, local-light glow):
  `apWorldUnitsPerKm = 1000 x unitsPerMeter x hazeWorldScale`. At 1 the game's terrain is hazed for
  its measured distance. With the haze on World Scale below 1, terrain is hazed as if 1 / World
  Scale times as far away while the clouds over it are not, and distant land turns far bluer than
  the clouds above it.
- Inside NUMOS everything (planet, atmosphere, clouds, wind, haze, fog, local-light glow,
  lightning, LUTs, march steps, shadow maps) runs at full scale in its own metres and kilometres. A
  scaled NUMOS is therefore, by construction, the full-scale one shrunk: it looks and behaves
  identically, only smaller. The haze's metres are its own (Haze World Scale). Settings in metres or kilometres are NUMOS lengths and shrink with the
  world; settings in world units (`groundLevelWorldUnits`, `cloudVerticalOffsetWorldUnits`,
  `cameraWorldOverride`) are the game's and do not.
- Radiance is scale-invariant. A host light described by radiance and size needs no conversion (its
  size converts as a length); only a light described as intensity per distance squared would, and
  NUMOS takes none.

The conversion has one implementation per language: `Renderer::Impl::worldUnitsPerKm()` (with
`worldToCloudKm` / `cloudKmToWorld` / `cloudFrameCameraKm` for positions) on the CPU, and
`numosWorldToKm`, `numosKmToWorld`, `numosWorldToKmYUp`, `numosKmYUpToWorld` and
`numosWorldToCloudKm` (`lib/atmosphere_common.slangh`, reading `args.worldUnitsPerKm`) in the
shaders. The aerial perspective converts with `Renderer::Impl::hazeWorldUnitsPerKm()` and
`numosWorldToAirKm`, `numosAirKmToWorld`, `numosWorldToAirKmYUp` (`args.apWorldUnitsPerKm`), with
its eye `args.apCameraAltitudeKm` above the datum; its cloud shadow lookups convert world positions
with the clouds' scale. Positions are converted relative to the rendered camera (`args.cameraPosition`, whose cloud
frame position is `args.cameraWorldPosYUpKm`), so the world origin, the ground datum, the cloud
field's vertical offset and the camera-world override carry over with the camera.

Every crossing, and the side it converts on:

| Crossing | Direction | Where |
| --- | --- | --- |
| Camera position (and free camera, world origin, override) -> cloud anchor | in | `updateFrame` anchor, `cloudFrameCameraKm` |
| Ground datum `groundLevelWorldUnits` | in | `updateFrame` (`m_groundLevelYUpKm`) |
| Cloud vertical offset `cloudVerticalOffsetWorldUnits` | in | `cloudFrameCameraKm`; "Center Layer at Player" back out (`getCloudOffsetAtPlayer`) |
| Linear view Z -> surface distance for the cloud march | in | `cloudSurfaceDistanceKm` (cloud_render) |
| Cloud temporal reprojection (cloud distance, wind step, layer-2 follow step, camera move; docs/cloud-temporal-filter.md) | out / in | `cloudTemporalHistory` (cloud_render) |
| Cloud depth (`cloudDepth`) vs a surface: alpha-blend hit distance, fog in front of the cloud | in / out | `numosApplyCloudComposite` |
| `foregroundDepthKm` | in (host converts with `numosWorldToKm`) | `numosApplyCloudComposite` |
| Surface / volume point -> cloud shadow map | in | `sampleCloudGroundShadowFromMap` / `_OptionB`, `apCloudSunTransmittance` (`numosWorldToCloudKm`) |
| Froxel range `aerialPerspectiveFroxelRangeMeters` -> grid in world units | out | `apGrid` |
| Froxel / far-field segment lengths and positions -> media, fog, glow | in | `apSampleMedium`, `apIntegrateSegment`, `apFogOpticalDepth`, `apNearTransmittance`, far pass |
| Far-field slice distance -> world position | out | `aerial_perspective_far` |
| View distance -> far-field LUT | in | `numosApplyAerialPerspective`, `numosAerialPerspectiveAlongRay` |
| Medium coefficients per world unit | out | `numosAirMedium` |
| Host volumetrics handoff `volumetricsHandoffWorldUnits` | none (compared in world units) | `apStartDistance` |
| Local lights (position, size, cull radius) | none until integrated; glow path length converts in | `buildAerialPerspectiveLights`, `apLocalLightAirlight` |
| Sky-visibility shadow ray (1 NUMOS km) | out | froxel lighting pass |
| Sun shadow ray, local-light shadow ray | none (world-space geometry; sun ray unbounded) | froxel lighting pass |
| Lightning: host-placed strike in, strike as host light (position, radius) out, haze glow radius | in / out | `placeLightning`, `lightningRenderPosition`, `syncDistantLights`, `buildAerialPerspectiveLights` |
| Host light injection (`apLightInjection`) | none (radiance) | froxel lighting pass |
| Sun, moons | none (directions) | `syncDistantLights` |

Not exactly invariant: a few numerical guards are fixed in world units (`1e-4` on a light's
equivalent radius, `1e-6` / `1e-12` divide guards). They only matter for degenerate inputs (a lamp
under 0.1 mm at 1 unit per metre).

## Ground level

Altitude zero for the sky, the height fog and the cloud field (both layers, the cloud shadow maps,
the reflection dome, the placement bake, lightning) is one datum, the ground level. Cloud heights are
heights above it, so a datum the game does not match puts the deck far above (or below) the player:
games rarely build their land at a fixed height, and the hand-set `groundLevelWorldUnits` is one
number for a whole map.

**Ground Level: Auto** (`rtx.atmosphere.groundLevelAuto`, default on) measures it instead:

- *Probes* (`ground_probe.comp`, only with a host TLAS): 64 rays a frame, straight down from 4 NUMOS
  km above the camera to 4 km below it, from points of an R2 sequence spread over a disc of 3 sigma
  around the camera with a density falling as 1 / r. Hosts that know their terrain can add
  `FrameInputs::groundPoints` (or use them alone, without a TLAS).
- *Readback*: the hits come back through a three-slot readback ring (`IGpu::readBuffer`, which never
  blocks) a few frames later. Each slot keeps the render-to-world offset of the frame that cast it,
  so the hits land in world coordinates even under a moving world origin or the camera-world
  override. The field lives on the CPU because the datum has CPU consumers (the args' camera
  altitude, the sky-view LUT key, lightning placement, "Center Layer at Player") and because a few
  frames of latency are irrelevant for something this smooth; a GPU-only field would have needed a
  second, GPU-side copy of every one of those.
- *A low statistic*: hits land in world-anchored cells of sigma / 2; a cell keeps its last 64 hits and
  takes their 25th percentile. Roofs, canopies and rocks are always above the ground, never below,
  and the ground between buildings and under trees is reached by most probes, so the low quantile
  measures the ground.
- *Heavy low-pass*: the field at a point is the normalized Gaussian convolution (sigma =
  `groundSmoothingMeters`, NUMOS metres, so it scales with World Scale) of the measured cells, pulled
  toward the mean of everything measured this session where no cell is near. Unmeasured places (no
  hit, beyond the loaded scene) carry no weight; far from every measurement the field is that mean, so
  it is defined everywhere and has no edges. Without any measurement the configured level is used.
- *Stability*: the field is a function of where, not when. A jump or a climb does not move the datum
  (it depends on horizontal position only), and walking moves it only as fast as the smoothed land
  rises. The only time dependence is a place's measurement improving: a new cell's weight fades in
  over 4 s and a refined estimate moves at most 20 NUMOS m/s. Within 2 s of a camera cut (load,
  teleport) both apply at once.
- *A different world*: when the cloud anchor jumps (`cloudAnchorCutKm`: a teleport, a load, an interior
  or another worldspace) or the host sets `FrameInputs::worldChanged`, the field is discarded (cells and
  session mean) and probes still in flight are dropped: places at the same coordinates can be unrelated
  (an interior under a city, the main menu's scene). Until fresh probes return (two or three frames)
  the configured level applies. A camera cut alone (another camera, a scope, a menu over the same
  world) keeps the field.

The datum is the field evaluated under the resolved camera anchor, and everything that has an
altitude hangs off that one value. That is what keeps the clouds, their shadows, the dome, layer 2,
the height fog and lightning in agreement with each other: none of them reads the field separately.

**Not done: the deck following the land at each point.** The exact form of this design puts the deck
base at base + G(x, z) everywhere, not base + G(camera). The two agree under the camera and differ at
distance d by G(x) - G(camera), at most |grad G| d. With the default smoothing the slope of G is small
on most maps, but World Scale works against it: a world scaled by 0.14 has seven times the relief in
NUMOS metres, and the difference 10 km out can reach hundreds of metres. Doing it exactly means
warping the cloud frame's altitude in every cloud shader: the height fractions
(`computeCloudHeightFractionC` and the explicit altitude sites, about 45 in all), the slab's bounding
shells widened by the field's range (the view march span, the light marches, the shadow map's depth
parameterization, which bake and lookup must share), the vertical optical-depth, Beer and horizon
maps, lightning placement and the far fog's analytic column. The field would reach the shaders as a
16 x 16 window in AtmosphereArgs (no new bindings). That is a rendering change across the whole cloud
system and it was not made blind: it needs GPU runs to validate. The CPU field above is already
world-anchored and is exactly the input it would use.

## Night gain

`rtx.atmosphere.nightGainStops` (default 0 = physical, range 0-16) multiplies the natural light of the night
together: the moons (their direct light, their discs, the sky and clouds they light), airglow, zodiacal light
and the star field. The sun, its twilight and lightning keep their physical levels. Small values are a
perception adjustment on a calibrated game (a dark-adapted, scotopic eye sees a moonlit night brighter than
a display shows it); large ones balance the night against a game whose own lights are not calibrated.

The factor in force (`src/night_gain.h`, `FrameOutputs::nightGain`) fades in through twilight. With
`twilight` the sun's sky relative to daylight and `night` the natural night sky (moons and night glow) on the
same scale, x = log2(twilight / (night * G)) is how many stops the twilight sky is above the boosted night.
The gain is complete at x <= 0, absent at x >= X, and a smoothstep in between, with X = the gain's size in
stops (at least 1) but never reaching into the day (X <= log2(1 / (night * G))). Over the blend the boosted
night stays as many stops under the twilight as the gain still has to go, so the total sky keeps falling as
the sun sets (tested over -30..+10 degrees for gains up to 12 stops) and settles on night * G. A full moon at
+10 completes near a sun depression of 6 degrees, at +2 near 11, a moonless night later. Everything keyed to
the sky's level uses the gained sky: NUMOS's pre-exposure (so a boosted night does not overflow) and the
level where a lightning flash ends.

## Rules

1. **No host types cross the boundary.** No DXVK, Vulkan, D3D or ImGui types in public headers except the optional backends.
2. **The library never owns the frame.** The host calls it; it never calls back into the host except through `IGpu`.
3. **Explicit access, explicit barriers.** Every dispatch declares read/write per resource (by binding kind), and NUMOS calls `IGpu::barrier()` where the Remix integration emitted one, which covers its read-after-write hazards. A host without hazard tracking honours `barrier()` and orders its transfers; DXVK-based hosts also get the tracking for free.
4. **One shared args layout.** `AtmosphereArgs` is shared C++/Slang, guarded by a size and offset check, and is bound as its own constant buffer, never embedded in a host struct.
5. **Shared C++/Slang headers live in `namespace numos`** in C++, so they cannot collide with a host's own `vec3` or `mat4`.
6. **Pixel parity first.** Until the viewer and the Remix adapter match the imported Remix output, changes are structural only. Look changes come after.

## Layout

```
include/numos/        public C++ API (renderer.h, settings.h, gpu.h, shaders.h, weather.h, seed.h, math.h)
src/                  core (CPU) and pass orchestration
shaders/numos/passes/ compute passes (*.comp.slang)
shaders/numos/lib/    shared shader code and shared C++/Slang headers
shaders/numos/util/   vendored math/colour/camera helpers (from Remix, MIT)
shaders/numos/host/   functions host shaders call
backends/vulkan/      reference IGpu implementation
viewer/               standalone test app
tools/                shader build, binding-table generator
```

## Order of work

1. Restructure the shader tree under `shaders/numos/` (output stays byte-identical).
2. Shared headers: `namespace numos`, own math types, no DXVK.
3. Settings struct with defaults and key table.
4. CPU core: port of the args packing and frame state.
5. `IGpu` plus pass orchestration: port of the dispatch code.
6. Weather blender and presets.
7. Vulkan backend and viewer.
8. Remix adapter, in Remix.
