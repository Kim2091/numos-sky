// Copyright 2026 Kim2091 (Sparkles)
// Portions Copyright (c) 2025 CattaRappa (sambow23)
// SPDX-License-Identifier: Apache-2.0 AND MIT
// The portions are under the MIT licence (licenses/LICENSE-MIT-contributors.txt);
// see THIRD_PARTY_NOTICES.md.

#pragma once

#include "numos/util/shader_types.h"

NUMOS_SHARED_BEGIN

// Most moons the sky can draw. The moon loops unroll over it, and every moon costs six constant rows.
#define MAX_MOONS 4u

// A moon: Earth's real Moon (rtx.atmosphere.realMoonEnabled) and/or game moons, packed from index 0;
// the host computes everything from the sun-moon geometry. Six 16-byte rows.
struct MoonParams {
  vec3 direction;          // toward the moon, Y-up, normalized (topocentric for the real moon)
  float angularRadius;     // drawn half-angle, radians

  vec3 illuminance;        // its light at the top of the atmosphere, x radianceScale: the sun's
                           // illuminance x the measured lunar phase law x lunar colour x albedo
                           // tint x the share of the sun's disk the Earth leaves it (eclipse)
  float enabled;           // 1 = drawn and lighting, 0 = skip

  vec3 northPole;          // the moon's rotation axis in the same frame, orients its surface
  float phaseAngle;        // sun-moon-observer angle, radians: 0 = full, pi = new

  vec3 albedoTint;         // relative to the Moon's albedo and colour (1 = the Moon)
  float earthshine;        // the Earth's illuminance on the moon over the sun's (~1e-4 at new moon)

  vec3 primeMeridian;      // from the moon's centre through longitude 0 on its equator: with
                           // northPole, places its surface map (libration included)
  float textureLayer;      // its band of the moon texture atlas (NightSkyMoon), -1 = untextured

  vec3 sunlightDirection;  // toward the sun from the moon, Y-up, shades its disk: the true sun's
                           // direction, or one placed at the phase a game moon's phase setting gives
  float reserved;
};

// Everything the NUMOS passes and the host shader API read for one frame. Shared with C++ and laid
// out in 16-byte rows (scalar layout); include/numos/detail/layout_checks.h pins every offset.
// Lengths are NUMOS km unless a name says otherwise.
struct AtmosphereArgs {
  vec3 sunDirection;
  float planetRadius;  // km

  vec3 sunIlluminance;
  float atmosphereThickness;  // km

  vec3 rayleighScattering;
  float mieAnisotropy;  // Henyey-Greenstein g, [-1, 1]

  vec3 mieScattering;
  uint flipUpAxis;  // negate the world up axis when converting into the atmosphere's Y-up frame

  // Aerosols absorb as well as scatter (Hillaire 2020, Table 1), so Mie extinction is scattering
  // plus absorption. Raising absorption relative to scattering makes haze darker and browner as it
  // thickens, like dust.
  vec3 mieAbsorption;  // km^-1
  float ozoneLayerAltitude;  // peak of the ozone tent profile, km

  // Ozone absorption gives the twilight sky its blue (Hillaire 2016, section 3.4).
  vec3 ozoneAbsorption;  // km^-1

  uint transmittanceLutWidth;
  uint transmittanceLutHeight;
  uint multiscatteringLutSize;
  uint skyViewLutWidth;

  uint skyViewLutHeight;
  float ozoneLayerWidth;  // half-width of the ozone tent profile, km (the paper's 30 km tent = 15)
  // Camera altitude above the ground datum. It ignores the cloud field's vertical offset; cloud
  // geometry uses cameraWorldPosYUpKm.y, which includes it.
  float cameraAltitudeKm;
  float multiScatterPhysicalStrength;  // 0 = analytical fill, 1 = LUT hemisphere integration (physical)

  // Derived on the CPU.
  float atmosphereRadius;  // planetRadius + atmosphereThickness
  float rayleighScaleHeight;  // km
  float mieScaleHeight;  // km
  float sunAngularRadius; // radians

  // ----- Night sky (lib/night_sky.slangh) -----
  // Bit 0: the star / Milky Way assets are loaded and bound (NightSky* globals); bit 1: draw them
  // (rtx.atmosphere.nightSkyStars); bit 2: the moon surface atlas is loaded (NightSkyMoon); bit 3:
  // the sky is dark enough for the night sky's own light (airglow, zodiacal light, starlight).
  uint nightSkyFlags;
  uint starCellsPerFace;    // cube-map cells per face edge of the star catalogue (stars.bin)
  uint milkyWayWidth;       // the Milky Way background texture (milkyway.bin), texels
  uint milkyWayHeight;
  float timeSeconds;        // wall-clock seconds, for the stars' scintillation

  // The natural night-sky glow (night_sky.slangh evalNightSkyGlow), all 0 until the sky is dark
  // (nightSkyFlags bit 3). Radiances are physical x radianceScale.
  vec3 airglowEmission;     // airglow radiance per km of path through the ~90 km emission layer

  float skyPixelAngle;      // angular size of a pixel at the screen centre (radians): a star is
                            // drawn as a footprint at least this wide, so it never aliases
  float starScintillation;  // turbulence strength relative to a typical night (1)
  float airglowAltitudeKm;  // centre and full width of the airglow layer
  // Ground-fog clear zone (fogNearFadeMeters): the height fog and cloud layer 2 thin to nothing
  // toward the camera over this distance (km), so the ground right around the player stays readable.
  float fogNearFadeKm;

  float airglowThicknessKm;
  float zodiacalLightScale; // zodiacal light radiance per S10 as a fraction of sunIlluminance
  float cloudViewSamplesMax;            // cap on the adaptive cloud-march step count
  // Safety factor on the SDF empty-space skip in marchCloudSlab; 0 disables the skip.
  float nvdfStepScale;
  float cloudForwardPeakWeight;  // share of a cloud droplet's extinction in its diffraction peak (disk
                                 // aureoles, atmosphere_sky.slangh); 0 = no aureoles

  vec3 nightGlowInScatter;  // airglow + zodiacal light + starlight averaged over the sky above, as
                            // the air's in-scatter source per unit scattering coefficient
  float cloudDropletDiameterUm;  // effective cloud droplet diameter (um): the aureoles' width

  MoonParams moons[MAX_MOONS];

  // ----- Celestial frame (night sky) -----
  // Rows of the rotation from equatorial J2000 (x toward RA 0h, z toward the north celestial pole)
  // into the local Y-up frame, from the observer's latitude and the local sidereal time; the stars,
  // the Milky Way and the zodiacal light turn with it. The four rows also carry unrelated scalars.
  vec3 celestialToLocalX;
  // Diagnostic. Bit 1: evalSkyRadiance returns a flat grey, to measure the cost of the miss path.
  uint  debugSkyBisectFlags;

  vec3 celestialToLocalY;
  // The coverage the NVDF body SDF was baked at (the quantized weather coverage, or a pinned value).
  // The occupancy bake uses it, and the sampler offsets the level set by the difference to it.
  float nvdfNominalCoverage;

  vec3 celestialToLocalZ;
  float cloudHexTilingEnable;             // 1 = hide the tiled noise volume's repetition with
                                          // randomized triangle-lattice taps, 0 = one periodic tap

  vec3 eclipticPoleLocal;                 // the ecliptic's north pole in the local Y-up frame
  float cloudDensity;       // extinction multiplier for the clouds

  vec3 cloudColor;          // cloud albedo tint

  float cloudAltitude;      // cloud base, km
  float cloudEnabled;       // 1 = clouds on
  float cloudShadowStrength;// how strongly clouds dim ground and atmosphere lighting [0..1]

  vec2 cloudWindOffset;     // accumulated wind advection, km
  float skyIndirectRadianceScale; // multiplier on diffuse-indirect sky radiance (>= 0)
  float cloudTemporalHistoryWeight; // screen cloud temporal filter: history share per frame (0 = off)

  float cloudWindStepXKm;  // this frame's wind advection (cloudWindOffset minus last frame's), km,
  float cloudWindStepZKm;  // Y-up XZ; the temporal filter's reprojection follows the cloud with it.

  float cloudThickness;        // cloud slab depth, km
  float cloudLayer2TypeSpread; // [0,1] cloud-type variation for layer 2 (independent of layer 1)
  uint cloudViewSamples;       // view-march steps through the slab (the floor of the adaptive count)

  // ----- Spatial variation (Nubis-style weather fields) -----
  float cloudTypeMean;             // [0,1] mean cloud type: 0 stratus, 0.5 stratocumulus, 1 cumulus
  float cloudTypeSpread;           // [0,1] variation around the mean
  float cloudTypeNoiseScale;       // frequency of the type field
  float cloudCoverageMean;         // [0,1] mean coverage

  float cloudCoverageSpread;       // [0,1] variation around the mean
  float cloudCoverageNoiseScale;   // frequency of the coverage field (independent of type)
  // Distance (km) past which the march's per-sample jitter stops animating. Freezing it is only
  // correct without a temporal filter, so the default is large.
  float nubis3JitterAnimateKm;
  // radianceScale / last frame's: brings radiance kept from the previous frame (cloud history) into
  // this frame's scale. Exactly 1 except on the frame the scale steps.
  float radianceScaleHistoryRatio;
  uint  cloudMultiScatterOctaves;  // Wrenninge octaves to sum (clamped to 1..4 in the shader)
  float cloudLayer2NoiseSeed;      // offset into layer 2's 2D coverage and type noise, so its pattern
                                   // is uncorrelated with layer 1's at the same XZ

  float cloudNoiseTileKm;   // period of the tiling 3D detail noise, km
  // Strength of the clouds' effect (occlusion and their own light) on sky ambient [0..1]; 1 = physical.
  float cloudSkyAmbientCloudOcclusionStrength;
  float cloudCellSizeKm;         // average cloud-cluster footprint for the placement map bake, km
  // Slow 3D offset of the base noise only (not of height, placement or the column model), so the
  // field morphs in place: clouds form and dissolve while altitude and placement stay put. Y scrolls
  // through the volume's tiling vertical axis and does most of the morphing. 0 = frozen.
  float cloudEvolutionOffsetX;
  float cloudEvolutionOffsetY;

  // ----- Cloud lighting (Nubis Cubed 2023; evalNubisCubedSampleCore) -----
  float cloudPhaseG1;              // forward HG asymmetry of the first sun octave (silver lining)
  // Layer 2 patch-field map (cloud_layer2_map.comp): its centre in the cloud frame (km, world xz
  // minus the wind offset) and whether it holds this frame's layer 2 (1) or must not be read (0).
  float cloudLayer2MapCenterX;
  float cloudLayer2MapCenterZ;
  float cloudLayer2MapValid;
  uint  cloudRenderFrameIdx;       // frame counter for fastJitter() in cloud_render.comp.slang

  float cloudBodyBillowStrength;  // billow displacement, x cloudBodyBillowSizeKm (km)
  float cloudBodyBillowSizeKm;    // largest billow wavelength (km); 4 octaves below it
  float cloudCurlStrengthKm;      // curl distortion of the detail lookup (km)
  float cloudColumnTopVariation;   // [0,1] per-cloud tower-height jitter (0 = uniform tops)

  float cloudSunOctaveA;           // sun octaves: per-octave extinction factor (a)
  float cloudSunOctaveB;           // per-octave contribution factor (b)
  float cloudSunOctaveC;           // per-octave phase anisotropy factor (c)
  float cloudColumnTopShape;       // exponent mapping column presence to top height (lower = taller edges)

  uint  cloudSunOctaveCount;       // sun octaves [1, 8]
  float cloudBodyBillowGain;       // amplitude ratio between billow octaves
  float groundAlbedo;              // the ground: multiple scattering, the surface below the horizon, cloud bases
  float cloudColumnBaseVariation;  // [0, ~0.4] largest local cloud-base lift as a fraction of the slab

  // Primary rays composite the screen cloud pass (cloudRenderRTEnable); every other ray reads the
  // secondary dome LUT (cloudSecondaryLutEnable). The screen pass is in primary-ray pixel
  // coordinates, so it cannot answer for any other direction.
  uint  cloudRenderRTEnable;       // 0 or 1
  uint  cloudSecondaryLutEnable;   // 0 or 1
  uint moonSkyLightMask;           // bit i: moons[i] lights the sky bake (it is not lost under the sun's sky)
  uint cloudKeyLight;              // the light the clouds and their shadow map are lit by: 0 = the sun,
                                   // i + 1 = moons[i] (set once no visible cloud can see the sun)

  // worldUnitsPerKm is the one conversion between host units and NUMOS km
  // (unitsPerMeter x worldScale x 1000); numosWorldToKm and friends in atmosphere_common.slangh apply
  // it. cameraWorldPosYUpKm is the camera in the cloud frame; numosWorldToCloudKm places render-space
  // positions relative to it.
  uint  cloudVoxelShadowsEnable;   // 0 or 1: cloud shadows on lights (sampleCloudGroundShadow_OptionB)
  float worldUnitsPerKm;           // host units per NUMOS km
  float cloudColumnFeather;        // feather band of the coverage remap that decides column presence
  float cloudDiffuseSunStrength;   // sunlight diffused down through the cloud above [0..1]
  float cloudTemporalStillSamples; // screen cloud temporal filter: history length cap for a still
                                   // view, in samples

  vec3  cameraWorldPosYUpKm;       // camera position, Y-up km, world-absolute

  // ----- Cloud layer 2 -----
  // A second, stratiform layer (ground fog, stratus, a high deck, dust) marched and lit by the same
  // code as layer 1 (cloud_march_common.slangh, CloudSlab). The shadow maps, the dome-LUT
  // reduction's ground shadow and the aerial perspective's cloud shadow stay layer-1 only.
  uint  cloudLayer2Enable;         // 1 = render layer 2
  float cloudLayer2Altitude;       // base, km (already following the camera when that is on)
  float cloudLayer2Thickness;      // depth, km

  float cloudLayer2TypeMean;       // [0,1] 0 = stratus/fog water profile, 1 = cumulus profile
  float cloudLayer2CoverageMean;   // [0,1] share of the area the layer covers
  float cloudLayer2DensityScale;   // extinction as a multiple of cloudDensity

  // A march sample whose contribution weight (view transmittance x haze x its own opacity) is below
  // this reuses the last lit sample's light instead of marching its own light paths. 0 disables it.
  float cloudLightingLodThreshold;
  // The host's linear view Z for a sky miss (NRD's sentinel), so the cloud pass can tell sky from
  // surface without hardcoding the value.
  float cloudMissLinearViewZ;
  float lightningEnvelopePrevious;  // lightningEnvelope of the previous frame (the cloud history's flash)
  // Target view-march step length, km. The per-ray step count is span / step, clamped to
  // [cloudViewSamples, cloudViewSamplesMax], so long horizon spans are not undersampled.
  // 0 = the fixed count.
  float cloudViewStepKm;

  // Unit wind direction in the Y-up XZ plane: the detail drift and the base shear follow it. It
  // changes under weather drift, so normalizeForSkyLutCache zeroes it or every wind rotation would
  // re-bake the sky LUTs.
  float cloudWindDirUnitX;
  float cloudWindDirUnitZ;

  // Layer 2's vertical move since the previous frame (km, Y-up) while its base follows the camera,
  // 0 otherwise: the cloud temporal filter reprojects layer 2's share of a pixel through it.
  float cloudLayer2FollowStepKm;
  // NVDF profile: the depth into the body over which the dimensional profile ramps 0 to 1, and the
  // level-set shift per unit of coverage away from the baked nominal (coverage grows or shrinks the
  // bodies without a re-bake).
  float nvdfProfileDepthKm;
  float nvdfCoverageOffsetKm;
  // Scale of the detail noise before the value-erosion remap of the dimensional profile.
  float nubis3ErosionStrength;
  // The flash's luminous intensity this frame (envelope x lightningLuminousIntensity, as illuminance x
  // km^2, x radianceScale): the one source of the cloud glow and the scene light. 0 = no flash.
  float lightningFlashIntensity;

  // Layer 2 single-scattering albedo per channel (1 = water cloud).
  vec3  cloudLayer2Albedo;
  float lightningEnvelope;        // raw flicker envelope [0..1]

  // Lightning. The CPU scheduler (Renderer::Impl::advanceLightning) drives the flicker envelope. The
  // screen cloud pass adds a glow around the strike (never the secondary LUT, which must not bake a
  // transient flash), and syncDistantLights hands the host a sphere light at the same place. Both rows
  // are zeroed in normalizeForSkyLutCache so a flash never re-keys the sky LUTs.
  vec3  lightningStrikePosKm;     // strike position, world-anchored Y-up km (the frame of cameraWorldPosYUpKm)
  // Floor of the view march's distance-adaptive step (Schneider 2023, pp. 172-174):
  // step = max(SDF x nvdfStepScale, clamp(cloudViewStepKm x sqrt(t / 12 km), floor, 4 x cloudViewStepKm)).
  // 0 = fixed-length steps.
  float nubis3AdaptiveStepKm;

  vec3  lightningColor;           // flash hue (linear RGB, unit luminance), for the glow and the light

  // ----- Aerial perspective (docs/aerial-perspective-v2.md) -----
  // Rebuilt every frame from the camera, so normalizeForSkyLutCache zeroes every field of this
  // block: none of it may re-key the transmittance / multiscattering / sky-view bakes.
  //
  // Near field: a froxel grid fitted to the camera frustum. Slice w in [0, 1] sits at forward
  // (view Z) distance d0 * ((1 + range / d0)^w - 1), linear near the camera and exponential past d0.
  uint  apFroxelSizeX;            // grid width; 0 when aerial perspective is off
  uint  apFroxelSizeY;
  uint  apFroxelSlices;
  float apFroxelRange;            // forward distance of the last slice's far edge, world units

  float apFroxelLinearDepth;      // d0, world units
  float apFroxelLogScale;         // ln(1 + range / d0)
  // Host froxel volumetrics range (world units, along the view ray): the near field integrates
  // nothing nearer than this, so the two hand off instead of stacking. 0 without host volumetrics.
  float apStartDistance;
  uint  isZUp;                    // non-zero when the game world is Z-up rather than Y-up
  uint  apFrameIndex;             // jitter sequence index for the froxel lighting pass

  // World-space origin of every aerial perspective ray. Rays themselves use the active Camera.
  vec3  cameraPosition;

  // Far field: a camera-centred LUT over every direction (sky-view parameterization) x distance,
  // slice w at d * ((1 + range / d)^w - 1) km.
  uint  apFarSizeX;
  uint  apFarSizeY;
  uint  apFarSlices;
  float apFarLinearDepthKm;       // d

  float apFarLogScale;            // ln(1 + range / d)
  // The aerial perspective's own frame (rtx.atmosphere.hazeWorldScale): host units per haze km, and
  // the camera's height above the ground datum in haze km. Every length and position the near
  // field, the far field and the height fog integrate converts through numosWorldToAirKm and
  // friends; cloud lookups keep worldUnitsPerKm.
  float apWorldUnitsPerKm;
  float apCameraAltitudeKm;
  // Upper bound on the Mie anisotropy both fields use; the sky keeps the uncapped lobe.
  float aerialPerspectiveMieAnisotropyMax;

  uint  apSceneShadow;            // 1: trace sun and sky visibility against the host TLAS
  uint  apLocalLightShadows;      // 1: trace the sampled local light against the host TLAS
  float apHistoryWeightMin;       // 1 / history frames: the floor of the temporal blend weight
  uint  apHistoryReset;           // 1: the froxel history does not describe this frame's grid

  // Exponential height fog: sigma_t(h) = fogExtinctionPerKm * exp(-(h - fogBaseAltitudeKm) / H),
  // h in km above the atmosphere's ground datum. 0 extinction = no fog.
  float fogExtinctionPerKm;
  float fogScaleHeightKm;
  float fogBaseAltitudeKm;
  float fogAlbedo;

  float fogAnisotropy;
  uint  apHostLightInjection;     // 1: add the host's local-light injection volume
  float apLocalLightIntensity;    // gain on the local-light term

  // Local lights in the compact AerialPerspectiveLight buffer. Zero disables the whole path (the
  // cull pass, the lighting pass's RIS and its shadow ray), so it is the only gate consumers test.
  uint  apLocalLightCount;
  // Cull cluster grid: the froxel grid coarsened by AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE in X and Y.
  uint  apLocalLightTilesX;
  uint  apLocalLightTilesY;
  uint cloudDomeInterleave;
  float cloudDetailLodBias;
  uint cloudDetailLodEnable;

  uint cloudScreenInterleave;
  float cloudReprojectDepthTolerance;
  // Texture slot of each cascade: map m, cascade k in the 3 bits at 16 m + 4 k.
  uint cloudShadowSlots;
  // 1 when the maps are baked and the cloud passes should read them.
  uint cloudShadowLive;

  // Cloud shadow maps (lib/cloud_shadow_map.slangh): [0] the sun map, [1] the horizon map. Each
  // cascade (x..w = cascades 0..3) carries its own baked direction and window, in km on the map
  // plane (cloud frame), because cascades re-bake one at a time.
  vec4 cloudShadowDirX[2];
  vec4 cloudShadowDirY[2];
  vec4 cloudShadowDirZ[2];
  vec4 cloudShadowCenterU[2];
  vec4 cloudShadowHalfU[2];
  vec4 cloudShadowCenterV[2];
  vec4 cloudShadowHalfV[2];
  // The bake dispatch only: map | cascade << 4 | slot << 8, and the first texel row of this chunk.
  uint cloudShadowBakeTarget;
  uint cloudShadowBakeRow0;
  float nightSkyGain;  // rtx.atmosphere.nightGainStops as the factor in force (twilight-faded), for the star field
  // cd/m^2 per unit of NUMOS's pre-exposed radiance (luxPerUnit / R): the AP debug view's absolute
  // scale. Former pad5.
  float apDebugNitsPerUnit;

  // rtx.atmosphere.aerialPerspectiveDebugView (kApDebugView*, lib/aerial_perspective.slangh),
  // and the radiance scale (pre-exposure R) the composite writes its debug color back in.
  uint  apDebugView;
  float apDebugRadianceScale;
  // Pads the struct to a whole 16-byte row.
  float padApDebug0;
  float padApDebug1;

};

NUMOS_SHARED_END
