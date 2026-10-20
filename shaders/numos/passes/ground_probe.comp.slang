// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Ground probes for the automatic ground datum (docs/DESIGN.md "Ground level"; lib/ground_probe.h).
//
// One thread per probe. Probe n of the session takes point n of the R2 sequence, spread over a disc
// around the camera with a density falling as 1 / r (radius linear in the sequence), so the ground
// near the camera is measured densely and the rest sparsely. Its ray starts startAboveWorld above
// the camera and runs straight down for lengthWorld against the opaque scene. It records the first
// hit, whatever it is: roofs and canopies are rejected later by the CPU's low per-cell statistic.
// A miss (nothing loaded there, or the start point is inside a mountain) writes w = 0.
//
// Dispatched only with a host TLAS (FrameInputs::opaqueTlas).

#include "numos/lib/atmosphere_common.slangh"
#include "numos/lib/ground_probe.h"
#include "numos/util/instance_definitions.h"   // OBJECT_MASK_OPAQUE

layout(binding = GROUND_PROBE_BINDING_ACCELERATION_STRUCTURE)
RaytracingAccelerationStructure sceneTlas;

// xyz the hit (render space), w 1 for a hit and 0 for a miss.
layout(binding = GROUND_PROBE_BINDING_HITS)
RWStructuredBuffer<float4> probeHits;

layout(push_constant)
ConstantBuffer<GroundProbeArgs> probeArgs;

[shader("compute")]
[numthreads(GROUND_PROBE_GROUP_SIZE, 1, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
  const uint i = threadId.x;
  if (i >= probeArgs.probeCount) {
    return;
  }
  // R2 (Roberts 2018) in 0.32 fixed point, so the sequence index can grow without losing precision.
  const uint n = probeArgs.sequenceBase + i;
  const vec2 u = vec2(float((n * 3242174889u) >> 8u), float((n * 2447445413u) >> 8u)) * (1.0f / 16777216.0f);
  const float r = probeArgs.radiusWorld * u.x;
  const float phi = 2.0f * kPi * u.y;
  const bool isZUp = probeArgs.isZUp != 0u;
  const bool flip = probeArgs.flipUpAxis != 0u;
  const vec3 origin = probeArgs.originWorld
                    + atmosphereYUpToWorld(vec3(r * cos(phi), probeArgs.startAboveWorld, r * sin(phi)), isZUp, flip);
  const vec3 down = atmosphereYUpToWorld(vec3(0.0f, -1.0f, 0.0f), isZUp, flip);

  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = down;
  ray.TMin = 0.0f;
  ray.TMax = probeArgs.lengthWorld;
  static const uint kRayFlags = RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
  RayQuery<kRayFlags> query;
  query.TraceRayInline(sceneTlas, kRayFlags, OBJECT_MASK_OPAQUE, ray);
  query.Proceed();
  const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  probeHits[i] = hit ? float4(origin + down * query.CommittedRayT(), 1.0f) : float4(origin, 0.0f);
}
