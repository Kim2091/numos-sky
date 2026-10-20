// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Cloud sun-space shadow map bake (layout: lib/cloud_shadow_map.slangh).
//
// One thread per texel; a dispatch covers a chunk of rows of one cascade. The texel's sun ray is
// marched from where it leaves the cloud shell toward the sun to where it leaves on the far side,
// integrating the same body field the view march's light taps read, and the running optical depth
// is written at each of the kCloudShadowMapSlices evenly spaced depths. Steps are sized to the
// cascade's texels, grow with distance from the camera (only the soft structure matters far out),
// and skip empty air using the distance field.

#include "numos/util/shader_types.h"
#include "numos/lib/atmosphere_args.h"
#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/cloud_nubis3_common.slangh"
#include "numos/lib/cloud_shadow_map.slangh"

layout(binding = 0)
ConstantBuffer<AtmosphereArgs> atmosphereArgs;

layout(binding = 1, rg16f)
RWTexture3D<float2> cloudShadowMapOutput;

layout(binding = 2)
SamplerState cloudSampler;

layout(binding = 3)
Texture3D<float> cloudNvdfSdf;

layout(binding = 4, r16f)
RWTexture3D<float> cloudHorizonMapOutput;

// Beer form of the sun map (one layer per slot): x = front depth fraction (first body density along
// the ray from where it leaves the shell toward the sun), y = back depth fraction (last), z / w =
// the ray's whole body / soft-body optical depth.
layout(binding = 5, rgba16f)
RWTexture3D<float4> cloudBeerMapOutput;

void writeSlice(bool horizon, uint3 texel, vec2 opticalDepth) {
  if (horizon) {
    cloudHorizonMapOutput[texel] = opticalDepth.x;
  } else {
    cloudShadowMapOutput[texel] = opticalDepth;
  }
}

// Width of the soft body's ramp outside the surface (cloudSoftBodyDensity is nonzero up to here).
static const float kSoftOutsideKm = 0.25f;

struct BodySample {
  float hard;     // body density the light marches use
  float soft;     // the widened body the soft sun octaves see
  float emptyKm;  // distance along any direction guaranteed free of both
};

BodySample sampleBody(vec3 worldPosKm, AtmosphereArgs args, vec3 planetCenterWorldKm) {
  const float hf = computeCloudHeightFractionC(worldPosKm, planetCenterWorldKm, args.planetRadius,
                                               args.cloudAltitude, args.cloudThickness);
  const vec3 perturbed = worldPosKm - vec3(args.cloudWindOffset.x, 0.0f, args.cloudWindOffset.y);
  const float kHalfTexelV = 0.5f / float(CLOUD_NVDF_SIZE_Y);
  const float v = clamp(hf, kHalfTexelV, 1.0f - kHalfTexelV);
  const float offsetKm = (args.cloudCoverageMean - args.nvdfNominalCoverage) * args.nvdfCoverageOffsetKm;
  float sdfKm;
  float boundMinKm;
  float boundaryKm = 100.0f;
  if (args.cloudHexTilingEnable > 0.5f) {
    // The hex union of the three shrunk tiles, as cloudBodySdfKm and the full density. The
    // empty-space bound takes the three tiles unshrunk (the shrink varies with position, so the
    // union is not a distance) and the distance to the triangle's edge, where the tiles change.
    const CloudHexTile hex = computeCloudHexTile(perturbed.xz, args);
    const float s1 = cloudNvdfSdf.SampleLevel(cloudSampler, vec3(hex.uv1.x, v, hex.uv1.y), 0).r;
    const float s2 = cloudNvdfSdf.SampleLevel(cloudSampler, vec3(hex.uv2.x, v, hex.uv2.y), 0).r;
    const float s3 = cloudNvdfSdf.SampleLevel(cloudSampler, vec3(hex.uv3.x, v, hex.uv3.y), 0).r;
    sdfKm = min(s1 + cloudUnionShrinkKm(hex.w1, args),
                min(s2 + cloudUnionShrinkKm(hex.w2, args), s3 + cloudUnionShrinkKm(hex.w3, args)));
    boundMinKm = min(s1, min(s2, s3));
    boundaryKm = hex.boundaryKm;
  } else {
    const vec2 tileUV = frac(perturbed.xz / args.cloudNoiseTileKm);
    sdfKm = cloudNvdfSdf.SampleLevel(cloudSampler, vec3(tileUV.x, v, tileUV.y), 0).r;
    boundMinKm = sdfKm;
  }
  sdfKm -= offsetKm;
  // Outside the shell (a horizon ray passing under the base) there is no cloud.
  const float altitudeKm = length(worldPosKm - planetCenterWorldKm) - args.planetRadius - args.cloudAltitude;
  const float inShell = altitudeKm >= 0.0f && altitudeKm <= args.cloudThickness ? 1.0f : 0.0f;
  BodySample s;
  s.hard = inShell * saturate(-sdfKm / max(args.nvdfProfileDepthKm, 1e-3f)) * cloudCumulusHeightDensity(hf, args.cloudThickness);
  s.soft = inShell * saturate(0.5f - (sdfKm + 0.5f * args.nvdfProfileDepthKm) / 0.5f) * cloudCumulusHeightDensity(hf, args.cloudThickness);
  s.emptyKm = min(boundMinKm - offsetKm - kSoftOutsideKm, boundaryKm);
  return s;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  AtmosphereArgs args = atmosphereArgs;
  // One cascade of one map per dispatch, a chunk of rows at a time, into the target slot.
  const uint map = args.cloudShadowBakeTarget & 1u;
  const bool horizon = map == 1u;
  const uint k = (args.cloudShadowBakeTarget >> 4u) & 3u;
  const uint slot = (args.cloudShadowBakeTarget >> 8u) & 7u;
  const uint2 texel = uint2(threadId.x, threadId.y + args.cloudShadowBakeRow0);
  if (any(texel >= uint2(kCloudShadowMapSize))) {
    return;
  }
  const uint3 dispatchId = uint3(texel, 0u);
  const uint layer0 = slot * kCloudShadowMapSlices;

  const CloudShadowBasis basis = cloudShadowBasis(cloudShadowCascadeDir(args, map, k));
  const vec2 d = (vec2(texel) + 0.5f) / float(kCloudShadowMapSize) * 2.0f - 1.0f;
  const float halfU = args.cloudShadowHalfU[map][k];
  const float u = args.cloudShadowCenterU[map][k] + d.x * halfU;
  const float halfV = args.cloudShadowHalfV[map][k];
  const float v = args.cloudShadowCenterV[map][k] + d.y * halfV;

  // The texel's ray in the world frame of this bake (cloud frame + this frame's wind offset), moved
  // along S to its point nearest the camera so the shell intersections stay well conditioned.
  const vec3 windKm = vec3(args.cloudWindOffset.x, 0.0f, args.cloudWindOffset.y);
  const vec3 linePoint = basis.U * u + basis.V * v + windKm;
  const vec3 camera = args.cameraWorldPosYUpKm;
  const vec3 Q = linePoint + basis.S * dot(camera - linePoint, basis.S);
  const vec3 planetCenter = getCloudPlanetCenterWorldKm(args);
  const vec3 rel = Q - planetCenter;
  const float rho = length(rel);
  const float mu = dot(basis.S, rel / rho);
  const float baseRadius = args.planetRadius + args.cloudAltitude;
  const float topRadius = baseRadius + args.cloudThickness;

  // A sun ray leaves the shell toward the sun through the top, and on the far side through the base,
  // or through the top again if it passes over the base sphere. A horizon ray takes the whole chord
  // of the top sphere: near the camera it dips under the base and comes back up into the deck on
  // both sides, and points on either side must read the same ray (cloudHorizonDepthFraction).
  float top0, top1, base0, base1;
  float lengthKm = 0.0f;
  float tStart = 0.0f;
  const bool hasBase = cloudShadowSphereRoots(rho, mu, baseRadius, base0, base1);
  if (cloudShadowSphereRoots(rho, mu, topRadius, top0, top1)) {
    tStart = top1;
    const float tEnd = (hasBase && !horizon) ? base1 : top0;
    lengthKm = tStart - tEnd;
  }
  if (!(lengthKm > 1e-4f)) {
    for (uint j = 0u; j < kCloudShadowMapSlices; ++j) {
      writeSlice(horizon, uint3(dispatchId.xy, layer0 + j), vec2(0.0f));
    }
    if (!horizon) {
      cloudBeerMapOutput[uint3(dispatchId.xy, slot)] = vec4(1.0f, 1.0f, 0.0f, 0.0f);
    }
    return;
  }
  float frontKm = -1.0f;
  float backKm = 0.0f;

  const float sliceKm = lengthKm / float(kCloudShadowMapSlices - 1u);
  const float texelKm = 2.0f * max(halfU, halfV) / float(kCloudShadowMapSize);
  // The horizon map only serves the soft horizon light past the live first 1.5 km of its streams.
  const float baseStepKm = horizon ? clamp(texelKm, 0.15f, 0.5f) : clamp(0.5f * texelKm, 0.04f, 0.25f);
  const float windowKm = halfU;
  // Only the stretch a lookup can reach is integrated: points in this window read at most
  // kReachKm along their ray (the light marches' kCloudSunFarKm), and t = 0 is the ray's point
  // nearest the camera. Outside it the ray counts as empty.
  const float kReachKm = 150.0f;
  const float tReach = 1.2f * windowKm + kReachKm;

  writeSlice(horizon, uint3(dispatchId.xy, layer0), vec2(0.0f));
  vec2 opticalDepth = vec2(0.0f);
  float t = tStart;
  uint j = 1u;
  for (uint iter = 0u; iter < 4096u && j < kCloudShadowMapSlices; ++iter) {
    const float tBoundary = tStart - sliceKm * float(j);
    const vec3 p = Q + basis.S * t;
    const float distanceKm = length((p - camera).xz);
    if (t > tReach || t < -tReach) {
      // Out of reach: jump to the reachable stretch, or through the remaining slices.
      t = t > tReach ? max(tReach, tBoundary) : tBoundary;
      for (;;) {
        const float tb = tStart - sliceKm * float(j);
        if (t > tb + 1e-5f || j >= kCloudShadowMapSlices) {
          break;
        }
        writeSlice(horizon, uint3(dispatchId.xy, layer0 + j), opticalDepth);
        ++j;
      }
      continue;
    }
    // Steps grow with distance from the camera: far out only the soft, large-scale structure counts.
    const float stepKm = min(min(baseStepKm * max(1.0f, 2.0f * distanceKm / windowKm), 2.0f), t - tBoundary);
    const vec3 mid = p - basis.S * (0.5f * stepKm);
    const BodySample body = sampleBody(mid, args, planetCenter);
    opticalDepth += vec2(body.hard, body.soft) * stepKm;
    if (body.hard > 0.0f) {
      if (frontKm < 0.0f) {
        frontKm = tStart - t;
      }
      backKm = tStart - t + stepKm;
    }
    t -= stepKm;
    // Empty air past the step (the bound holds around the midpoint), and the stretch under the base.
    float advanceKm = max(0.95f * body.emptyKm - 0.5f * stepKm, 0.0f);
    if (horizon && hasBase && t < base1 && t > base0) {
      advanceKm = max(advanceKm, t - base0);
    }
    for (;;) {
      const float tb = tStart - sliceKm * float(j);
      if (t <= tb + 1e-5f) {
        writeSlice(horizon, uint3(dispatchId.xy, layer0 + j), opticalDepth);
        ++j;
        if (j >= kCloudShadowMapSlices) {
          break;
        }
        continue;
      }
      if (advanceKm <= 0.0f) {
        break;
      }
      const float move = min(advanceKm, t - tb);
      t -= move;
      advanceKm -= move;
    }
  }
  // Iteration cap: hold the last value through the remaining slices.
  for (; j < kCloudShadowMapSlices; ++j) {
    writeSlice(horizon, uint3(dispatchId.xy, layer0 + j), opticalDepth);
  }
  if (!horizon) {
    cloudBeerMapOutput[uint3(dispatchId.xy, slot)] = frontKm < 0.0f
      ? vec4(1.0f, 1.0f, 0.0f, 0.0f)
      : vec4(frontKm / lengthKm, min(backKm / lengthKm, 1.0f), opticalDepth);
  }
}
