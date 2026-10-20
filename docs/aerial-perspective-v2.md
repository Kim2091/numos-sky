# Aerial perspective v2: froxel near field + Hillaire far field

Status: implemented. Replaces the aerial perspective
(AP) volume that NUMOS inherited from its Remix integration, and is meant to be able to replace Remix's own
RTX volumetrics (the froxel radiance cache) as a game's fog and air system.

## 1. What is being replaced

The old NUMOS AP (`aerial_perspective_lut.comp.slang`, six variants) is a Hillaire-style
camera-frustum volume pushed far past what that construction is good at:

| | Old NUMOS AP |
| --- | --- |
| Grid | 192 x 192 screen texels (square, so ~10 x 5.6 px at 1080p) x 32 slices |
| Depth | exponential from the volumetrics handoff (>= 1 m) to 32 km: each slice ~38 % deeper than the last |
| Work | rebuilt from scratch every frame: 6 march steps per slice, each with a transmittance and a multiscattering LUT tap; up to 6 sun rays + 6 sky-probe rays per froxel, one ray per cluster light per slice |
| Temporal | none (only an optional visibility re-trace period) |
| Far field | surfaces past 32 km get the last slice; clouds read the volume at their mean depth |
| Artifact control | a per-pixel near fade (50-250 m) that hides the coarse grid's shadow halos, so the volume contributes nothing in the first 50 m |

At 192 x 192 x 32 that is 1.18 M froxels, ~7 M medium evaluations and up to ~14 M rays per frame
with scene shadows on, for a volume that still cannot represent anything nearer than 50 m or
smaller than a 10 px column. That is "heavy and too low res".

## 2. What Remix's RTX volumetrics do

(`rtx_global_volumetrics.cpp`, `shaders/rtx/pass/volumetrics/*`, the volumetrics headers in the
RTXDI submodule. Numbers are Remix defaults.)

* **Grid.** Render resolution / `froxelGridResolutionScale` (8) in X and Y, `froxelDepthSlices`
  (64) in Z: 240 x 135 x 64 = 2.07 M froxels at a 1080p render resolution (presets go from 32 px to
  3 px). Depth is quadratic, `viewZ = near + (slice/N)^2 * froxelMaxDistance`, and the grid ends at
  **20 m** by default; NUMOS AP starts where it stops.
* **Storage.** Luminance as L1 SH (RGBA16F) + chroma CoCg (RG16F) + age (R8), each double
  buffered: ~54 MB at the default grid. ReSTIR reservoirs on a coarser grid (XY / 4, 128 slices,
  RGBA32UI, double buffered).
* **Passes.** Volume ReSTIR initial (RIS over ~32 light candidates, optional visibility ray),
  optional visibility reuse, temporal reuse (M <= 2), spatial reuse (8 neighbours), then the
  integrate pass: per main froxel one reservoir, **one NEE shadow ray**, radiance added to the SH.
  About 2.1 M + 0.26 M rays per frame at the default grid. Lights are the whole Remix light list
  (sun and moons included, cloud shadow folded onto the sun), each scaled by its
  `volumetricRadianceScale`; shadow rays use `OBJECT_MASK_OPAQUE | OBJECT_MASK_ALPHA_BLEND`.
* **Temporal.** Each froxel centre is reprojected into the previous grid with the previous camera,
  history is read trilinearly, weight `1/(age+5)`, age capped at 128 frames. Out-of-frustum
  reprojection or a camera cut resets. Sample positions are jittered by +-0.5 froxel; no
  neighbourhood clamping.
* **Medium.** Homogeneous: `transmittanceColor` at `transmittanceMeasurementDistance` gives
  sigma_t, `singleScatteringAlbedo`, HG `anisotropy`. Optional fBm density noise applied only in
  the composite; optional "atmosphere" clip sphere as a crude height fog.
* **Consumers.** The composite ray-marches the cache per pixel (N/4 steps, dithered), evaluates
  HG from the SH, then applies analytic Beer-Lambert past the grid. Secondary/indirect path
  tracer rays get **extinction only** (`calcVolumetricAttenuation`); particles and a few
  approximations read irradiance from the SH (`evalVolumetricNEE`).

So Remix has the near-field machinery (resolution near the camera, temporal accumulation, many
lights with visibility) but only over 20 m, with a homogeneous medium and no multiple
scattering; NUMOS had the physically based atmosphere but none of the near-field machinery.

## 3. What Hillaire 2020 prescribes

"A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020:

* **Transmittance LUT** (paper: 256 x 64), Bruneton parameterization by altitude and view zenith.
* **Multiple-scattering LUT** Psi_ms (32 x 32) by altitude and sun zenith: second-order
  scattering from a uniform sphere of directions, summed to infinite order as 1/(1 - f_ms),
  applied with an isotropic phase. Lets every later integrand get all scattering orders from one
  tap, which is what makes the rest cheap.
* **Sky-view LUT** (192 x 108) by azimuth and a non-linear latitude that concentrates texels at
  the horizon, rendered once per frame from the camera.
* **Aerial perspective volume** (32 x 32 x 32) fitted to the camera frustum, 32 slices over 32 km,
  storing in-scatter RGB and mean transmittance; each pixel reads it once.
* Planet shadowing in every integrand, sun shadowing of the volume via shadow maps.

NUMOS already has the first three (transmittance 512 x 128, multiscattering 32 x 32, sky-view
512 x 256). The fourth is what gets rebuilt. Hillaire's own AP volume is a *distant haze* tool:
32 linear 1 km slices cannot hold a 3 m lamp glow or a shadow shaft through a doorway, and the
paper never asks it to.

## 4. Design

Two volumes, split by distance, sharing one medium and one composite rule.

```
camera ---------- near field (froxel grid) ---------- R ---------- far field (Hillaire LUT) ----------> 400 km
         scene-shadowed sun, sky visibility, local lights,          atmosphere only, planet + cloud
         height fog, temporal accumulation                           shadow, all directions, no history
```

### 4.1 Medium

One participating medium, evaluated identically by both volumes:

* **Atmosphere**: Rayleigh + Mie + ozone at altitude (existing `sampleAtmosphereMedium`), distances
  in haze km (`numosWorldToAirKm`; DESIGN.md "Units and scale").
* **Height fog** (new): exponential, `sigma_t(h) = sigma0 * exp(-(h - h0) / H)` with sigma0 from a
  meteorological visibility (`fogVisibilityMeters`, Koschmieder: sigma0 = 3.912 / V, 0 = off),
  scale height `fogHeightFalloffMeters`, base `fogBaseHeightMeters`, grey albedo `fogAlbedo`, HG
  `fogAnisotropy`. Distances in the same haze km. This is what Remix's homogeneous fog presets map
  onto (a very large H is homogeneous fog).
* Local-light glow also uses haze km. The haze, the fog and the glow share one scale,
  `rtx.atmosphere.hazeWorldScale` (default 1, the game's measured size); the clouds use
  `rtx.atmosphere.worldScale`. With the haze on World Scale, a World Scale below 1 hazed the game's
  terrain as if it were 1 / World Scale times as far away while the clouds over it were not.

### 4.2 Near field: froxel grid

* **Resolution.** X = ceil(renderWidth / `aerialPerspectiveFroxelSizePixels`), Y likewise; default
  8 px, i.e. 240 x 135 at a 1080p render resolution, the same XY as Remix's default grid.
  Z = `aerialPerspectiveFroxelDepthSlices`, default 96.
* **Range.** `aerialPerspectiveFroxelRangeMeters`, default 1000 m (Remix: 20 m). That is where scene
  shadows and local lights live; past it the far field takes over.
* **Slice distribution.** Linear near the camera, exponential far:
  `z(w) = d0 * ((1 + R / d0)^w - 1)`, `d0 = R / 500` (2 m at the default range), z = forward view
  distance. At the defaults each slice is ~6.5 % deeper than the previous: 0.13 m thick at the
  camera, 0.8 m at 10 m, 6.6 m at 100 m, 65 m at 1 km. Inside its 20 m Remix's quadratic grid is
  still finer (0.44 m at 10 m); matching it there takes ~128 slices. Past 20 m Remix has nothing,
  which is the trade: 1.5x Remix's froxel count for 50x its range.
* **Lighting pass** (`aerial_perspective_froxel_lighting`, one thread per froxel, 8x8x1 groups so a group shares one light cluster).
  The sample point is jittered inside the froxel each frame (R2 sequence over frames, hashed per
  froxel). Per froxel per frame:
  * **one environment ray**: on even (froxel parity + frame) a sun visibility ray, on odd a sky
    visibility ray in a cosine-distributed upper-hemisphere direction. Both against the host TLAS,
    `OBJECT_MASK_OPAQUE`, `FORCE_OPAQUE`, first hit (the old AP's policy). Both terms are
    accumulated temporally, so each converges at half rate but each froxel costs one ray, not twelve.
  * **local lights**: weighted reservoir sampling over the froxel's light cluster (up to 16 lights
    per cluster, target = unshadowed phase-weighted luminance), one shadow ray to the chosen light
    (stops at the emitter surface), estimate `f / p`. Clusters with no lights cost nothing.
  * **host injection** (optional): a host volume with the same layout adds its own local-light
    in-scatter (section 4.6).
  * **temporal**: reproject the froxel centre with the previous camera, trilinear history read,
    weight `max(1 / (age + 1), 1 / aerialPerspectiveHistoryFrames)` (default 32 frames), reset on
    out-of-frustum, camera cut or any setting that moves the grid.
  * **Fresh froxels** (no history: just entered the frustum, or reset). Until 2026-10-15 a fresh
    froxel started from sun and sky visibility 1 ("unoccluded") and traced only the term its
    alternation turn asked for, so the other term stayed at 1 until its next turn and then decayed
    through the history. Indoors, where the true visibilities are ~0 and the exposure is high,
    that showed as a bright haze veil along whichever screen edge the camera turned toward
    (reported in game interiors); a shorter history only shortened it. Now a fresh froxel
    traces both terms in its first frame and takes them outright (weight 1), the sky with 4
    azimuth-stratified rays so a partial sky starts at a quarter-step value rather than 0 or 1.
    Only the revealed froxels pay the extra rays (a pan reveals a few grid columns per frame).
    Without scene shadows (`aerialPerspectiveSceneShadow` off, or no TLAS) visibility is 1
    everywhere, so there is nothing to fix there.
  * What is stored is lighting only, never the medium: sun visibility (scene only), sky
    visibility, and local-light in-scatter per unit scattering coefficient. Cloud shadow, sun
    transmittance, phase and density are smooth and analytic, so they are recomputed every frame
    in the integration and never lag.
* **Integration pass** (`aerial_perspective_froxel_integrate`, one thread per column, serial over
  slices, 8x8 groups). Per slice: medium at the slice midpoint, sun = sunIlluminance x transmittance
  LUT x planet shadow x cloud shadow x scene visibility x phase, multiple scattering = Psi_ms x
  sigma_s x sky visibility (Hillaire Eq. 10 with the visibility the paper drops, because indoors it
  is not negligible), fog lit the same way with its own phase, local lights x sigma_s; integrated
  with the energy-conserving form (Hillaire 2015, Eq. 17). Writes cumulative in-scatter (RGB, and
  the mean transmittance in A) at the far edge of every slice.
* **Transmittance is not stored.** Both media are exponential in height, so the optical depth of
  any segment inside the near range has a closed form (flat-earth: at 20 km the curvature drop is
  31 m, far below a scale height). The composite evaluates the RGB transmittance per pixel, exactly,
  instead of reading a filtered texture.
* **Formats.** Lighting: RGBA16F (local RGB, sun visibility) + RGBA8 (sky visibility, age),
  double-buffered; integrated: RGBA16F. 3.11 M froxels -> 2 x 37.3 MB + 24.9 MB = **~100 MB**
  (Remix's default grid: ~54 MB plus its reservoirs).

### 4.2.1 Local-light glow is per pixel

A lamp's glow falls off as 1/d^2, far faster than a froxel is wide, so through the grid it showed
the 8 px cells. The glow is therefore not integrated into the grid. The composite evaluates it per
pixel (`apLocalLightAirlight`) for every light in the pixel's 8x8-froxel screen tile (the cull pass
writes one extra per-tile list over the whole range): equiangular quadrature along the pixel's ray
(Kulla & Fajardo 2012), 8 fixed midpoint taps in angle around the ray's closest approach, each with
the real medium, phase, spot shaping, closed-form transmittance from the camera and the froxel
grid's local-light visibility. In angle the 1/d^2 cancels the Jacobian, so 8 taps are near exact
however close the ray passes the lamp, with no banding and no noise. The grid supplies only the
shadowing, as a luminance-weighted visibility in Aux.B: RIS picks lights by unshadowed luminance, so
the picked light's 0/1 visibility is an unbiased sample of it. The lighting pass still stores the
shadowed RGB estimate for `numosAirSegment`.

Cost: per pixel, (lights in its tile) x 8 taps, each about one medium evaluation, one light
evaluation, one closed-form transmittance and one trilinear tap. Pixels whose tile holds no light
pay one buffer read. At 1080p with, say, 4 lights covering a third of the screen, that is ~22 M
tap evaluations, ALU-bound with no rays.

The composite must now run after the cloud composite, since the near field's air and glow lie in
front of the clouds.

### 4.3 Far field: camera-centred Hillaire LUT

`aerial_perspective_far` bakes a 3D LUT indexed like the sky-view LUT (absolute azimuth, sqrt
latitude) x distance: **128 x 64 x 48**, RGBA16F in-scatter + RGBA16F transmittance (6.3 MB),
distances `t(w) = d * ((1 + D/d)^w - 1)` with d = 0.5 km, D = 400 km (~15 % per slice). It
covers every direction, not just the frustum, so reflections, secondary rays and clouds out of
view can use it. One thread per direction marching all 48 slices (two sub-steps each), 8192
threads; atmosphere only, planet shadow, sun transmittance, Psi_ms, and the cloud shadow hook.
It depends on sun, altitude and clouds, so it is rebuilt every frame; at ~0.8 M medium evaluations
it costs a small fraction of the old volume.

Height fog is not in this LUT. Its density is exponential in height, so optical depth along any
ray has a closed form and its in-scatter, lit by the sun and Psi_ms at the fog base, is
`J * (1 - T)`; it is evaluated per lookup. That keeps sky pixels fogged out to the horizon without
the LUT having to know what the sky behind it contains.

### 4.4 Composite rule

For a pixel at distance t along view direction v, with (S_n, T_n) the near volume at min(t, R),
(S_f, T_f) the far LUT, (S_g, T_g) the analytic fog over [R, t]:

```
t <= R : L = S_n(t) + T_n(t) * L_surface
t >  R : L = S_n(R) + T_n(R) * ( S_g + T_g * ( S_atm + T_atm * L_surface ) )
         S_atm = (S_f(t) - S_f(R)) / T_f(R),  T_atm = T_f(t) / T_f(R)
miss   : L = S_n(R) + T_n(R) * ( S_g(R..inf) + T_g * (L_sky - S_f(R)) / T_f(R) )
```

The miss line is what lets near-field shadows and lamp glow continue across a silhouette into the
sky: the sky already holds the unshadowed column, so the near volume replaces its first R of it.
In clear air with nothing shadowed, S_n(R) = S_f(R) and the sky is unchanged. Transmittance is
RGB everywhere (wavelength-dependent), including between the two fields.

**Camera in another medium.** Every line above assumes the camera ray starts in air. With the
camera under water the first part of every ray is water: its extinction and in-scatter belong to
the host's medium, and the scene-visibility rays (`OBJECT_MASK_OPAQUE`) pass through the water
surface, so the froxels see the open sky and the composite would add a full, bright air column in
front of a dark underwater view (reported in a game: the whole view washed blue-white).
The host must skip `numosApplyAerialPerspective` while its camera is submerged. Clipping the air to
the part past the water surface is not done: the composite knows neither where the ray leaves the
water nor its refracted direction, and anything seen through the surface from below is already
attenuated by the water column in front of it.

### 4.5 Cloud-shadow hook

`apCloudSunTransmittance(worldPos)` is the only place either volume asks about clouds. Today it
reads the camera-centred D_sun grid (`sampleCloudGroundShadow_OptionB_explicit`) inside the grid's
valid half extent and fades to a coverage-based mean past it, because the grid wraps every 12 km
while the de-tiled cloud field does not. A proper sun-space cloud shadow map (a 2D optical-depth
map covering ~100 km) would drop in here unchanged.

### 4.6 Local lights: host interface

* **Built in.** `FrameInputs::localLights` (unchanged struct). The CPU ranks and frustum-culls
  them, a cull pass bins them into clusters of 8 x 8 froxels x 1 slice, the lighting pass samples
  one per froxel per frame by RIS with one shadow ray. This is Remix's structure (RIS + one NEE
  ray per froxel + temporal accumulation) without its reservoir passes: the froxel history is the
  temporal reuse, and the cluster bound replaces the 32 global candidates. Per-light
  `volumetricRadianceScale` is the host's to apply to `LocalLight::radiance`, as before.
* **Host injection.** With `FrameInputs::hostLocalLightInjection`, NUMOS publishes
  `FrameOutputs::aerialPerspectiveLightInjection`, a 3D RGBA16F texture laid out exactly like the
  froxel grid. The host writes, at any point in frame N, the phase-weighted incident local-light
  radiance per unit scattering coefficient at froxel centres (`numosFroxelCenterWorld` gives the
  position, `numosFroxelViewDirection` the direction to weight the phase for), e.g. from Remix's
  own Volume ReSTIR passes. NUMOS reads it in frame N+1 through its reprojection, so it takes the
  same temporal path as the built-in samples. Remix can therefore keep its light sampling and
  visibility exactly as is and hand NUMOS the result.

### 4.7 Path tracer API (`numos_host.slangh`)

* `numosApplyAerialPerspective(...)`: the composite rule above, for primary hits and misses.
* `numosAerialPerspectiveAlongRay(dirWorld, distanceWorld, ...)`: far LUT + fog for any direction
  from the camera, returning in-scatter and RGB transmittance. For reflections and other secondary
  rays whose origin is near the camera, and for clouds.
* `numosAirSegment(originWorld, dirWorld, length, ...)`: in-scatter and transmittance of an
  arbitrary segment, marched through the near lighting volume where the segment is inside the
  frustum (scene-shadowed sun, sky visibility, local lights) and analytically unshadowed outside it.
  This is what makes the air a participating medium for secondary rays; Remix currently gives
  those rays extinction only.
* `numosAirMedium(worldPos)`: sigma_s / sigma_t at a point, for hosts with their own integrator.

### 4.8 Cloud march integration point

`evalAerialPerspectiveFar(dirYUp, tKm, args, farScatter, farTransmittance, sampler, out T)` in
`lib/aerial_perspective.slangh` returns the air in-scatter and RGB transmittance between the eye
and a point `tKm` (km) along a Y-up view ray, cloud-shadowed. The march in the main checkout
(`cloudAerialPerspective` in `cloud_march_common.slangh`) currently builds the same pair from a
transmittance-LUT ratio and a share of the sky-view radiance with its in-scatter held at zero; it
should instead call this once per sample (one trilinear tap on each of the two far textures), with
the two textures bound to the cloud pass. That gives clouds wavelength-correct, cloud-shadowed AP
out to 400 km.

## 5. Costs (reasoned, not measured)

At a 1920 x 1080 render resolution with the defaults (240 x 135 x 96 near, 128 x 64 x 48 far):

| Pass | Threads | Per thread | Rays / frame |
| --- | --- | --- | --- |
| light cull | 30 x 17 x 96 clusters, only with lights | light list scan | 0 |
| froxel lighting | 3.11 M | reprojection + 2 history taps, RIS over <= 16 lights | 3.11 M environment + 1 per lit froxel |
| froxel integrate | 32.4 K columns | 96 slices x (2 LUT taps + 1 D_sun tap + fog) | 0 |
| far LUT | 8.2 K | 96 sub-steps x (2 LUT taps + 1 D_sun tap) | 0 |
| composite | per pixel | 2 near taps + 2-4 far taps + analytic fog + 8 taps per light in the pixel's tile | 0 |

Rays per frame: ~3.1 M plus one per froxel that has a light in its cluster, against Remix's
~2.3 M (plus its four ReSTIR passes, which this design does not have) and the old AP's up to
~14 M. Medium evaluations: ~3.9 M against the old AP's ~7 M. Memory ~106 MB against Remix's
~60 MB and the old AP's ~36 MB; the difference is the history pair the old AP did not have. The
two knobs that scale all of it are the froxel size (cost ~ 1/size^2) and the slice count (linear).
At 64 slices the grid is exactly Remix's default froxel count and memory drops to ~66 MB.

## 6. Migration

Deleted: `aerial_perspective_lut.comp.slang` and its six variants, `aerial_perspective_camera.slangh`,
`aerial_perspective_lut_binding_indices.h`, the march interfaces and slice helpers at the end of
`atmosphere_common.slangh`, the near fade, the visibility interleave/scan machinery, and these
settings (kept as deprecated keys, migrated where a replacement exists):

| Old key | New |
| --- | --- |
| `aerialPerspectiveLocalLightUnitsPerMeter` | `aerialPerspectiveFroxelUnitsPerMeter` (value moved); since 2026-10-11 both `worldScale` |
| `aerialPerspectiveDepthRangeMeters`, `aerialPerspectiveLutResolution`, `aerialPerspectiveLutDepthSlices` | `aerialPerspectiveFroxelRangeMeters`, `...FroxelSizePixels`, `...FroxelDepthSlices` (different meaning; ignored with a notice) |
| `aerialPerspectiveNearFadeStartMeters`, `...NearFadeEndMeters` | none: the near grid has no halo to hide |
| `aerialPerspectiveSceneShadowRangeMeters`, `aerialPerspectiveLocalLightShadowRangeMeters` | the froxel range |
| `aerialPerspectiveSeparateVisibility`, `...VisibilityUpdatePeriod`, `...VisibilityPerSlice`, `...IntegrateScan`, `...SceneShadowDebug` | none |

Kept: `aerialPerspective`, `aerialPerspectiveMieAnisotropyMax` (and, until 2026-10-11,
`aerialPerspectiveWorldCompression` and `aerialPerspectiveFroxelUnitsPerMeter`, now `worldScale`),
`aerialPerspectiveSceneShadow`, the local-light switches (`...LocalLights`, `...LocalLightIntensity`,
`...LocalLightShadows`, `...LocalLightWindow`, `...LocalLightMaxCount`, `...LocalLightCutoff`),
`cloudAerialInScatterStrength`. New: `aerialPerspectiveFroxelSizePixels`,
`aerialPerspectiveFroxelDepthSlices`, `aerialPerspectiveFroxelRangeMeters`,
`aerialPerspectiveHistoryFrames`, `fogVisibilityMeters`,
`fogHeightFalloffMeters`, `fogBaseHeightMeters`, `fogAlbedo`, `fogAnisotropy`.

Host changes: `FrameOutputs::aerialPerspectiveLut/LocalLut` become `apNearScatter`,
`apNearTransmittance`, `apFarScatter`, `apFarTransmittance`, plus `apFroxelLighting` (radiance
cache) and `aerialPerspectiveLightInjection`; `numosApplyAerialPerspective` takes those. Remix
should turn `rtx.volumetrics.enable` off when NUMOS AP is on (or pass its froxel range as
`volumetricsHandoffWorldUnits`, which is still honoured: the near volume then starts there).

## 7. Open questions

* Cloud shadows in the far field are only exact within the D_sun grid (~6 km); a sun-space cloud
  shadow map is the real fix and belongs with the cloud work.
* Remix's `volumetricRadianceScale` and fog presets need a mapping in the Remix adapter.
* Local lights in the radiance cache are phase-weighted for the camera's view direction; secondary
  consumers get that value, which is exact only for nearly isotropic fog.
* Moonlight is not in either volume (as before).
