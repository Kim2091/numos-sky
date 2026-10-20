// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// NUMOS viewer composite: what a host's lighting + composite would do with NUMOS outputs, reduced to
// a flat ground plane. Exercises the whole host shader API (numos_host.slangh):
//   sky miss       -> evalSkyRadiance (primary, cloudless)
//   ground         -> sun * cloud ground shadow + sky irradiance (evalSkyRadiance over the upper
//                     hemisphere, clouds from the secondary dome LUT)
//   both           -> cloud composite, then aerial perspective, then a simple tonemap into RGBA8.
#include "viewer_common.h"

// ---- NUMOS host-API globals (names fixed by numos_host.slangh) ----
layout(binding = VIEWER_TRANSMITTANCE_LUT)       Texture2D AtmosphereTransmittanceLut;
layout(binding = VIEWER_MULTISCATTERING_LUT)     Texture2D AtmosphereMultiscatteringLut;
layout(binding = VIEWER_SKY_VIEW_LUT)            Texture2D AtmosphereSkyViewLut;
layout(binding = VIEWER_SKY_VIEW_SAMPLER)        SamplerState AtmosphereSkyViewSampler;
layout(binding = VIEWER_CLOUD_SKY_TRANSMITTANCE) Texture2D<float4> AtmosphereCloudSkyTransmittanceLut;
layout(binding = VIEWER_CLOUD_SHADOW_MAP)        Texture3D<float2> AtmosphereCloudShadowMap;
layout(binding = VIEWER_CLOUD_NOISE_SAMPLER)     SamplerState AtmosphereCloudNoise3DSampler;
layout(binding = VIEWER_CLOUD_SECONDARY_LUT)     Texture2D<float4> AtmosphereCloudSecondaryLut;
layout(binding = VIEWER_BLUE_NOISE)              Texture2DArray BlueNoise;
layout(binding = VIEWER_NIGHT_SKY_STAR_CELLS)    StructuredBuffer<uint> NightSkyStarCells;
layout(binding = VIEWER_NIGHT_SKY_STARS)         StructuredBuffer<float4> NightSkyStars;
layout(binding = VIEWER_NIGHT_SKY_MILKY_WAY)     StructuredBuffer<uint> NightSkyMilkyWay;
layout(binding = VIEWER_NIGHT_SKY_STARLIGHT_LOW) Texture2D<float4> NightSkyStarlightLow;
layout(binding = VIEWER_NIGHT_SKY_MOON)          Texture2D<float4> NightSkyMoon;
#define ATMOSPHERE_AVAILABLE
#define ATMOSPHERE_TRANSMITTANCE_LUT_AVAILABLE
#define NIGHT_SKY_AVAILABLE

#include "numos/host/numos_host.slangh"

layout(binding = VIEWER_ARGS)            ConstantBuffer<ViewerArgs> viewer;
layout(binding = VIEWER_CAMERA)          ConstantBuffer<Camera> camera;
layout(binding = VIEWER_ATMOSPHERE_ARGS) ConstantBuffer<AtmosphereArgs> atmosphereArgs;
layout(binding = VIEWER_AP_NEAR_SCATTER)      Texture3D<float4> apNearScatter;
layout(binding = VIEWER_AP_FAR_SCATTER)       Texture3D<float4> apFarScatter;
layout(binding = VIEWER_AP_FAR_TRANSMITTANCE) Texture3D<float4> apFarTransmittance;
layout(binding = VIEWER_AP_FROXEL_AUX)        Texture3D<float4> apFroxelAux;
layout(binding = VIEWER_AP_LIGHTS)            StructuredBuffer<AerialPerspectiveLight> apLights;
layout(binding = VIEWER_AP_LIGHT_CLUSTERS)    StructuredBuffer<uint> apLightClusters;
layout(binding = VIEWER_AP_SAMPLER)           SamplerState apSampler;
layout(binding = VIEWER_CLOUD_RENDER)    Texture2D<float4> cloudRender;
layout(binding = VIEWER_CLOUD_DEPTH)     Texture2D<float4> cloudDepth;
layout(binding = VIEWER_LINEAR_VIEW_Z)   Texture2D<float> linearViewZ;
layout(binding = VIEWER_OUTPUT, rgba8)   RWTexture2D<float4> output;
layout(binding = VIEWER_ACCUM, rgba32f)  RWTexture2D<float4> accum;

vec3 acesFilm(vec3 x)
{
  return saturate((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

// Sky irradiance on the ground plane: the cosine-weighted mean of the sky radiance a diffuse
// gather ray would see (evalSkyRadiance's non-primary path, so clouds come from the secondary dome
// LUT with their own lighting, not just as an occluder). Every ground point sees the same
// camera-centred LUTs, so the integral is one number per frame; each 8x8 group evaluates it with a
// fixed stratified set of kSkyGatherSide^2 directions (kSkyGatherTapsPerThread per thread) and
// reduces in groupshared memory, so all groups agree exactly and there is no noise.
static const uint kSkyGatherThreads = 64u;
static const uint kSkyGatherTapsPerThread = 4u;
static const uint kSkyGatherSide = 16u;  // kSkyGatherSide^2 == threads * taps per thread
groupshared vec3 skyGatherPartial[kSkyGatherThreads];

// Cosine-weighted direction (Malley) for stratum `index` of a kSkyGatherSide^2 grid, about `up`.
vec3 skyGatherDirection(uint index, vec3 up)
{
  const vec2 u = (vec2(float(index % kSkyGatherSide), float(index / kSkyGatherSide)) + 0.5f) / float(kSkyGatherSide);
  const float r = sqrt(u.x);
  const float phi = 2.0f * kPi * u.y;
  const vec3 local = vec3(r * cos(phi), sqrt(max(1.0f - u.x, 0.0f)), r * sin(phi));  // y = cos(theta)
  const vec3 t = abs(up.x) < 0.9f ? normalize(cross(up, vec3(1, 0, 0))) : normalize(cross(up, vec3(0, 1, 0)));
  const vec3 b = cross(up, t);
  return t * local.x + up * local.y + b * local.z;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
  const AtmosphereArgs args = atmosphereArgs;
  const bool isZUp = viewer.zUp != 0;
  const vec3 groundNormal = isZUp ? vec3(0, 0, 1) : vec3(0, 1, 0);

  // Before the bounds test: every thread of the group takes part in the reduction.
  {
    vec3 sum = vec3(0.0f);
    for (uint i = 0u; i < kSkyGatherTapsPerThread; ++i)
    {
      const vec3 dir = skyGatherDirection(groupIndex * kSkyGatherTapsPerThread + i, groundNormal);
      sum += evalSkyRadiance(dir, args, uint2(0, 0), viewer.frameIdx, isZUp, /*isPrimaryRay=*/false,
                             /*applySkyIndirectRadianceScale=*/true,
                             /*skyDisks=*/NUMOS_SKY_DISKS_NONE);  // the ground takes the sun and moon as lights
    }
    skyGatherPartial[groupIndex] = sum;
  }
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = kSkyGatherThreads / 2u; stride > 0u; stride >>= 1u)
  {
    if (groupIndex < stride)
    {
      skyGatherPartial[groupIndex] += skyGatherPartial[groupIndex + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  // E = pi * mean(L) for cosine-weighted directions.
  const vec3 skyIrradiance = skyGatherPartial[0] * (kPi / float(kSkyGatherThreads * kSkyGatherTapsPerThread));

  if (pixel.x >= viewer.outputWidth || pixel.y >= viewer.outputHeight)
  {
    return;
  }

  // Diagnostic (--sky-ambient-check n): every pixel holds one number per channel, no aerial
  // perspective, no cloud composite; read it from --accumulate 1 --hdr-out.
  //   1: the ground's sky irradiance from the gather above (evalSkyRadiance, the dome LUT itself).
  //   2: the same cosine-weighted integral through the host helper sampleSkyAmbientForVolume
  //      (sky-view LUT x cloud sky-light LUT), i.e. what a host's volumes and particles receive.
  //   3: the cloud sky-light LUT's transmittance over the same directions: (cosine-weighted mean,
  //      min, max), unitless (pre-divided by the exposure so the PFM holds the raw value).
  if (viewer.skyAmbientCheck != 0u)
  {
    vec3 value = skyIrradiance;
    if (viewer.skyAmbientCheck >= 2u)
    {
      const uint taps = kSkyGatherSide * kSkyGatherSide;
      vec3 sum = vec3(0.0f);
      float tSum = 0.0f, tMin = 1.0f, tMax = 0.0f;
      uint lutWidth, lutHeight, lutLevels;
      AtmosphereCloudSkyTransmittanceLut.GetDimensions(0u, lutWidth, lutHeight, lutLevels);
      for (uint i = 0u; i < taps; ++i)
      {
        const vec3 dir = skyGatherDirection(i, groundNormal);
        if (viewer.skyAmbientCheck == 2u)
        {
          sum += sampleSkyAmbientForVolume(dir, args, AtmosphereSkyViewLut, AtmosphereCloudSkyTransmittanceLut,
                                           AtmosphereSkyViewSampler);
        }
        else
        {
          // The helper's own lookup, transmittance only.
          float azimuth = atan2(dir.z, dir.x);
          if (azimuth < 0.0f) azimuth += 2.0f * kPi;
          const float elevV = sqrt(asin(saturate(dir.y)) / (kPi * 0.5f)) * 0.5f + 0.5f;
          const vec2 cloudUv = vec2(azimuth / (2.0f * kPi), max(elevV, 0.5f + 0.5f / float(max(lutHeight, 1u))));
          const float t = sampleCloudDomeLut(AtmosphereCloudSkyTransmittanceLut, AtmosphereSkyViewSampler, cloudUv, 0.0f).a;
          tSum += t;
          tMin = min(tMin, t);
          tMax = max(tMax, t);
        }
      }
      value = viewer.skyAmbientCheck == 2u ? sum * (kPi / float(taps))
                                           : vec3(tSum / float(taps), tMin, tMax) / max(viewer.exposure, 1e-30f);
    }
    if (viewer.accumFrames > 0u)
    {
      accum[pixel] = vec4(value, 1.0f);
    }
    output[pixel] = vec4(pow(acesFilm(value * viewer.exposure), 1.0f / 2.2f), 1.0f);
    return;
  }
  const vec2 uv = cameraPixelCoordinateToScreenUV(camera, ivec2(pixel));
  const CameraDirections ray = cameraScreenUVToDirection(camera, uv, false);
  const vec3 origin = cameraGetWorldPosition(camera);
  const float viewZ = linearViewZ[pixel];
  const bool miss = viewZ >= viewer.missLinearViewZ;

  vec3 radiance;
  float viewDistance = 0.0f;
  if (miss)
  {
    radiance = evalSkyRadiance(ray.worldDirection, args, pixel, viewer.frameIdx, isZUp,
                               /*isPrimaryRay=*/viewer.secondarySky == 0u,
                               /*applySkyIndirectRadianceScale=*/false,
                               /*skyDisks=*/NUMOS_SKY_DISKS_ALL);  // the cloud composite below covers them
  }
  else
  {
    viewDistance = viewZ / max(abs(ray.viewDirection.z), 1e-6f);
    const vec3 position = origin + ray.worldDirection * viewDistance;
    const vec3 normal = groundNormal;
    const vec3 toSun = -viewer.sunDirection;

    // Sun and moon, shadowed by the clouds as a host does for lights NUMOS marks cloudShadowed.
    float cloudT = 1.0f;
    if (args.cloudVoxelShadowsEnable != 0u && viewer.sunCloudShadowed != 0u)
    {
      cloudT = sampleCloudGroundShadow_OptionB(position, toSun, args, isZUp);
    }
    const vec3 toMoon = -viewer.moonDirection;
    float moonCloudT = 1.0f;
    if (args.cloudVoxelShadowsEnable != 0u && viewer.moonCloudShadowed != 0u)
    {
      moonCloudT = sampleCloudGroundShadow_OptionB(position, toMoon, args, isZUp);
    }
    // Treat the emitted distant-light value as irradiance at normal incidence (viewer-only shading).
    const vec3 direct = viewer.sunRadiance * kPi * saturate(dot(normal, toSun)) * cloudT
                      + viewer.moonRadiance * kPi * saturate(dot(normal, toMoon)) * moonCloudT;

    // Lambertian: radiance = albedo / pi * (sun irradiance + sky irradiance).
    radiance = viewer.groundAlbedo * (direct + skyIrradiance) / kPi;

    // The lightning flash: NUMOS's sphere light, unshadowed (it is not cloudShadowed). Irradiance
    // pi L r^2 cos / d^2.
    if (any(viewer.flashRadiance > 0.0f))
    {
      const vec3 toFlash = viewer.flashPosition - position;
      const float r2 = viewer.flashRadius * viewer.flashRadius;
      const float d2 = max(dot(toFlash, toFlash), r2);
      const float cosine = saturate(dot(normal, toFlash) * rsqrt(d2));
      radiance += viewer.groundAlbedo * viewer.flashRadiance * (r2 / d2 * cosine);
    }
  }



  vec3 alphaBlend = vec3(0.0f);
  vec3 foreground = vec3(0.0f);
  vec4 cloudOverPixel;
  numosApplyCloudComposite(camera, args, cloudRender, cloudDepth, apFarScatter, apFarTransmittance, apSampler,
                           pixel, miss, viewDistance, 1.0f, vec3(1.0f), radiance, alphaBlend, false, 0.0f,
                           foreground, 0.0f, false, cloudOverPixel);

  // After the clouds: the near field (lamp glow, shadowed air) is in front of them.
  numosApplyAerialPerspective(camera, args, false, apNearScatter, apFarScatter, apFarTransmittance, apFroxelAux,
                              apLights, apLightClusters, apSampler,
                              pixel, miss, viewDistance, vec3(1.0f), vec3(1.0f), cloudOverPixel, radiance);

  // Offline stand-in for a host's temporal denoiser: a still camera's running average in linear light.
  if (viewer.accumFrames > 0u)
  {
    const vec3 history = viewer.accumFrames == 1u ? vec3(0.0f) : accum[pixel].rgb;
    // One non-finite sample would poison the average for good; drop it.
    const bool finite = all(!isnan(radiance) && !isinf(radiance));
    radiance = finite ? lerp(history, radiance, 1.0f / float(viewer.accumFrames)) : history;
    accum[pixel] = vec4(radiance, 1.0f);
  }

  output[pixel] = vec4(pow(acesFilm(radiance * viewer.exposure), 1.0f / 2.2f), 1.0f);
}
