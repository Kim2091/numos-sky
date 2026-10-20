// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Shared C++/Slang layout for the NUMOS viewer's own passes.
#pragma once

#include "numos/util/shader_types.h"

NUMOS_SHARED_BEGIN

struct ViewerArgs {
  // Ground: a flat plane at groundHeight (world units) along the up axis, albedo groundAlbedo.
  vec3  groundAlbedo;
  float groundHeight;
  vec3  sunRadiance;        // FrameOutputs::lights[kSunLight].radiance
  float missLinearViewZ;    // depth written where the ray misses the ground
  vec3  sunDirection;       // propagation direction (towards the ground)
  uint  zUp;
  float exposure;           // output = tonemap(radiance * exposure)
  uint  frameIdx;
  uint  outputWidth;
  uint  outputHeight;
  uint  accumFrames;        // >0: running average over this many frames (1 = restart); 0 = off
  uint  secondarySky;       // 1: draw the sky through the secondary-ray path (the cloud dome LUT)
  uint  sunCloudShadowed;   // FrameOutputs::lights[kSunLight].cloudShadowed
  uint  moonCloudShadowed;  // ... of the moon light below
  vec3  moonRadiance;       // the brightest active moon light, FrameOutputs::lights[kFirstMoonLight + i]
  uint  skyAmbientCheck;   // --sky-ambient-check: 0 off; 1-3 write a diagnostic (viewer_composite)
  vec3  moonDirection;      // its propagation direction
  float pad2;
  vec3  flashPosition;      // FrameOutputs::lights[kLightningLight], a sphere light (render space)
  float flashRadius;
  vec3  flashRadiance;      // 0 while there is no flash
  float pad3;
};

NUMOS_SHARED_END

#define VIEWER_DEPTH_ARGS          0
#define VIEWER_DEPTH_CAMERA        1
#define VIEWER_DEPTH_OUTPUT        2

// Composite: slots 0-9 are the NUMOS host-API globals (numos_host.slangh), then the rest.
#define VIEWER_TRANSMITTANCE_LUT          0
#define VIEWER_MULTISCATTERING_LUT        1
#define VIEWER_SKY_VIEW_LUT               2
#define VIEWER_SKY_VIEW_SAMPLER           3
#define VIEWER_CLOUD_SKY_TRANSMITTANCE    4
#define VIEWER_CLOUD_SHADOW_MAP           5
#define VIEWER_CLOUD_NOISE_SAMPLER        7
#define VIEWER_CLOUD_SECONDARY_LUT        8
#define VIEWER_BLUE_NOISE                 9
#define VIEWER_ARGS                       10
#define VIEWER_CAMERA                     11
#define VIEWER_ATMOSPHERE_ARGS            12
#define VIEWER_AP_NEAR_SCATTER            13
#define VIEWER_AP_FAR_SCATTER             14
#define VIEWER_AP_SAMPLER                 15
#define VIEWER_CLOUD_RENDER               16
#define VIEWER_CLOUD_DEPTH                17
#define VIEWER_LINEAR_VIEW_Z              18
#define VIEWER_OUTPUT                     19
#define VIEWER_ACCUM                      20
// The night sky's host globals (numos_host.slangh).
#define VIEWER_NIGHT_SKY_STAR_CELLS       21
#define VIEWER_NIGHT_SKY_STARS            22
#define VIEWER_NIGHT_SKY_MILKY_WAY        23
#define VIEWER_NIGHT_SKY_STARLIGHT_LOW    24
#define VIEWER_NIGHT_SKY_MOON             25
#define VIEWER_AP_FAR_TRANSMITTANCE       26
#define VIEWER_AP_FROXEL_AUX              27
#define VIEWER_AP_LIGHTS                  28
#define VIEWER_AP_LIGHT_CLUSTERS          29
