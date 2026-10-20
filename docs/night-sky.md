# The night sky

NUMOS draws night with the same physics as day: every source of light goes through the same atmosphere, the same transmittance and multiscattering LUTs, the same sky-view bake, and the same cloud lighting path. Nothing fades in or out on a timer, and no setting stands in for the physics. Twilight runs continuously into night because each source is simply as bright as it really is.

This page is for host integrators and for anyone changing the night code.

## Sources of light

| Source | Model | Where |
| --- | --- | --- |
| Moonlight | Earth's Moon at its real place for the configured date and latitude (Astronomical Almanac low-precision theory, topocentric parallax). Brightness follows the measured phase law, V = -12.73 + 0.026\|a\| + 4e-9 a^4 (Allen), so the opposition surge is included. Also: lunar colour, earthshine, and the eclipse share of the sun's disk. Game moons can be added: four slots in all, the real moon taking one. | `src/celestial.*`, `updateCelestial` |
| Moon disk | Lommel-Seeliger shading, normalised so the disk delivers exactly the moon's illuminance at any drawn size. LRO colour albedo and LOLA relief are placed by the moon's axis and prime meridian, so the real librations come out of the geometry. | `evalMoonDisk` (atmosphere_sky.slangh) |
| Eclipse light | In the Earth's umbra: sunlight refracted through the limb at 12 km, twice through the configured atmosphere, times 5e-5. That gives a copper-red moon at ~1.5e-5 of full (Danjon ~2). | `getAtmosphereArgs` |
| Stars | Hipparcos stars brighter than V 7.5, as points. Each is drawn as a Gaussian at least one pixel wide (it never aliases), plus a 4% wing. Scintillation is sigma ~ 0.1 X^1.75 (Young 1967), chromatic near the horizon. | `night_sky.slangh` |
| Milky Way | Every fainter star, from the NASA SVS Deep Star Maps (Gaia DR2), calibrated against Hipparcos. The north galactic pole comes to 23.6 V mag/arcsec^2, against a measured 23.5. | `night_sky.slangh` |
| Airglow | Emission from a layer 92 km up and 12 km thick, 130 S10 at the zenith. It is integrated along each ray's path through the layer, so the brightening toward the horizon (van Rhijn) comes from the geometry. Its colour is the CIE colour of a typical line + continuum spectrum (`tools/airglow_colour.py`). | `evalNightSkyGlow`, sky-view bake |
| Zodiacal light | Leinert et al. 1998 Table 17 (S10 by helio-ecliptic longitude and latitude), solar coloured. | `zodiacal_light_table.slangh` |
| Night glow on the air | Airglow, zodiacal light and starlight averaged over the sky above, scattered by the air below them. | `evalAtmosphereRadiance` |

Calibration (moonless, no clouds):
- The zenith comes to **1.7e-4 cd/m^2**, about 22.0 V mag/arcsec^2, a dark site.
- Twilight reaches that floor at sun elevation **-18 deg**, the astronomical definition of night: 24 / 0.90 / 0.025 / 2.3e-3 / 2.3e-4 cd/m^2 at -3 / -6 / -9 / -12 / -16 deg.
- 1 S10 = 6.61e-12 of the sun's illuminance per steradian.

The day sky is bit-identical with the night code present. Everything night-only is gated:
- by `nightSkyFlags` bit 3 (the sun's sky under 3% of daylight);
- by `moonSkyLightMask`, which admits a moon only when its sky would register against the sun's.

## What a host must do

1. **Divide by the pre-exposure.** Night radiance is ~1e-8 of day, below what fp16 can hold, so NUMOS multiplies every light source by a power of two, `FrameOutputs::radianceScale`. It is 1 whenever the sun is up. Divide everything NUMOS outputs by it, or fold it into your exposure. If you forget, night comes out up to 2^40 times too bright.
2. **Pass the assets.** Load `assets/nightsky/*.bin` and call `Renderer::setNightSkyAssets`. NUMOS copies the bytes and streams them to the GPU at 2.5 MB per frame. Without them there are no stars, and moons have a uniform surface; airglow and zodiacal light need no assets.
3. **Bind the night globals** in shaders that call the sky functions: `NightSkyStarCells`, `NightSkyStars`, `NightSkyMilkyWay`, `NightSkyStarlightLow`, `NightSkyMoon` (see `numos_host.slangh`), and define `NIGHT_SKY_AVAILABLE`.
   - Say which rays carry the sun and moon disks (`evalSkyRadiance`'s `skyDisks`): not diffuse or glossy gathers, which get the sun and moons as lights (`FrameOutputs::lights`); yes camera rays and delta-lobe continuations (mirrors, glass and water, pass-through cards). A host that lays something over its camera rays' sky (an alpha-blended card) traces them without disks and adds `numosSkyDisks` under that transmittance before `numosApplyCloudComposite`. The disks' aureoles come with the cloud render's radiance; there is nothing to add (`numosSkyDiskAureole` is deprecated and returns 0). See "Disk aureole" below.
4. **Exposure and tonemapping are yours.**
   - NUMOS outputs physical luminance. A moonless sky is 1.7e-4 cd/m^2; a full-moon sky looks like a blue daytime sky in a long exposure, because physically it is one.
   - Limit how far auto-exposure brightens at night, or night will look like the long-exposure photos.
   - For a natural look, add a night-vision step in the tonemapper: desaturate and shift toward blue at low luminance (rod vision, the Purkinje shift).
5. **Coordinates.** NUMOS's local frame is x east, y up, z north, the frame the sun's azimuth (clockwise from +Z, east = 90) describes. It is left-handed, as on a D3D-style host. A right-handed host that maps its axes onto it directly sees the sky mirrored.

## Disk aureole

The sun or a moon behind thin cloud is not a crisp disk that is merely dimmed: it sits in a bright glow (aureole, and with uniform droplets a coloured corona), and behind thicker cloud only the glow is left. The glow is the droplets' diffraction peak. `rtx.atmosphere.cloudDiskAureole` (on) and `rtx.atmosphere.cloudDropletDiameter` (d, 20 um) control it; code in `cloud_march_common.slangh` (`cloudAureoleRadiance`).

**Physics.** A droplet much larger than the wavelength removes twice the light its cross-section intercepts (extinction efficiency ~2). Half of that is diffraction into a narrow forward peak, of the droplet's Airy-pattern width (half width ~0.51 lambda / d: 0.8 deg for 20 um at 550 nm, 1.7 deg at 10 um, 3.4 deg at 5 um, with a theta^-3 tail); the rest is the broad lobe of refraction and reflection. The weight of the peak is f = 1 - wD from the Jendersie & d'Eon (2023) droplet phase fit, the same fit the reference path tracer uses (`cloud_reference.slangh`): 0.50 at 20 um, 0.52 at 10 um, 0.55 at 5 um.

**The split.** The march's view transmittance T = exp(-tau) is the unscattered beam: the disk keeps it, sharp (`numosSkyDisks` under the cloud composite, unchanged). The march's phase function (dual HG, g ~0.8) is the broad lobe only; it holds no diffraction peak (the reference path tracer treats the peak as "not scattered" for the same reason). Counting scatterings along the view ray as a Poisson process with rates f tau (peak) and (1 - f) tau (broad), the light that arrives after one or more peak scatterings and no broad one is

  aureole share = exp(-(1 - f) tau) - exp(-tau) = T^(1 - f) - T

of the disk's illuminance (sunlit part plus earthshine for a moon). It is spread over the sky by the peak's own phase: an HG lobe with 1 - g = 0.671 lambda / d (which matches the Airy half width, and both fall off as theta^-3), per colour channel at 0.65 / 0.55 / 0.45 um, so a corona is red outside. n peak scatterings convolve n such lobes, which is exactly HG with g^n; the kernel takes the mean n of the aureole's light, f tau / (1 - exp(-f tau)), so it widens as the cloud thickens. The kernel is normalised over the sphere, so with a uniform cloud the glow integrates to exactly the share above.

**Energy accounting.** Of a disk's light entering cloud of optical depth tau: T stays in the sharp disk; T^(1 - f) - T arrives in the aureole; 1 - T^(1 - f) has scattered broadly at least once, which the march accounts for (its in-scatter). The aureole's light that a peak scattering takes outside the visible sky is inside the HG kernel's tails and stays counted there. The glow along a view ray is evaluated with that ray's own optical depth, the cloud the forward-scattered light crossed (small-angle scattering travels essentially along the view ray), so it follows the cloud's structure and needs no gather. Without cloud (T = 1) the share is 0 and nothing changes; a disk below the horizon has none.

**Double counting.** The march's broad lobe is normalised over its own scattering, and near the disk it is two orders of magnitude below the peak (HG g = 0.8 is 3.6 per sr straight ahead, the peak ~150 per sr at 20 um), so the march adds about 2% to the aureole's own radiance within a few degrees. Its scattering coefficient, however, is the full extinction, so in energy terms its broad lobe already carries the peak's share f of single scattering, spread wide: a pre-existing calibration question of the march against the reference path tracer (which scatters broadly with wD sigma), not something this changes. With the aureole the light reaching the camera along the disk's direction, T^(1 - f), is the reference's view transmittance (its delta tracking against wD sigma).

**Where it is computed.** In the cloud march itself (`marchCloudLayers`, once per ray, after both layers), added to the cloud's premultiplied radiance. It needs the cloud's true optical depth, which only the march has (below). So it rides the screen pass's temporal filter and denoise with the rest of the cloud's light, and it is in the dome LUT the secondary rays read: mirrors, glass and water see it, and diffuse gathers of the sky pick it up, which together with the sun's cloud-shadowed light sample (T) gives surfaces the forward channel's T^(1 - f). Only the clouds' key light has one: the sun, or at night the moon that lights the clouds (the cloud args' sun slot); a second moon's is left out.

**Past the march's exit.** The march stops at T < 1% (tau 4.6) and reports such cloud as T = 0 (the exit rescale, `kCloudExitTransmittance`), but the forward channel exp(-(1 - f) tau) is still ~0.1 there and decays over about twice that depth. The first version read the aureole from the reported T; it fell from ~0.1 of the sun's light to 0 on the exit contour, a hard bright outline in thick cloud near the sun (FNV, 2026-10-15). A second version extrapolated the depth past the stop at the ray's mean extinction over the rest of the slab; but the slab's shell runs far beyond the cloud's far side (tens of km toward a low sun), so the extrapolation jumped from 0 to tens of optical depths across the contour and drew the same edge (FNV, same day). Now a ray that stopped early measures the cloud it did not march: density only, no light, with the march's own density field (`cloudMarchDensity`, the same span and control fields), from its stop to the end of that crossing and through every later crossing of either layer. The taps start at the march's own step length and double (8 per crossing), so the cloud right behind the stop is resolved as the march resolves it and distant cloud still counts. Every input is then continuous across neighbouring rays: on both sides of the exit contour the aureole is the same function of the ray's total optical depth, one side marched and the other measured, and the two differ only by quadrature, which vanishes where little cloud is left (a ray that just reaches the limit does so at the cloud's far side). The measurement only lowers the aureole (its share falls with depth past tau ~1.4), so the aureole at the marched depth bounds it; where that bound is under 2% of the cloud's own light on the ray the measurement is skipped and the bound used, an error too small to show as an edge, which keeps the cost on the rays the aureole can reach. Marching every opaque ray on to 1e-4 instead would cost 15-20% of the pass.

**Limits.** Aerosol haze gets no separate peak: aerosols are ~0.1-1 um, so their diffraction is tens of degrees wide, which the air's Mie phase (Cornette-Shanks, in the sky-view LUT, around every disk) already holds. A real droplet size distribution washes the corona's rings out; the kernel has no rings, only the red outer edge.

## Customising the night sky per game

- **Game moons.** `rtx.atmosphere.moon<N>.*` places extra moons (four slots in all, the real moon taking one) by elevation and rotation, with a drawn size and an albedo tint. Phase, brightness and the light they cast follow from the sun geometry, exactly as for the real moon. Turn off `realMoonEnabled` for a game that has its own single moon. A game whose moon keeps its own phase calendar (rather than its place relative to the sun; a moon overhead at every midnight would otherwise always be full) sets `moon<N>.phase` (0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter): the light, earthshine and the lit limb (west while waxing) then follow that phase, with no eclipse. Negative (the default) keeps the geometric phase.
- **Custom moon surfaces.** `NightSkyAssets::gameMoons[i]` gives game moon *i* its own map. Convert any equirectangular colour map (longitude 0 at the centre, north up) and an optional grey height map:

  ```
  python tools/build_nightsky_assets.py --custom-moon albedo.png [--custom-moon-height height.png --height-range-km -8 8] --custom-out moon_game0.bin
  ```

  The viewer loads `assets/nightsky/moon_game<N>.bin` when present. NUMOS rescales each map so its near side averages the moon's measured brightness and colour. A map only says where the surface is brighter or darker; it never changes how much light the moon gives. A game moon shows its map's centre to the observer, with north toward the ecliptic's side of the sky.
- **Custom star fields.** `tools/build_custom_sky.py` writes a game's own `stars.bin`, `milkyway.bin` and `starlight_low.bin`, in the same formats as the real sky's, so the renderer is unchanged. Pass them to `setNightSkyAssets` instead of the shipped ones; the viewer takes `--night-sky DIR`. It can build:
  - a procedural galaxy, calibrated so `--brightness 1 --stars 25000` matches Earth's sky in brightness distribution and star counts. Options set the band's width, bulge, star clouds, dust lanes and tint, and where the galaxy sits on the celestial sphere (`--galaxy-pole`, `--galaxy-centre`);
  - or a sky from the game's own data: an equirectangular image of the diffuse light (`--background`, scaled to `--background-mean-s10`) and/or a star list (`--stars-csv`: RA, Dec, V, B-V).

  ```
  python tools/build_custom_sky.py --out mysky --seed 7 --band-width 14 --dust 1.5 --preview mysky.png
  ```

## Settings

| Key | Default | Meaning |
| --- | --- | --- |
| `rtx.atmosphere.radianceAutoScale` | true | the pre-exposure above (leave on) |
| `rtx.atmosphere.realMoonEnabled` | true | draw Earth's Moon for the date |
| `rtx.atmosphere.realMoonAngularDiameter` | 0.518 | its drawn size in degrees; its light stays physical, so a bigger disk is dimmer per pixel |
| `rtx.atmosphere.calendarYear`, `dayOfYear`, `timeOfDayHours` | 2026, 80, 12 | the date the sky is drawn for |
| `rtx.atmosphere.latitudeDegrees`, `northOffsetDegrees` | | where the celestial pole is |
| `rtx.atmosphere.nightSkyStars` | true | stars and Milky Way |
| `rtx.atmosphere.starScintillation` | 1 | turbulence relative to a typical night |

With a hand-placed sun (no time cycle), the sky turns rigidly around it: the frame puts the sun's catalogue position for the date exactly on the drawn sun (`celestial::alignedFrame`). The moon's place and phase therefore always agree with the sun on screen.

The old `moonNeeStrength`, `moonAtmosphericCouplingStrength`, `nightSkyBrightness` and `nightSkyColor` settings were removed on 2026-10-06; a config that still sets them loads, and the keys are ignored.

## Data assets

`tools/build_nightsky_assets.py` downloads the sources and writes `assets/nightsky/`. Its docstring holds the file formats and the star-map calibration. Credits and licences are in `assets/nightsky/CREDITS.md`. The Gaia-derived `milkyway.bin` and `starlight_low.bin` are CC BY-SA 3.0 IGO; the rest is free with acknowledgement or public domain. The zodiacal table is the published Leinert et al. (1998) data; the cells within ~30 deg of the sun, which the paper leaves blank, are filled with an extrapolation toward the F corona.

## Cost

- **Sky-view bake.** At night it adds one transmittance tap per sample, plus the airglow and zodiacal terms per texel. It re-bakes only when the sun, the moons or the celestial frame move a quantisation step.
- **Per sky pixel.** Stars cost one cube-map cell (a few candidates on average) and the Milky Way four texel loads. A moon costs eight texel loads, and only inside its disk.
- **Clouds.** They are lit by the moon through the sun's own path: `getCloudArgs` swaps the key light once the sun can no longer reach any visible cloud. That costs nothing extra.

Not measured yet on a free GPU; the whole of NUMOS has a 2 ms target at 1080p.

## Known approximations

- Positions come from low-precision theories (~0.3 deg for the moon). Eclipses happen on the right dates, but their timing and depth are approximate.
- The moon's relief tilts its shading normal; it does not cast shadows near the terminator.
- The night glow on the air uses one isotropic, hemisphere-averaged source with about two air masses of extinction, and no multiple scattering of its own.
- Starlight scattered toward the eye comes from the same average; individual bright stars do not light the air.
