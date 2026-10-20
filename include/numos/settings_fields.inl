// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// NUMOS settings. One entry per setting; the library owns this list.
// Descriptions are user-facing: hosts show them as tooltips and in their option documentation.
//
// NUMOS_SETTING(type, name, default, "conf key", min, max, flags, "description")
// NUMOS_MOON_SETTING(...) is the same, with the key relative to rtx.atmosphere.moon<N>.
// Deprecated settings carry NUMOS_SETTING_NO_SAVE and are migrated by migrateDeprecated().

#ifndef NUMOS_MOON_SETTING
#define NUMOS_MOON_SETTING(type, name, def, key, mn, mx, flags, desc)
#endif
#ifndef NUMOS_SETTING
#define NUMOS_SETTING(type, name, def, key, mn, mx, flags, desc)
#endif

NUMOS_SETTING(float, sunSize, 0.545f, "rtx.atmosphere.sunSize", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The angular diameter of the sun's disc, in degrees. 0.545 is close to the real sun. The sun's shadow softness follows it unless Sun Shadow Softness is set.")
NUMOS_SETTING(bool, aerialPerspective, true, "rtx.atmosphere.aerialPerspective", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws the air between the camera and every surface: haze, fog, sun shafts and the glow around lamps. Near the camera it uses a scene-shadowed froxel grid, and beyond that an unshadowed far-field table. This is the strongest distance cue in an outdoor scene. With fog and local lights on, it can replace the renderer's own volumetric fog.")
NUMOS_SETTING(float, aerialPerspectiveScale, 0.0f, "rtx.atmosphere.aerialPerspectiveScale", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.unitsPerMeter. An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, cloudScale, 0.0f, "rtx.atmosphere.cloudScale", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.unitsPerMeter and rtx.atmosphere.worldScale. An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, unitsPerMeter, 0.0f, "rtx.atmosphere.unitsPerMeter", 0.0f, NUMOS_NO_MAX, 0,
  "How many game units make one real metre. This measures the game and is not a look setting: for example 100 for an engine that works in centimetres, or 39.37 for one that works in inches. 0 takes the value from rtx.sceneScale. The clouds, sky, haze, fog, local-light glow and lightning all convert with it. To make the sky smaller than the game's real scale, change World Scale and leave this at the true value.")
NUMOS_SETTING(float, worldScale, 1.0f, "rtx.atmosphere.worldScale", 0.001f, 1000.0f, 0,
  "The size of the sky's world relative to the game's. The planet, atmosphere, clouds, wind and lightning are all scaled by it together, so a scaled sky looks like the full-size one, only smaller. The haze, fog and local-light glow follow Haze World Scale instead. 1 is real size. Lower it for a map built much smaller than the place it depicts, where real-size clouds look too high and too large (0.1 makes the sky a tenth of the size).\nSettings given in metres or kilometres (cloud heights and depths, the lightning range) scale with it. Settings in world units (Ground Level, Cloud Vertical Offset, Camera World Override) are the game's own units and do not.")
NUMOS_SETTING(float, hazeWorldScale, 1.0f, "rtx.atmosphere.hazeWorldScale", 0.001f, 1000.0f, 0,
  "The size of the haze's world relative to the game's: World Scale for the haze, height fog, sun shafts and local-light glow alone. 1 is real size, so the game's terrain is hazed at its true distance. With World Scale below 1 and this left at 1, the clouds shrink but distant land is not hazed as if it were farther away. Settings in metres that belong to the haze (fog distances and heights, the froxel range) scale with it.")
NUMOS_SETTING(float, cloudWorldCompression, 1.0f, "rtx.atmosphere.cloudWorldCompression", 0.1f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.worldScale (1 divided by this value). An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, cloudUnitsPerMeter, 0.0f, "rtx.atmosphere.cloudUnitsPerMeter", 0.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: a second, clouds-only units-per-metre. Its ratio to rtx.atmosphere.unitsPerMeter is migrated into rtx.atmosphere.worldScale, which keeps the clouds where they were. Save the config again to stop the notice.")
NUMOS_SETTING(float, aerialPerspectiveWorldCompression, 1.0f, "rtx.atmosphere.aerialPerspectiveWorldCompression", 0.1f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: the haze now shares rtx.atmosphere.worldScale with the clouds. A value that disagrees with the clouds' scale is reported, and the clouds' scale is kept. Save the config again to stop the notice.")
NUMOS_SETTING(float, groundLevelWorldUnits, 0.0f, "rtx.atmosphere.groundLevelWorldUnits", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The height, in the game's world units along its up axis, that counts as ground level (altitude zero) while Ground Level: Auto is off, or before Auto has measured anything. Cloud heights, the height fog and the sky's altitude are measured from it. The easiest way to set it is the Set to Here button, used while standing on ground the game treats as sea level.")
NUMOS_SETTING(bool, groundLevelAuto, true, "rtx.atmosphere.groundLevelAuto", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Measures the ground level from the scene instead of using Ground Level. Rays cast straight down (or ground points the game supplies) build a smoothed map of the ground height around the player, and the ground level is that map under the camera. Cloud bases, the height fog and layer 2 then keep their heights above the local ground as the player travels, while climbing a building or jumping changes nothing. Needs ray-traced scene geometry or ground points from the game. Without either, Ground Level is used.")
NUMOS_SETTING(float, groundSmoothingMeters, 4000.0f, "rtx.atmosphere.groundSmoothingMeters", 100.0f, NUMOS_NO_MAX, 0,
  "The distance, in metres, over which Ground Level: Auto averages the measured ground. Larger values follow only the broad shape of the land and move the cloud deck less as the player travels. Smaller values follow hills and valleys more closely. Changing it starts a fresh measurement.")
NUMOS_SETTING(float, cloudVerticalOffsetWorldUnits, 0.0f, "rtx.atmosphere.cloudVerticalOffsetWorldUnits", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Moves the whole cloud field up or down, in game world units along the up axis. It changes where the clouds sit without changing their size, the ground level or the haze. Center Layer at Player sets it once to place the main layer around the player. Center again after changing World Scale or the layer depth.")
NUMOS_SETTING(float, cloudBaseHeightMeters, 1300.0f, "rtx.atmosphere.cloudBaseHeightMeters", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The height of the main cloud deck's underside above the ground level, in metres.")
NUMOS_SETTING(float, cloudDepthMeters, 2000.0f, "rtx.atmosphere.cloudDepthMeters", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The vertical depth of the main cloud deck, in metres, measured up from its base.")
NUMOS_SETTING(float, cloudLayer2BaseHeightMeters, 5500.0f, "rtx.atmosphere.cloudLayer2BaseHeightMeters", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The height of cloud layer 2's base above the ground level, in metres. 0 or slightly below puts a fog bank on the ground. Ignored while Layer 2 Follows Camera is on.")
NUMOS_SETTING(float, cloudLayer2DepthMeters, 2000.0f, "rtx.atmosphere.cloudLayer2DepthMeters", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The vertical depth of cloud layer 2, in metres. Use 50-500 for ground fog or a dust layer, and a few hundred to 2000 for stratus or a high deck.")
NUMOS_SETTING(bool, useCameraWorldOverride, false, "rtx.atmosphere.useCameraWorldOverride", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Anchors the clouds to rtx.atmosphere.cameraWorldOverride instead of the camera position the renderer sees. Turn it on for games that render relative to the camera, where the camera always appears to sit at the origin and the clouds would otherwise move with the view. The game integration then has to send the real camera position every frame.")
NUMOS_SETTING(Vector3, cameraWorldOverride, Vector3(0.0f, 0.0f, 0.0f), "rtx.atmosphere.cameraWorldOverride", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The camera's world position in the game's own units and axis convention, used as the cloud anchor while Use Camera World Override is on. A game integration normally sends it every frame.")
NUMOS_SETTING(float, cloudAnchorCutKm, 0.5f, "rtx.atmosphere.cloudAnchorCutKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How far, in kilometres, the cloud anchor has to jump in one frame to count as a camera cut, which rebuilds the cloud reflection dome at once. Teleports and loading into a new area move it further than a player can travel in one frame. 0 turns the check off.")
NUMOS_SETTING(int32_t, aerialPerspectiveFroxelSizePixels, 8, "rtx.atmosphere.aerialPerspectiveFroxelSizePixels", 2, 64, 0,
  "The screen size of one froxel of the near-field haze, in render-resolution pixels. 8 gives a 240x135 grid at 1080p. Smaller froxels sharpen sun shafts and lamp glow. GPU cost and memory grow with 1 / size squared.")
NUMOS_SETTING(int32_t, aerialPerspectiveFroxelDepthSlices, 96, "rtx.atmosphere.aerialPerspectiveFroxelDepthSlices", 16, 256, 0,
  "The number of depth slices in the near-field haze, spread from the camera to Froxel Range. More slices give finer detail in depth. GPU cost and memory grow linearly with it.")
NUMOS_SETTING(float, aerialPerspectiveFroxelRangeMeters, 1000.0f, "rtx.atmosphere.aerialPerspectiveFroxelRangeMeters", 10.0f, 50000.0f, 0,
  "How far the near-field haze reaches from the camera, in metres. Inside it the air is shadowed by the scene (sun shafts, dark interiors), lit by local lights and carries the height fog. Beyond it, an unshadowed far field carries the atmosphere to the horizon. Raising it spreads the same slices over a longer distance.")
NUMOS_SETTING(int32_t, aerialPerspectiveHistoryFrames, 32, "rtx.atmosphere.aerialPerspectiveHistoryFrames", 1, 255, 0,
  "How many frames of history each near-field froxel averages. Each frame a froxel traces one sun-or-sky visibility ray and one local-light ray, and the history smooths them into stable shadowing. More frames look smoother, but a moving light or occluder takes longer to settle. Camera cuts reset the history.")
NUMOS_SETTING(float, aerialPerspectiveFroxelUnitsPerMeter, 0.0f, "rtx.atmosphere.aerialPerspectiveFroxelUnitsPerMeter", 0.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: the fog, local-light glow and froxel range now share rtx.atmosphere.worldScale with the clouds. A value that disagrees with the clouds' scale is reported, and the clouds' scale is kept. Save the config again to stop the notice.")
NUMOS_SETTING(bool, aerialPerspectiveLocalLights, true, "rtx.atmosphere.aerialPerspectiveLocalLights", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Lets scene lights such as lamps, torches, muzzle flashes and headlights glow in the near-field haze, alongside the sun and sky. The cost follows how many lights are near each other on screen, not the total light count. With this on, the renderer's own volumetric lighting can be turned off.")
NUMOS_SETTING(bool, aerialPerspectiveLocalLightWindow, false, "rtx.atmosphere.aerialPerspectiveLocalLightWindow", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Fades each local light's glow smoothly to zero at its cull radius, instead of letting it fall off with distance and stop abruptly at that radius. Turn it on for games whose lights are authored with such a falloff window. Local Light Cutoff then sets the reach.")
NUMOS_SETTING(float, aerialPerspectiveLocalLightIntensity, 1.0f, "rtx.atmosphere.aerialPerspectiveLocalLightIntensity", 0.0f, 100.0f, 0,
  "A gain on the glow of local lights in the haze. 1 is physical. Raise it when a game's lights are authored dimmer than the haze that looks right at a distance.")
NUMOS_SETTING(bool, aerialPerspectiveLocalLightShadows, true, "rtx.atmosphere.aerialPerspectiveLocalLightShadows", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Traces a shadow ray from each froxel toward the local light it sampled. Without it, a lamp lights the fog on both sides of the wall it stands behind. With it, light through a doorway casts a real shaft. Needs ray-traced scene geometry.")
NUMOS_SETTING(int32_t, aerialPerspectiveLocalLightMaxCount, 256, "rtx.atmosphere.aerialPerspectiveLocalLightMaxCount", 0, 4096, 0,
  "The maximum number of scene lights the near-field haze uses per frame. The lights that can glow brightest in view are kept. Lower it to save GPU time in scenes with very many lights.")
NUMOS_SETTING(float, aerialPerspectiveLocalLightCutoff, 0.002f, "rtx.atmosphere.aerialPerspectiveLocalLightCutoff", 0.0f, NUMOS_NO_MAX, 0,
  "The glow brightness below which a light counts as not reaching a point. Each light's reach is sized from its own power, so a torch stops after a few metres and a floodlight reaches much further. Lower it if bright lights visibly stop lighting the fog at a fixed distance. Raise it to save GPU time.")
NUMOS_SETTING(bool, aerialPerspectiveSceneShadow, true, "rtx.atmosphere.aerialPerspectiveSceneShadow", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Traces the scene to find how much of the sun and the sky each near-field froxel sees. This gives sun shafts through doorways and foliage, and keeps interiors from filling with sunlit haze. Needs ray-traced scene geometry. Without it the near field is unshadowed.")
NUMOS_SETTING(float, aerialPerspectiveMieAnisotropyMax, 0.8f, "rtx.atmosphere.aerialPerspectiveMieAnisotropyMax", -1.0f, 1.0f, 0,
  "The upper limit on how strongly the haze scatters light forward, used by the haze in front of surfaces but not by the sky. A strongly forward-scattering haze is brightest looking toward the sun, where the far field cannot know that the air is in shadow. Lower it if the haze glows too much toward a sun hidden behind scenery. Values at or above Mie Anisotropy have no effect.")
NUMOS_SETTING(float, fogVisibilityMeters, 0.0f, "rtx.atmosphere.fogVisibilityMeters", 0.0f, NUMOS_NO_MAX, 0,
  "Height fog: the visibility at the fog's base height, in metres, which is the distance at which contrast falls to 2%. 0 means no fog. 50 is thick fog, 1000 mist and 10000 light haze.")
NUMOS_SETTING(float, fogHeightFalloffMeters, 100.0f, "rtx.atmosphere.fogHeightFalloffMeters", 1.0f, NUMOS_NO_MAX, 0,
  "How quickly the height fog thins with height: its density falls by a factor of e every this many metres above the base. Large values give an even fog.")
NUMOS_SETTING(float, fogBaseHeightMeters, 0.0f, "rtx.atmosphere.fogBaseHeightMeters", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The height fog's base, in metres above the ground level. The fog has its full visibility here and thins above it.")
NUMOS_SETTING(float, fogAlbedo, 0.9f, "rtx.atmosphere.fogAlbedo", 0.0f, 1.0f, 0,
  "The share of the height fog's dimming that is scattered light rather than absorbed light. Lower values give a darker, smokier fog.")
NUMOS_SETTING(float, fogAnisotropy, 0.3f, "rtx.atmosphere.fogAnisotropy", -0.95f, 0.95f, 0,
  "How strongly the height fog scatters light forward, from -0.95 to 0.95. Positive values brighten the fog toward the sun and around lamps. Water-droplet fog scatters strongly forward, smoke much less.")
NUMOS_SETTING(float, aerialPerspectiveDepthRangeMeters, 32000.0f, "rtx.atmosphere.aerialPerspectiveDepthRangeMeters", 100.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: the far field always reaches the horizon, and the near field's reach is rtx.atmosphere.aerialPerspectiveFroxelRangeMeters. Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(float, aerialPerspectiveNearFadeStartMeters, 50.0f, "rtx.atmosphere.aerialPerspectiveNearFadeStartMeters", 0.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: the near-field haze needs no near fade. Save the config again to stop the notice.")
NUMOS_SETTING(float, aerialPerspectiveNearFadeEndMeters, 250.0f, "rtx.atmosphere.aerialPerspectiveNearFadeEndMeters", 0.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: the near-field haze needs no near fade. Save the config again to stop the notice.")
NUMOS_SETTING(int32_t, aerialPerspectiveLutResolution, 192, "rtx.atmosphere.aerialPerspectiveLutResolution", 8, 256, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.aerialPerspectiveFroxelSizePixels. Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(int32_t, aerialPerspectiveLutDepthSlices, 32, "rtx.atmosphere.aerialPerspectiveLutDepthSlices", 4, 128, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.aerialPerspectiveFroxelDepthSlices. Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(float, aerialPerspectiveLocalLightUnitsPerMeter, 0.0f, "rtx.atmosphere.aerialPerspectiveLocalLightUnitsPerMeter", 0.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.worldScale, which scales the local-light glow with everything else. A value that disagrees with the clouds' scale is reported, and the clouds' scale is kept. Save the config again to stop the notice.")
NUMOS_SETTING(float, aerialPerspectiveLocalLightShadowRangeMeters, 300.0f, "rtx.atmosphere.aerialPerspectiveLocalLightShadowRangeMeters", 0.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: local lights are shadowed throughout the near field (rtx.atmosphere.aerialPerspectiveFroxelRangeMeters). Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(bool, aerialPerspectiveSeparateVisibility, true, "rtx.atmosphere.aerialPerspectiveSeparateVisibility", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored. Save the config again to stop the notice.")
NUMOS_SETTING(int32_t, aerialPerspectiveVisibilityUpdatePeriod, 1, "rtx.atmosphere.aerialPerspectiveVisibilityUpdatePeriod", 1, 8, NUMOS_SETTING_NO_SAVE,
  "Deprecated: the near field spreads its rays over its history (rtx.atmosphere.aerialPerspectiveHistoryFrames). Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(bool, aerialPerspectiveVisibilityPerSlice, true, "rtx.atmosphere.aerialPerspectiveVisibilityPerSlice", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored. Save the config again to stop the notice.")
NUMOS_SETTING(bool, aerialPerspectiveIntegrateScan, true, "rtx.atmosphere.aerialPerspectiveIntegrateScan", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored. Save the config again to stop the notice.")
NUMOS_SETTING(int32_t, aerialPerspectiveSceneShadowDebug, 0, "rtx.atmosphere.aerialPerspectiveSceneShadowDebug", 0, 2, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored. Save the config again to stop the notice.")
NUMOS_SETTING(float, aerialPerspectiveSceneShadowRangeMeters, 4600.0f, "rtx.atmosphere.aerialPerspectiveSceneShadowRangeMeters", 0.0f, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: the near field is shadowed throughout its range (rtx.atmosphere.aerialPerspectiveFroxelRangeMeters). Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(float, sunShadowSoftnessDeg, 0.0f, "rtx.atmosphere.sunShadowSoftnessDeg", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Sun shadow softness, as the sun light's angular radius in degrees. 0 is physical and follows Sun Size. A value above 0 softens or sharpens shadows without changing the visible sun disc, for soft shadows under a small sun.")
NUMOS_SETTING(float, sunIntensity, 1.09f, "rtx.atmosphere.sunIntensity", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The strength of the sun.")
NUMOS_SETTING(float, sunElevation, 15.0f, "rtx.atmosphere.sunElevation", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The sun's height above the horizon, in degrees. A game integration can set it every frame, which overrides the saved value.")
NUMOS_SETTING(float, sunRotation, 0.0f, "rtx.atmosphere.sunRotation", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The sun's compass direction (azimuth), in degrees. A game integration can set it every frame, which overrides the saved value.")
NUMOS_SETTING(bool, flipUpAxis, false, "rtx.atmosphere.flipUpAxis", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Flips the world's up axis for the sky. Turn it on when the sky renders upside down, with the bright zenith below you and the dark ground half overhead.\nA sky that looks rotated 90 degrees is a different problem: rtx.zUp does not match the game. To tell them apart, stop the time cycle and set a mid-height sun: a zenith off to one side means rtx.zUp, a zenith under your feet means this setting.\nFlipping the axis also reverses the direction the sun travels across the sky. Sun Rotation and North Offset cannot undo that, so the sun's arc has to be placed again afterwards.")
NUMOS_SETTING(bool, timeCycleEnable, false, "rtx.atmosphere.timeCycleEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Moves the sun with a built-in clock instead of Sun Elevation and Sun Rotation. Use it for games without a day and night cycle of their own. While on, the clock controls the sun, and those two settings (and any values a game sends for them) are ignored.")
NUMOS_SETTING(float, timeOfDayHours, 12.0f, "rtx.atmosphere.timeOfDayHours", 0.0f, 24.0f, 0,
  "The time of day in hours, from 0 to 24. 12 is solar noon. Around an equinox, 6 is about sunrise and 18 about sunset. While the time cycle runs, this is the start time, and editing it moves the clock. While the cycle is off it still places the sun, so it works as a manual time-of-day control.")
NUMOS_SETTING(float, dayLengthMinutes, 24.0f, "rtx.atmosphere.dayLengthMinutes", 0.01f, NUMOS_NO_MAX, 0,
  "How many real minutes one full 24-hour day lasts while the time cycle runs. 24 gives one minute per in-game hour. Raise it for a slower day.")
NUMOS_SETTING(float, latitudeDegrees, 45.0f, "rtx.atmosphere.latitudeDegrees", -90.0f, 90.0f, 0,
  "The observer's latitude in degrees, positive north. It sets how high the sun climbs and how tilted its path is. At 0 (the equator) the sun passes almost straight overhead. High latitudes keep it low, with long, shallow sunrises and sunsets.")
NUMOS_SETTING(int32_t, dayOfYear, 80, "rtx.atmosphere.dayOfYear", 1, 365, 0,
  "The day of the year, from 1 to 365, which sets the season. 80 is the March equinox (sunrise due east, sunset due west, a 12-hour day), 172 the June solstice and 355 the December solstice.")
NUMOS_SETTING(float, northOffsetDegrees, 0.0f, "rtx.atmosphere.northOffsetDegrees", -360.0f, 360.0f, 0,
  "Rotates the sun's whole path so that north in the sky lines up with north in the game world. Adjust it until the sun rises in the direction the game treats as east.")
NUMOS_SETTING(float, seaLevelWorldKm, 0.0f, "rtx.atmosphere.seaLevelWorldKm", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.groundLevelWorldUnits. An existing value is converted automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, altitudeScale, 1.0f, "rtx.atmosphere.altitudeScale", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: a vertical-only scale is no longer supported. Set rtx.atmosphere.unitsPerMeter to the game's true scale instead. A value other than 1 is reported and ignored.")
NUMOS_SETTING(float, viewAltitudeKm, 0.0f, "rtx.atmosphere.viewAltitudeKm", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: folded into rtx.atmosphere.groundLevelWorldUnits. An existing value is converted automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, airDensity, 1.0f, "rtx.atmosphere.airDensity", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "A multiplier on the density of air molecules. 1 is a clear Earth sky. Higher values give a deeper blue sky and redder sunsets.")
NUMOS_SETTING(float, aerosolDensity, 1.1f, "rtx.atmosphere.aerosolDensity", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "A multiplier on the density of dust and haze particles. 1 is typical. Higher values give a hazier, whiter sky and a larger glow around the sun.")
NUMOS_SETTING(float, ozoneDensity, 1.0f, "rtx.atmosphere.ozoneDensity", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "A multiplier on the density of the ozone layer. 1 is typical. Ozone gives the twilight sky its blue colour.")
NUMOS_SETTING(float, planetRadius, 6371.0f, "rtx.atmosphere.planetRadius", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The planet's radius, in kilometres. 6371 is Earth.")
NUMOS_SETTING(float, atmosphereThickness, 100.0f, "rtx.atmosphere.atmosphereThickness", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The thickness of the atmosphere, in kilometres. 100 is Earth.")
NUMOS_SETTING(float, mieAnisotropy, 0.8f, "rtx.atmosphere.mieAnisotropy", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How strongly the haze scatters light forward, from -1 to 1. It sets the size and brightness of the glow around the sun. 0.8 is typical of Earth's haze. The sun's disc itself is drawn separately, at Sun Size.")
NUMOS_SETTING(Vector3, rayleighScattering, Vector3(5.802e-3f, 13.558e-3f, 33.1e-3f), "rtx.atmosphere.rayleighScattering", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The air's base scattering coefficients per colour channel, per kilometre. They give the sky its blue colour.")
NUMOS_SETTING(Vector3, mieScattering, Vector3(3.996e-3f, 3.996e-3f, 3.996e-3f), "rtx.atmosphere.mieScattering", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The haze's base scattering coefficients per colour channel, per kilometre.")
NUMOS_SETTING(Vector3, mieAbsorption, Vector3(4.4e-3f, 4.4e-3f, 4.4e-3f), "rtx.atmosphere.mieAbsorption", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The haze's base absorption coefficients per colour channel, per kilometre. Real haze absorbs about as much light as it scatters. Raising this relative to Mie Scattering darkens the haze, and making it differ between channels tints it: this is how dust becomes brown and dim, or smoke grey and dark, rather than only denser. Scaled by Aerosol Density.")
NUMOS_SETTING(Vector3, airTint, Vector3(1.0f, 1.0f, 1.0f), "rtx.atmosphere.airTint", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "A per-channel multiplier on the air's scattering. 1, 1, 1 is Earth's air. Change it for an alien or polluted sky colour. Weather can blend it.")
NUMOS_SETTING(Vector3, aerosolTint, Vector3(1.0f, 1.0f, 1.0f), "rtx.atmosphere.aerosolTint", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "A per-channel multiplier on the haze's scattering and absorption together. 1, 1, 1 leaves the haze as configured. Lowering a channel makes the haze affect that colour less, so 1, 0.8, 0.55 gives a warm dust storm. Weather and game integrations can set it.")
NUMOS_SETTING(Vector3, ozoneAbsorption, Vector3(0.650e-3f, 1.881e-3f, 0.085e-3f), "rtx.atmosphere.ozoneAbsorption", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The ozone layer's base absorption coefficients per colour channel, per kilometre.")
NUMOS_SETTING(float, ozoneLayerAltitude, 25.0f, "rtx.atmosphere.ozoneLayerAltitude", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The height of the ozone layer's densest point, in kilometres.")
NUMOS_SETTING(float, ozoneLayerWidth, 15.0f, "rtx.atmosphere.ozoneLayerWidth", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The half-thickness of the ozone layer, in kilometres. Together with Ozone Density it sets the total amount of ozone.")
NUMOS_SETTING(Vector3, sunIlluminance, Vector3(15.0f, 15.0f, 15.0f), "rtx.atmosphere.sunIlluminance", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The sun's base colour and brightness.")
NUMOS_SETTING(bool, radianceAutoScale, true, "rtx.atmosphere.radianceAutoScale", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Scales everything the sky outputs by a power of two that follows the light level, so a night sky, which is about a hundred-millionth as bright as the day sky, keeps its precision in 16-bit render targets. Daylight always uses a scale of 1. Turn it off only for debugging: a moonless night then turns black in 16-bit targets.")
NUMOS_SETTING(float, multiScatterPhysicalStrength, 1.0f, "rtx.atmosphere.multiScatterPhysicalStrength", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Blends between a simple approximation of light that scatters many times in the air (0) and the physical calculation (1). 1 gives a natural gradient from the zenith to a warm horizon. Lower values flatten the gradient and turn the horizon bluer.")
NUMOS_SETTING(float, multiScatterStrength, 1.0f, "rtx.atmosphere.multiScatterStrength", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: the multiple scattering is always physical. Save the config again to stop the notice.")
NUMOS_SETTING(float, sunsetSaturation, 1.0f, "rtx.atmosphere.sunsetSaturation", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: colour grading belongs in the tonemapper. Save the config again to stop the notice.")
NUMOS_SETTING(float, skyIndirectRadianceScale, 1.0f, "rtx.atmosphere.skyIndirectRadianceScale", 0.0f, NUMOS_NO_MAX, 0,
  "A multiplier on the sky light that reaches surfaces through diffuse bounces. 1 is physical. Raise it when shaded areas look too dull next to sunlit ones. Reflections and the visible sky keep their physical brightness, so reflections still match the sky.")
NUMOS_SETTING(bool, nightSkyStars, true, "rtx.atmosphere.nightSkyStars", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws the real night sky: about 25,700 catalogue stars as points and the fainter stars as the Milky Way, at their real brightness and colour. The stars turn with the date, time and latitude, dim and redden near the horizon, twinkle, and light the clouds and the ground. Needs the night sky data files. Turn it off for a game that draws its own stars.")
NUMOS_SETTING(bool, cloudDiskAureole, true, "rtx.atmosphere.cloudDiskAureole", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws the glow (aureole) that forms around the sun and the moons behind thin cloud, where cloud droplets bend light a few degrees off its path. Behind thin cloud the disc becomes a bright glow, and behind thicker cloud only the glow remains. Off, the disc is only dimmed.")
NUMOS_SETTING(float, cloudDropletDiameter, 20.0f, "rtx.atmosphere.cloudDropletDiameter", 5.0f, 50.0f, 0,
  "The typical cloud droplet size, in micrometres, from 5 to 50. 20 is typical of stratocumulus. Smaller droplets, as in fresh cumulus and fog, give a wider glow around the sun and the moons.")
NUMOS_SETTING(float, nightGainStops, 0.0f, "rtx.atmosphere.nightGainStops", 0.0f, 16.0f, 0,
  "Night Gain, in stops. 0 is physical. Brightens all natural night light together (the moons, the moonlit sky, starlight, airglow) so the sky, clouds and ground stay consistent.\nSmall values, about +1 to +3, make up for the eye's dark adaptation, which a display cannot reproduce, so a physical night looks darker on screen than in reality. Large values, up to +10 and beyond, balance the night against a game whose own lights are much brighter than physical.\nThe sun, twilight and lightning are not changed, and the gain fades in through twilight so the sky never brightens after sunset.")
NUMOS_SETTING(float, starScintillation, 1.0f, "rtx.atmosphere.starScintillation", 0.0f, 4.0f, 0,
  "How strongly the stars twinkle, relative to a typical night (1). Stars near the horizon twinkle most and can flash colours, while high stars stay steady. 0 turns twinkling off.")
NUMOS_SETTING(float, directionalLightRadianceScale, 1.0f, "rtx.atmosphere.directionalLightRadianceScale", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "A multiplier on the brightness of the sun and moon lights the sky adds to the scene. Adjust it if sunlight and moonlight on surfaces read too bright or too dim overall.")
NUMOS_SETTING(bool, realMoonEnabled, true, "rtx.atmosphere.realMoonEnabled", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws Earth's Moon where it really is for the configured date, time and latitude. Its phase, brightness, earthshine and eclipses follow from where the sun is. It lights the sky, the clouds and the ground. Turn it off for a game that places its own moon with rtx.atmosphere.moon0-3.")
NUMOS_SETTING(float, realMoonAngularDiameter, 0.518f, "rtx.atmosphere.realMoonAngularDiameter", 0.05f, 20.0f, 0,
  "The drawn size of the real moon, in degrees. 0.518 is the Moon's real size. A larger disc gives the same total moonlight, so its surface looks dimmer.")
NUMOS_SETTING(int32_t, calendarYear, 2026, "rtx.atmosphere.calendarYear", 1900, 2100, 0,
  "The year of the date the sky is drawn for, used with Day of Year and Time of Day. The real moon's position and phase, and the stars, follow it.")
NUMOS_SETTING(bool, cloudEnabled, true, "rtx.atmosphere.cloudEnabled", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws the clouds.")
NUMOS_SETTING(float, cloudDensity, 20.0f, "rtx.atmosphere.cloudDensity", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How dense and opaque the clouds are. Higher values give darker, more solid clouds.")
NUMOS_SETTING(float, cloudAltitude, 1.3f, "rtx.atmosphere.cloudAltitude", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.cloudBaseHeightMeters. An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(Vector3, cloudColor, Vector3(0.89f, 0.92f, 1.0f), "rtx.atmosphere.cloudColor", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The base colour of the clouds.")
NUMOS_SETTING(float, cloudWindSpeed, 0.02f, "rtx.atmosphere.cloudWindSpeed", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How fast the clouds drift, in kilometres per second.")
NUMOS_SETTING(float, cloudWindDirection, 45.0f, "rtx.atmosphere.cloudWindDirection", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The direction the clouds drift toward, in degrees (0 = +X, 90 = +Z).")
NUMOS_SETTING(float, nubis3JitterAnimateKm, 1.0e9f, "rtx.atmosphere.nubis3JitterAnimateKm", 0.0f, NUMOS_NO_MAX, 0,
  "The distance, in kilometres, beyond which the cloud sampling noise stops changing every frame. By default it never stops. Lower values make distant cloud noise hold still on screen instead of shimmering.")
NUMOS_SETTING(float, cloudEvolutionSpeed, 0.002f, "rtx.atmosphere.cloudEvolutionSpeed", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How fast cloud detail rises through each cloud, in kilometres per second, which makes cumulus look like it is building. It does not move the clouds themselves. 0 stops the rise.")
NUMOS_SETTING(float, cloudBoilSpeed, 0.002f, "rtx.atmosphere.cloudBoilSpeed", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How fast cloud detail is sheared downwind, in kilometres per second. The tops stream downwind while the bases stay put. 0 turns the shear off.")
NUMOS_SETTING(float, cloudShadowStrength, 1.0f, "rtx.atmosphere.cloudShadowStrength", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How strongly the clouds shade the sunlight on the ground and in the air, from 0 to 1. 1 is physical and 0 removes cloud shadows.")
NUMOS_SETTING(bool, cloudProfilingLog, false, "rtx.atmosphere.cloudProfilingLog", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Writes cloud GPU timings to the log every 120 frames.")
NUMOS_SETTING(int32_t, cloudProfilingMode, 0, "rtx.atmosphere.cloudProfilingMode", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "A cloud diagnostic mode. 0: Normal. 9: Reference, a slow, exact path tracer for the cloud lighting, for comparing against the normal mode.")
NUMOS_SETTING(uint32_t, cloudViewSamples, 32, "rtx.atmosphere.cloudViewSamples", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The base number of march steps through the cloud layer, from 1 to 32, used while Cloud Step Spacing is 0. Higher values look better and cost more GPU time.")
NUMOS_SETTING(float, cloudThickness, 3.05f, "rtx.atmosphere.cloudThickness", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.cloudDepthMeters. An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, cloudSkyAmbientCloudOcclusionStrength, 1.0f, "rtx.atmosphere.cloudSkyAmbientCloudOcclusionStrength", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much the clouds change the sky light in the air, from 0 to 1: they hide the sky behind them and add their own scattered light. 1 is physical. 0 ignores cloud cover and is meant for debugging.")
NUMOS_SETTING(uint32_t, cloudMultiScatterOctaves, 3, "rtx.atmosphere.cloudMultiScatterOctaves", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How many orders of light scattering inside the clouds are approximated, from 1 to 4. 3 is a good balance. 1 keeps only light scattered once, which makes cloud interiors dark.")
NUMOS_SETTING(float, cloudTypeMean, 0.0f, "rtx.atmosphere.cloudTypeMean", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The average cloud type across the sky, from 0 to 1: 0 is stratus, 0.5 stratocumulus and 1 cumulus.")
NUMOS_SETTING(float, cloudTypeSpread, 0.54f, "rtx.atmosphere.cloudTypeSpread", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much the cloud type varies across the sky, from 0 (the same everywhere) to 1 (the full range).")
NUMOS_SETTING(float, cloudTypeNoiseScale, 0.0034f, "rtx.atmosphere.cloudTypeNoiseScale", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How large the regions of different cloud type are. Smaller values give larger regions.")
NUMOS_SETTING(float, cloudCoverageMean, 0.54f, "rtx.atmosphere.cloudCoverageMean", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The average share of the sky covered by cloud, from 0 (clear) to 1 (overcast).")
NUMOS_SETTING(float, cloudCoverageSpread, 0.0f, "rtx.atmosphere.cloudCoverageSpread", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much the cloud coverage varies across the sky, from 0 (the same everywhere) to 1 (the full range).")
NUMOS_SETTING(float, cloudCoverageNoiseScale, 0.00257732f, "rtx.atmosphere.cloudCoverageNoiseScale", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How large the regions of different coverage are. Smaller values give larger regions.")
NUMOS_SETTING(float, cloudNoiseTileKm, 12.0f, "rtx.atmosphere.cloudNoiseTileKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How far, in kilometres, the cloud detail pattern runs before it repeats. Smaller values show repetition more. Larger values give coarser detail. 6 to 24 works well.")
NUMOS_SETTING(bool, cloudHexTilingEnable, true, "rtx.atmosphere.cloudHexTilingEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Hides repetition in the cloud pattern by blending randomly offset, rotated and mirrored copies of it. Turn it off only to see the plain repeating pattern.")
NUMOS_SETTING(float, cloudCellSizeKm, 3.65f, "rtx.atmosphere.cloudCellSizeKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The average size of a cloud cluster, in kilometres, from 0.5 to 6. Smaller values give many small clouds, larger values fewer and broader banks.")
NUMOS_SETTING(float, cloudColumnTopVariation, 0.45f, "rtx.atmosphere.cloudColumnTopVariation", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much cloud tops vary in height, from 0 to 1. 0 puts all tops at one height, a flat deck. Higher values give a varied skyline.")
NUMOS_SETTING(float, cloudColumnTopShape, 0.4f, "rtx.atmosphere.cloudColumnTopShape", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How cloud tops rise, from 0.1 to 2. Low values let thin cluster edges tower too, for blockier clouds. High values let only dense cores rise, for domed tops and feathered edges.")
NUMOS_SETTING(float, cloudColumnBaseVariation, 0.12f, "rtx.atmosphere.cloudColumnBaseVariation", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much cloud bases rise and fall, as a share of the layer depth, from 0 to 0.4. 0 gives perfectly flat bases.")
NUMOS_SETTING(float, cloudColumnFeather, 0.35f, "rtx.atmosphere.cloudColumnFeather", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How soft the edges of cloud clusters are, from 0.05 to 1. Low values give crisp, solid clouds. High values give soft, wispy edges.")
NUMOS_SETTING(float, nvdfNominalCoverage, 0.0f, "rtx.atmosphere.nvdfNominalCoverage", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The cloud coverage the cloud shape field is built for, from 0 to 1. 0 (Auto) follows the current coverage and rebuilds the field in the background when it changes. A fixed value is meant for debugging and look tuning.")
NUMOS_SETTING(float, nvdfProfileDepthKm, 0.12f, "rtx.atmosphere.nvdfProfileDepthKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How far into a cloud, in kilometres, its density takes to build up from the edge, from 0.1 to 3. Small values give dense, hard-edged clouds. Large values give soft, translucent edges.")
NUMOS_SETTING(float, nvdfCoverageOffsetKm, 0.2f, "rtx.atmosphere.nvdfCoverageOffsetKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much the clouds grow or shrink, in kilometres, per unit of coverage change from the coverage the shape field was built for, from 0 to 4. Higher values make coverage changes grow clouds faster, so they merge sooner at high coverage.")
NUMOS_SETTING(float, nubis3ErosionStrength, 0.58f, "rtx.atmosphere.nubis3ErosionStrength", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How strongly the detail noise carves into the clouds, from 0 to 2. 0 gives smooth blobs, 1 the standard look and higher values ragged, heavily carved clouds.")
NUMOS_SETTING(float, cloudBodyBillowStrength, 2.0f, "rtx.atmosphere.cloudBodyBillowStrength", 0.0f, 3.0f, 0,
  "How far the billows push the cloud surface in and out, as a multiple of Billow Size. 0 gives smooth towers. Changing it rebuilds the cloud shape field.")
NUMOS_SETTING(float, cloudBodyBillowSizeKm, 1.2f, "rtx.atmosphere.cloudBodyBillowSizeKm", 0.2f, 4.0f, 0,
  "The size of the largest billows, in kilometres. Three finer sizes follow at a half, a quarter and an eighth of it. Keep the finest above about 0.1 km. Changing it rebuilds the cloud shape field.")
NUMOS_SETTING(float, cloudCurlStrengthKm, 0.12f, "rtx.atmosphere.cloudCurlStrengthKm", 0.0f, 1.0f, 0,
  "How far, in kilometres, the detail is swirled sideways at the cloud base. It tears the base into wind-torn wisps and keeps the detail from looking stamped on. 0 turns it off.")
NUMOS_SETTING(float, cloudDiffuseSunStrength, 0.0f, "rtx.atmosphere.cloudDiffuseSunStrength", 0.0f, 1.0f, 0,
  "How much sunlight reaches the shaded parts of clouds after passing through the cloud above them, from 0 to 1. 0 keeps deep shadows under and between clouds. 1 lights grey, overcast or stormy skies the way thick cloud passes light, neutral grey rather than blue. The overcast, drizzle and storm weather presets set 1.")
NUMOS_SETTING(float, cloudBodyBillowGain, 0.65f, "rtx.atmosphere.cloudBodyBillowGain", 0.0f, 1.0f, 0,
  "The strength of each finer billow size relative to the one above it. 0.5 lets the large billows dominate, for smooth lobes. Toward 0.8, small billows are nearly as strong as large ones, for a cauliflower look. Changing it rebuilds the cloud shape field.")
NUMOS_SETTING(float, cloudLightingLodThreshold, 0.05f, "rtx.atmosphere.cloudLightingLodThreshold", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Saves GPU time by reusing the lighting of nearby cloud samples for samples that barely show, deep inside a cloud or far away, from 0 to 0.25. 0 lights every sample. Raise it for speed. Too high a value can flatten cloud shading.")
NUMOS_SETTING(float, nvdfStepScale, 0.95f, "rtx.atmosphere.nvdfStepScale", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How far the cloud march jumps through empty air, as a share of the distance to the nearest cloud, from 0 to 0.95. Higher values save GPU time, especially toward the horizon. 0 steps evenly. Lower it if cloud outlines show onion-like banding.")
NUMOS_SETTING(float, nubis3AdaptiveStepKm, 0.025f, "rtx.atmosphere.nubis3AdaptiveStepKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The smallest cloud march step, in kilometres, from 0 to 0.2. Steps start this fine near the camera to resolve close detail and grow with distance. 0 uses an even step length everywhere.")
NUMOS_SETTING(float, cloudViewStepKm, 0.25f, "rtx.atmosphere.cloudViewStepKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The target spacing of cloud samples, in kilometres, from 0 to 4. Larger spacing saves GPU time but can lose detail or miss thin clouds. Max Cloud Samples separately limits the steps per ray. 0 uses the fixed step count of Cloud View Samples.")
NUMOS_SETTING(uint32_t, cloudViewSamplesMax, 128, "rtx.atmosphere.cloudViewSamplesMax", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The maximum number of march steps per ray through the main cloud layer, from 2 to 256. Rays that leave the clouds or become opaque stop earlier. Lower it to limit the worst-case GPU cost. It has no effect while Cloud Step Spacing is 0.")
NUMOS_SETTING(bool, lightningEnable, true, "rtx.atmosphere.lightningEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Turns lightning on or off. While on, Lightning Strikes per Minute sets how often it strikes, and the storm weather presets raise that rate. Turn it off to silence lightning everywhere, including the storm presets and the Test Strike button.")
NUMOS_SETTING(float, lightningStrikesPerMinute, 0.0f, "rtx.atmosphere.lightningStrikesPerMinute", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The average number of lightning strikes per minute, from 0 to 60. The gaps between strikes are random, so strikes cluster and pause naturally. 0 means no automatic strikes. Test Strike still works.")
NUMOS_SETTING(float, lightningLuminousIntensity, 2e8f, "rtx.atmosphere.lightningLuminousIntensity", 0.0f, NUMOS_NO_MAX, 0,
  "The peak brightness of a lightning flash, in candela. It drives the glow in the clouds, the glow in the haze and the light on the scene. The default is realistic: a flash lights the ground a few kilometres away like a dim overcast day, outshines everything at night, and is barely visible against a daylit cloud. Raise it for a dramatic daytime storm.")
NUMOS_SETTING(bool, lightningSceneLight, true, "rtx.atmosphere.lightningSceneLight", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Adds each flash to the scene as a light at the strike. Off, the flash lights only the clouds and the air.")
NUMOS_SETTING(float, lightningFlashIntensity, 50.0f, "rtx.atmosphere.lightningFlashIntensity", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.lightningLuminousIntensity. Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(float, lightningSceneLightIntensity, 2000.0f, "rtx.atmosphere.lightningSceneLightIntensity", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.lightningLuminousIntensity (its strength) and rtx.atmosphere.lightningSceneLight (on or off). Ignored. Save the config again to stop the notice.")
NUMOS_SETTING(float, lightningRangeKm, 15.0f, "rtx.atmosphere.lightningRangeKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The farthest distance from the camera a strike can land, in kilometres, from 1.5 to 30. Strikes spread evenly over the area between 1 km and this distance.")
NUMOS_SETTING(Vector3, lightningColor, Vector3(0.72f, 0.78f, 1.0f), "rtx.atmosphere.lightningColor", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The colour of a lightning flash, in linear RGB, for both the cloud glow and the scene light. Only the hue counts. The brightness is set by Lightning Luminous Intensity.")
NUMOS_SETTING(float, cloudPhaseG1, 0.8f, "rtx.atmosphere.cloudPhaseG1", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How strongly clouds scatter light forward. It creates the bright silver lining at the edges of clouds in front of the sun.")
NUMOS_SETTING(uint32_t, cloudSunOctaveCount, 6, "rtx.atmosphere.cloudSunOctaveCount", 1, 8, 0,
  "How many orders of sunlight scattering inside the clouds are approximated, from 1 to 8. Dense clouds (high Cloud Density) need more to stay bright inside.")
NUMOS_SETTING(float, cloudSunOctaveA, 0.45f, "rtx.atmosphere.cloudSunOctaveA", 0.0f, 1.0f, 0,
  "How much less each further order of scattered sunlight is dimmed by the cloud. Lower values let more light through, for brighter, softer cloud interiors.")
NUMOS_SETTING(float, cloudSunOctaveB, 0.55f, "rtx.atmosphere.cloudSunOctaveB", 0.0f, 1.0f, 0,
  "How much each further order of scattered sunlight contributes. Higher values add more scattered light overall. Keep it at or below Octave A to stay plausible.")
NUMOS_SETTING(float, cloudSunOctaveC, 0.5f, "rtx.atmosphere.cloudSunOctaveC", 0.0f, 1.0f, 0,
  "How directional each further order of scattered sunlight is. Lower values spread the later orders more evenly in all directions.")
NUMOS_SETTING(float, fogNearFadeMeters, 40.0f, "rtx.atmosphere.fogNearFadeMeters", 0.0f, 1000.0f, 0,
  "A clear zone around the camera for ground-level fog. The height fog and cloud layer 2 fade in from nothing at the camera to full density at this distance, so the player's surroundings stay readable in thick fog. It is a readability aid, not physical. 0 turns it off.")
NUMOS_SETTING(float, groundAlbedo, 0.3f, "rtx.atmosphere.groundAlbedo", 0.0f, 1.0f, 0,
  "The share of light the ground reflects. Light bounced off the ground brightens the lower sky, lights the ground seen below the horizon and lights cloud bases from below. Set it to the scene's typical ground: snow lights cloud bases more than forest or sea.")
NUMOS_SETTING(float, cloudGroundAlbedo, 0.2f, "rtx.atmosphere.cloudGroundAlbedo", 0.0f, 1.0f, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.groundAlbedo. An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(bool, cloudSecondaryLutEnable, true, "rtx.atmosphere.cloudSecondaryLutEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Gives reflections and indirect light their clouds from a small sky dome rebuilt every frame instead of marching the clouds per ray. This saves a lot of GPU time on cloudy skies. Off, reflections and indirect light see a cloudless sky.")
NUMOS_SETTING(float, cloudHistoryDepthTolerance, 0.1f, "rtx.atmosphere.cloudHistoryDepthTolerance", 0.0f, 1.0f, 0,
  "How different the distance to the surface behind a cloud pixel may be for the cloud filters to still reuse a sample there, as a share of that distance. Lower values reject more history next to moving objects, which avoids smearing at the cost of more noise.")
NUMOS_SETTING(float, cloudTemporalHistoryWeight, 0.95f, "rtx.atmosphere.cloudTemporalHistoryWeight", 0.0f, 0.98f, 0,
  "How much of the previous frames the cloud filter keeps while the camera or the clouds move. 0.95 averages about 20 frames. Higher values hide more noise but smear more in motion. 0 turns the filter off and shows the raw, noisy march.")
NUMOS_SETTING(int32_t, cloudTemporalStillSamples, 64, "rtx.atmosphere.cloudTemporalStillSamples", 1, 1024, 0,
  "How many frames each cloud pixel averages while the view and the clouds are still. Higher values give calmer still images but take longer to settle after a change the filter does not detect.")
NUMOS_SETTING(int32_t, cloudMarchRate, 0, "rtx.atmosphere.cloudMarchRate", 0, 3, 0,
  "How many cloud pixels are marched each frame. The others are rebuilt from neighbouring samples and the cloud filter's history.\n0: Auto, chosen from the render and output resolution: quarter rate at native resolution and mild upscaling with at least 720 render lines, full rate otherwise.\n1: Full, every pixel every frame.\n2: Quarter, one pixel of each 2x2 block per frame, for a quarter of the cost. A still view looks the same as at full rate.\n3: Sixteenth, one pixel of each 4x4 block per frame, for a sixteenth of the cost. Noisy and soft in motion.\nThe quarter and sixteenth rates need Cloud Temporal History Weight above 0.")
NUMOS_SETTING(bool, cloudQuarterMarch, true, "rtx.atmosphere.cloudQuarterMarch", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.cloudMarchRate (2 = Quarter, 1 = Full, 0 = Auto). An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(bool, cloudSixteenthMarch, false, "rtx.atmosphere.cloudSixteenthMarch", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.cloudMarchRate = 3 (Sixteenth). An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, cloudSpatialDenoise, 0.1f, "rtx.atmosphere.cloudSpatialDenoise", 0.0f, 1.0f, 0,
  "An edge-aware blur on the clouds after the temporal filter, for the noise left where there is little history, such as while flying or inside a cloud. The value is the brightness difference, as a share, that still gets smoothed: 0.1 is about 10%. It changes only the displayed image, not the history. 0 turns it off.")
NUMOS_SETTING(bool, cloudShadowMapEnable, true, "rtx.atmosphere.cloudShadowMapEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Stores the clouds' shading toward the sun in a shadow map, rebuilt as the sun moves, instead of marching toward the sun for every cloud sample. It is much faster. Turn it off only to compare against the slow reference.")
NUMOS_SETTING(float, cloudShadowMapWindowKm, 6.0f, "rtx.atmosphere.cloudShadowMapWindowKm", 1.0f, 50.0f, 0,
  "The half-width, in kilometres, of the most detailed cloud shadow map level around the camera. Each further level is about 3.3 times wider, and the last reaches the horizon.")
NUMOS_SETTING(float, cloudShadowMapRebakeDegrees, 0.1f, "rtx.atmosphere.cloudShadowMapRebakeDegrees", 0.0f, 10.0f, 0,
  "How many degrees the sun has to move before the cloud shadow map is rebuilt. The map is also rebuilt when the camera moves a quarter of its width or the clouds change. 0 rebuilds it continuously.")
NUMOS_SETTING(int32_t, cloudShadowMapBakeRowsPerFrame, 32, "rtx.atmosphere.cloudShadowMapBakeRowsPerFrame", 8, 256, 0,
  "Cloud shadow map rebuilds are spread over several frames, this many rows (out of 256) per frame. Lower values make frames cheaper but follow a moving sun or camera more slowly.")
NUMOS_SETTING(float, cloudShadowMapMaxLagDegrees, 0.25f, "rtx.atmosphere.cloudShadowMapMaxLagDegrees", 0.0f, 10.0f, 0,
  "While the sun moves fast, such as with a sped-up time cycle, the shadow map is rebuilt faster so it trails the sun by at most this many degrees. With a slow sun it costs nothing extra. 0 always uses Bake Rows per Frame.")
NUMOS_SETTING(int32_t, cloudPerformancePreset, 0, "rtx.atmosphere.cloudPerformancePreset", 0, 1, 0,
  "A switch for cloud shortcuts that are faster but look slightly different.\n0: Quality.\n1: Performance. Each cloud sample takes its sun shadow from a simpler shadow map. Fine billow detail is softer, and billow tops look slightly brighter when the sun is low.\nThe cloud march rate is set separately by Cloud March Rate.")
NUMOS_SETTING(int32_t, cloudSecondaryLutInterleaveMode, 2, "rtx.atmosphere.cloudSecondaryLutInterleaveMode", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much of the cloud reflection dome is updated each frame. The rest is at most a few frames old. The whole dome is still rebuilt when the clouds change noticeably or the camera cuts.\n0: All of it every frame.\n1: Half.\n2: A quarter.")
NUMOS_SETTING(int32_t, cloudDetailLodMode, 0, "rtx.atmosphere.cloudDetailLodMode", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Blurs cloud detail by sample spacing in the cloud march and the reflection dome. It changes cloud shapes as well as fine texture.\n0: Off, the native look.\n1: Off, the same as 0.\n2: Always on.")
NUMOS_SETTING(float, cloudDetailLodBias, -3.0f, "rtx.atmosphere.cloudDetailLodBias", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The blur bias used while Cloud Detail LOD Mode is on. Negative values keep more detail, positive values blur further. It has no effect while the mode is off.")
NUMOS_SETTING(float, cloudVoxelGridRebakeGranularityKm, 0.1f, "rtx.atmosphere.cloudVoxelGridRebakeGranularityKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How far, in kilometres, the clouds or the camera have to move before the clouds count as changed. A change restarts the gradual updates of the reflection dome and the cloud history. 0 counts any movement.")
NUMOS_SETTING(bool, debugDispatchCloudRender, true, "rtx.atmosphere.debugDispatchCloudRender", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Diagnostic: runs the on-screen cloud pass. It is skipped anyway while the clouds or Cloud Render are off and no debug view is shown. Turn it off to measure the pass's cost. The clouds then freeze in place.")
NUMOS_SETTING(bool, debugDispatchCloudSkyTransmittance, true, "rtx.atmosphere.debugDispatchCloudSkyTransmittance", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Diagnostic: runs the small per-frame pass that measures how much the clouds hide the sky. Turn it off to measure its cost.")
NUMOS_SETTING(bool, debugDispatchSkyLuts, true, "rtx.atmosphere.debugDispatchSkyLuts", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Diagnostic: rebuilds the sky's lookup tables when the sun or the atmosphere changes. Turn it off to freeze them and measure their cost. The sky then stops following the sun.")
NUMOS_SETTING(bool, debugEnableSkyMissShading, true, "rtx.atmosphere.debugEnableSkyMissShading", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Diagnostic: shades rays that reach the sky. Turn it off to draw a flat grey sky and measure the cost of sky shading.")
NUMOS_SETTING(float, skyViewRebakeGranularityDeg, 0.1f, "rtx.atmosphere.skyViewRebakeGranularityDeg", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How many degrees the sun or a moon has to move before the sky is recalculated. 0.1 follows the sun smoothly at a small cost. 0 recalculates every frame while the sun moves. Other sky changes always recalculate at once.")
NUMOS_SETTING(float, skyViewAltitudeRebakeGranularityKm, 0.05f, "rtx.atmosphere.skyViewAltitudeRebakeGranularityKm", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How far, in kilometres, the camera has to climb or descend before the sky is recalculated for the new height. The default is 0.05 (50 m). 0 never recalculates for height, so the sky stays as seen from where it was last calculated.")
NUMOS_SETTING(bool, skyLutCacheKeySplitEnable, true, "rtx.atmosphere.skyLutCacheKeySplitEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Recalculates each part of the sky only when its own inputs change, so a moving sun updates only the small sky view table. The image is unchanged. Turn it off only to compare against recalculating everything together.")
NUMOS_SETTING(bool, cloudRenderRTEnable, true, "rtx.atmosphere.cloudRenderRTEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws the clouds where the camera sees the sky. Off, the directly visible sky is cloudless and the on-screen cloud pass is skipped, unless a debug view needs it. Reflections and indirect light still get their clouds from the reflection dome.")
NUMOS_SETTING(bool, cloudVoxelShadowsEnable, true, "rtx.atmosphere.cloudVoxelShadowsEnable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Casts cloud shadows from the cloud shadow map, on the air and on the ground. Off, there are no cloud shadows on either.")
NUMOS_SETTING(float, cloudShadowMarchStrength, 1.0f, "rtx.atmosphere.cloudShadowMarchStrength", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: cloud shadows come from the clouds themselves. Use rtx.atmosphere.cloudShadowStrength to fade them. Save the config again to stop the notice.")
NUMOS_SETTING(float, cloudShadowFactorStrength, 1.0f, "rtx.atmosphere.cloudShadowFactorStrength", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: cloud shadows come from the clouds themselves. Use rtx.atmosphere.cloudShadowStrength to fade them. Save the config again to stop the notice.")
NUMOS_SETTING(bool, cloudLayer2Enable, false, "rtx.atmosphere.cloudLayer2Enable", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws cloud layer 2: a flat layer such as ground fog, stratus, a high deck or dust. It is lit like the main deck, works at any height (including on the ground with the camera inside it) and is cut off by scene geometry. It shades and is shaded by the main deck when one lies between the other and the sun. Shadows on the ground and in the air come only from the main deck.")
NUMOS_SETTING(float, cloudLayer2Altitude, 5.5f, "rtx.atmosphere.cloudLayer2Altitude", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.cloudLayer2BaseHeightMeters. An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, cloudLayer2Thickness, 2.0f, "rtx.atmosphere.cloudLayer2Thickness", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.cloudLayer2DepthMeters. An existing value is migrated automatically. Save the config again to stop the notice.")
NUMOS_SETTING(float, cloudLayer2TypeMean, 0.6f, "rtx.atmosphere.cloudLayer2TypeMean", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How layer 2's density changes with height, from 0 to 1. 0 is stratus or fog, densest at the base and thinning toward a ragged top. 1 is cumulus-like, thin at the base and full by mid-height, with billowy rather than wispy detail.")
NUMOS_SETTING(float, cloudLayer2CoverageMean, 0.85f, "rtx.atmosphere.cloudLayer2CoverageMean", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The share of the area layer 2 covers, from 0 to 1. The layer is a sheet broken into patches a few kilometres across. Near 1 they merge into a closed sheet with a few holes. Independent of the main deck's coverage.")
NUMOS_SETTING(float, cloudLayer2TypeSpread, 1.0f, "rtx.atmosphere.cloudLayer2TypeSpread", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "How much layer 2's cloud type varies across the sky, from 0 to 1. Independent of the main deck.")
NUMOS_SETTING(float, cloudLayer2NoiseSeed, 1000.0f, "rtx.atmosphere.cloudLayer2NoiseSeed", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The seed for layer 2's patch layout. Different seeds give unrelated layouts.")
NUMOS_SETTING(float, cloudLayer2DensityScale, 0.65f, "rtx.atmosphere.cloudLayer2DensityScale", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Layer 2's density as a multiple of Cloud Density. At the default cloud density, 1 gives about 200 m of visibility inside a fog bank and 0.25 about 800 m.")
NUMOS_SETTING(Vector3, cloudLayer2Albedo, Vector3(1.0f, 1.0f, 1.0f), "rtx.atmosphere.cloudLayer2Albedo", 0.0f, 1.0f, 0,
  "The share of light layer 2's particles scatter rather than absorb, per colour channel. 1, 1, 1 is water droplets, which absorb nothing. Lower values absorb, and because light scatters many times the colour shifts far more than the value suggests. Dust is about 0.95, 0.9, 0.8. Smoke and soot are lower.")
NUMOS_SETTING(bool, cloudLayer2FollowCamera, false, "rtx.atmosphere.cloudLayer2FollowCamera", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Keeps layer 2's base at the camera's height plus Layer 2 Camera Offset instead of at Layer 2 Base Height, for a dust or fog layer the player always stands in, whatever the terrain height. Only the height follows the camera.")
NUMOS_SETTING(float, cloudLayer2CameraOffsetMeters, -30.0f, "rtx.atmosphere.cloudLayer2CameraOffsetMeters", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Layer 2's base relative to the camera while Layer 2 Follows Camera is on, in metres. Negative values put the base below the eye: -30 with a depth of 60 puts the camera in the middle of the layer.")
NUMOS_SETTING(uint32_t, cloudLayer2StepFloor, 8, "rtx.atmosphere.cloudLayer2StepFloor", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: layer 2 uses the main deck's step settings. Save the config again to stop the notice.")
NUMOS_SETTING(uint32_t, cloudLayer2StepMax, 32, "rtx.atmosphere.cloudLayer2StepMax", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated and ignored: layer 2 uses the main deck's step limit (rtx.atmosphere.cloudViewSamplesMax). Save the config again to stop the notice.")
NUMOS_SETTING(Vector3, cloudLayer2Color, Vector3(0.89f, 0.92f, 1.0f), "rtx.atmosphere.cloudLayer2Color", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: replaced by rtx.atmosphere.cloudLayer2Albedo. Layer 2 uses the main deck's Cloud Color with its own albedo. An existing value is migrated. Save the config again to stop the notice.")

NUMOS_MOON_SETTING(bool, enabled, false, "enabled", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "Draws this extra moon, placed by its elevation and rotation, for a game that drives its own moon. Turn rtx.atmosphere.realMoonEnabled off when it replaces the real moon. Its phase, light and earthshine follow from its position and the sun's, like the real moon's.")
NUMOS_MOON_SETTING(float, angularDiameter, 0.518f, "angularDiameter", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The moon's size in the sky, in degrees. 0.518 is the size of Earth's Moon. Its total light stays the same at any size, so a bigger disc has a dimmer surface.")
NUMOS_MOON_SETTING(float, angularRadius, 0.518f, "angularRadius", NUMOS_NO_MIN, NUMOS_NO_MAX, NUMOS_SETTING_NO_SAVE,
  "Deprecated: renamed angularDiameter, which takes the same value. An existing value is migrated. Save the config again to stop the notice.")
NUMOS_MOON_SETTING(Vector3, color, Vector3(1.0f, 1.0f, 1.0f), "color", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The moon's surface brightness and colour relative to Earth's Moon. 1, 1, 1 is lunar. It scales the moon's light and its disc alike.")
NUMOS_MOON_SETTING(float, elevation, 45.0f, "elevation", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The moon's height above the horizon, in degrees. A game integration can set it every frame, which overrides the saved value.")
NUMOS_MOON_SETTING(float, rotation, 90.0f, "rotation", NUMOS_NO_MIN, NUMOS_NO_MAX, 0,
  "The moon's compass direction (azimuth), in degrees. A game integration can set it every frame, which overrides the saved value.")
NUMOS_MOON_SETTING(float, phase, -1.0f, "phase", -1.0f, 1.0f, 0,
  "The moon's phase as a point in its cycle: 0 new, 0.25 first quarter, 0.5 full and 0.75 last quarter. A negative value takes the phase from the moon's position relative to the sun, like the real moon. Set it for a game whose moon follows its own phase calendar. Its light, earthshine and lit side follow this phase. A game integration can set it every frame.")

#undef NUMOS_SETTING
#undef NUMOS_MOON_SETTING