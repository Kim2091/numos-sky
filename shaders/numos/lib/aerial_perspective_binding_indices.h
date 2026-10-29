// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Binding slots of the aerial perspective passes (docs/aerial-perspective-v2.md).

// aerial_perspective_far: the camera-centred far-field LUT.
#define AP_FAR_ATMOSPHERE_ARGS           0
#define AP_FAR_TRANSMITTANCE_INPUT       1
#define AP_FAR_MULTISCATTERING_INPUT     2
#define AP_FAR_LUT_SAMPLER               3
#define AP_FAR_CLOUD_SHADOW_MAP          4
#define AP_FAR_CLOUD_SAMPLER             5
#define AP_FAR_SCATTER_OUTPUT            6
#define AP_FAR_TRANSMITTANCE_OUTPUT      7

// aerial_perspective_light_cull: local lights binned into froxel clusters.
#define AP_CULL_ATMOSPHERE_ARGS          0
#define AP_CULL_CAMERA                   1
#define AP_CULL_LIGHTS                   2
#define AP_CULL_CLUSTERS                 3

// aerial_perspective_froxel_lighting(_traced): per-froxel visibility and local lights, temporal.
#define AP_LIGHTING_ATMOSPHERE_ARGS      0
#define AP_LIGHTING_CAMERA               1
#define AP_LIGHTING_ACCELERATION_STRUCTURE 2
#define AP_LIGHTING_LIGHTS               3
#define AP_LIGHTING_CLUSTERS             4
#define AP_LIGHTING_HISTORY              5
#define AP_LIGHTING_HISTORY_AUX          6
#define AP_LIGHTING_SAMPLER              7
#define AP_LIGHTING_OUTPUT               8
#define AP_LIGHTING_OUTPUT_AUX           9
#define AP_LIGHTING_INJECTION            10
#define AP_LIGHTING_LINEAR_VIEW_Z        11
#define AP_LIGHTING_SURFACE_ARGS         12

// aerial_perspective_froxel_integrate: front-to-back integration of every froxel column.
#define AP_INTEGRATE_ATMOSPHERE_ARGS     0
#define AP_INTEGRATE_CAMERA              1
#define AP_INTEGRATE_TRANSMITTANCE_INPUT 2
#define AP_INTEGRATE_MULTISCATTERING_INPUT 3
#define AP_INTEGRATE_LUT_SAMPLER         4
#define AP_INTEGRATE_LIGHTING            5
#define AP_INTEGRATE_LIGHTING_AUX        6
#define AP_INTEGRATE_CLOUD_SHADOW_MAP    7
#define AP_INTEGRATE_CLOUD_SAMPLER       8
#define AP_INTEGRATE_OUTPUT              9
#define AP_INTEGRATE_LINEAR_VIEW_Z       10
#define AP_INTEGRATE_SURFACE_ARGS        11
#define AP_INTEGRATE_FAR_USAGE           12
