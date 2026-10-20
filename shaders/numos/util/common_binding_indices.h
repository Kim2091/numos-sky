// Modifications Copyright 2026 Kim2091 (Sparkles), licensed under Apache-2.0.
/*
* Copyright (c) 2021-2024, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

// These are set indices - not bindings
#define BINDING_SET_BINDLESS_RAW_BUFFER          1
#define BINDING_SET_BINDLESS_TEXTURE2D           2
#define BINDING_SET_BINDLESS_SAMPLER             3


#define BINDING_ACCELERATION_STRUCTURE           0
#define BINDING_ACCELERATION_STRUCTURE_PREVIOUS  1
#define BINDING_ACCELERATION_STRUCTURE_UNORDERED 2
#define BINDING_ACCELERATION_STRUCTURE_SSS       3
#define BINDING_SURFACE_DATA_BUFFER              4
#define BINDING_SURFACE_MAPPING_BUFFER           5
#define BINDING_SURFACE_MATERIAL_DATA_BUFFER     6
#define BINDING_SURFACE_MATERIAL_EXT_DATA_BUFFER 7
#define BINDING_VOLUME_MATERIAL_DATA_BUFFER      8
#define BINDING_LIGHT_DATA_BUFFER                9
#define BINDING_PREVIOUS_LIGHT_DATA_BUFFER       10
#define BINDING_LIGHT_MAPPING                    11
#define BINDING_BILLBOARDS_BUFFER                12
#define BINDING_BLUE_NOISE_TEXTURE               13
#define BINDING_BINDLESS_INDICES_BUFFER          14
#define BINDING_CONSTANTS                        15
#define BINDING_DEBUG_VIEW_TEXTURE               16
#define BINDING_GPU_PRINT_BUFFER                 17
#define BINDING_VALUE_NOISE_SAMPLER              18
#define BINDING_SAMPLER_READBACK_BUFFER          19
#define BINDING_LIGHT_IDENTITY_BUFFER            20

// Atmosphere LUTs use high binding slots to avoid conflicts with pass-specific bindings
#define BINDING_ATMOSPHERE_TRANSMITTANCE_LUT     200
#define BINDING_ATMOSPHERE_MULTISCATTERING_LUT   201
#define BINDING_ATMOSPHERE_SKY_VIEW_LUT          202
// 203: unused (kept for layout). Check for collisions before reusing it.
#define BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER   204
// 205, 206/207 and 212/213: unused (kept for layout).
// Cloud sky-light LUT. 2D (azimuth, elevation) RGBA16F texture baked per frame from the camera
// position by cloud_sky_transmittance_lut.comp.slang: rgb = premultiplied cloud radiance, a = cloud
// transmittance along that direction. The sky-ambient integration composites it over the sky-view
// LUT radiance per direction.
#define BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT 208

// Cloud render target (Nubis Cubed, 2023). RGBA16F screen-space target at the cloud render
// resolution: rgb = premultiplied cloud colour, a = cloud transmittance. Written once per frame by
// cloud_render.comp.slang.
#define BINDING_ATMOSPHERE_CLOUD_RENDER_RT 209

// Unused by NUMOS (a cloud optical-depth volume slot kept in the shared binding list).
#define BINDING_ATMOSPHERE_CLOUD_D_SUN 210

// Sky-view LUT sampler: linear, clamped. The shaders wrap the azimuth seam themselves
// (sampleAzimuthWrappedLut). The cloud render pass binds its own pass-local sampler for the same
// texture.
#define BINDING_ATMOSPHERE_SKY_VIEW_SAMPLER 214

// Secondary-ray cloud LUT. 256x256 RGBA16F full-sphere dome keyed by (azimuth, signed elevation
// = l*l*sign(l) * (pi/2)), holding the full cloud march per direction: rgb = premultiplied cloud
// radiance, a = view transmittance (same convention as BINDING_ATMOSPHERE_CLOUD_RENDER_RT). Baked
// once per frame by cloud_secondary_lut.comp.slang and read by the non-primary sky-miss branch of
// evalSkyRadiance (indirect, PSR and reflection rays). Sampled with the sky-view sampler, whose
// U repeat handles the azimuth seam.
#define BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT 215

// 216: unused (kept for layout). The placement map is bound at the NVDF bake passes' own slots.

// Cloud depth companion. RG32F screen-space target at the same extent as
// BINDING_ATMOSPHERE_CLOUD_RENDER_RT (the two are allocated and resized together): r = entry
// distance in km (where the marched span started), g = transmittance-weighted mean cloud depth in
// km. It is 32-bit because the march reaches 50+ km at the horizon, where float16's ~2^-10
// relative precision is tens of metres per step and visibly stair-steps the parallax reprojection.
// Written by cloud_render.comp.slang after the primary path tracing pass, so the march can clamp
// against this frame's view depth.
#define BINDING_ATMOSPHERE_CLOUD_DEPTH_RT 217

// The atmosphere and cloud bindings form a contiguous range above COMMON_MAX_BINDING (which only
// covers the base common bindings). The range is exposed so passes that bind their own resources
// can assert that nothing overlaps.
#define BINDING_ATMOSPHERE_MIN                   BINDING_ATMOSPHERE_TRANSMITTANCE_LUT
#define BINDING_ATMOSPHERE_MAX                   BINDING_ATMOSPHERE_CLOUD_DEPTH_RT

// Light sampling slots of the Remix host (a light BVH buffer and a statistics buffer). NUMOS passes
// do not use them; they are reserved so the ranges match the host, and sit above the atmosphere
// range to stay clear of pass-local slots.
#define BINDING_LIGHT_BVH_BUFFER                 218
#define BINDING_LIGHT_SAMPLING_STATS             219

#define COMMON_MAX_BINDING                       BINDING_LIGHT_IDENTITY_BUFFER
#define COMMON_NUM_BINDINGS                      (COMMON_MAX_BINDING + 1)

// Note: Used to represent a non-existent buffer
#define BINDING_INDEX_INVALID uint16_t(0xFFFF)

// Sentinel for an invalid surface index.  Equals the 21-bit maximum (SURFACE_INDEX_MAX_VALUE
// from instance_definitions.h) so that it fits inside the packed RayInteraction._surfaceAndFlags
// field.  The surfaceMapping buffer returns int32_t(-1) for unmapped surfaces; the 21-bit
// property setter truncates 0xFFFFFFFF to 0x1FFFFF automatically.
// This reserves the highest representable surface index as "invalid", reducing the usable
// range by one (max usable index = SURFACE_INDEX_MAX_VALUE - 1 = 2,097,150).
#define SURFACE_INDEX_INVALID 0x001FFFFFu

#define SAMPLER_FEEDBACK_INVALID           uint16_t(0xFFFF)
#define SAMPLER_FEEDBACK_MAX_TEXTURE_COUNT uint16_t(0xFFFF)

// Note: Light array may only be up to a size of 2^16-1, allowing the last index to be
// used for an invalid index similar to the max binding index for materials.
#define LIGHT_INDEX_INVALID (0xFFFF)

#ifdef __cplusplus

#define COMMON_RAYTRACING_BINDINGS \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE)            \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE_UNORDERED)  \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE_PREVIOUS)   \
  ACCELERATION_STRUCTURE(BINDING_ACCELERATION_STRUCTURE_SSS)        \
  STRUCTURED_BUFFER(BINDING_SURFACE_DATA_BUFFER)                    \
  STRUCTURED_BUFFER(BINDING_SURFACE_MAPPING_BUFFER)                 \
  STRUCTURED_BUFFER(BINDING_SURFACE_MATERIAL_DATA_BUFFER)           \
  STRUCTURED_BUFFER(BINDING_SURFACE_MATERIAL_EXT_DATA_BUFFER)       \
  STRUCTURED_BUFFER(BINDING_VOLUME_MATERIAL_DATA_BUFFER)            \
  STRUCTURED_BUFFER(BINDING_LIGHT_DATA_BUFFER)                      \
  STRUCTURED_BUFFER(BINDING_PREVIOUS_LIGHT_DATA_BUFFER)             \
  STRUCTURED_BUFFER(BINDING_LIGHT_MAPPING)                          \
  STRUCTURED_BUFFER(BINDING_BILLBOARDS_BUFFER)                      \
  TEXTURE2DARRAY(BINDING_BLUE_NOISE_TEXTURE)                        \
  CONSTANT_BUFFER(BINDING_CONSTANTS)                                \
  RW_TEXTURE2D(BINDING_DEBUG_VIEW_TEXTURE)                          \
  RW_STRUCTURED_BUFFER(BINDING_GPU_PRINT_BUFFER)                    \
  SAMPLER3D(BINDING_VALUE_NOISE_SAMPLER)                            \
  RW_STRUCTURED_BUFFER(BINDING_SAMPLER_READBACK_BUFFER)             \
  STRUCTURED_BUFFER(BINDING_LIGHT_IDENTITY_BUFFER)             \
  TEXTURE2D(BINDING_ATMOSPHERE_TRANSMITTANCE_LUT)                   \
  TEXTURE2D(BINDING_ATMOSPHERE_MULTISCATTERING_LUT)                 \
  TEXTURE2D(BINDING_ATMOSPHERE_SKY_VIEW_LUT)                        \
  SAMPLER(BINDING_ATMOSPHERE_CLOUD_NOISE_SAMPLER)                   \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_SKY_TRANSMITTANCE_LUT)          \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_RENDER_RT)                     \
  TEXTURE3D(BINDING_ATMOSPHERE_CLOUD_D_SUN)                         \
  SAMPLER(BINDING_ATMOSPHERE_SKY_VIEW_SAMPLER)                      \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_SECONDARY_LUT)                 \
  TEXTURE2D(BINDING_ATMOSPHERE_CLOUD_DEPTH_RT)                      \
  STRUCTURED_BUFFER(BINDING_LIGHT_BVH_BUFFER)                       \
  RW_STRUCTURED_BUFFER(BINDING_LIGHT_SAMPLING_STATS)

#endif
