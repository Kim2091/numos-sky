// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "numos/util/shader_types.h"

NUMOS_SHARED_BEGIN

// Compact, analytic form of a scene light for the froxel grid's local-light sampling. Every emitter
// is reduced to a sphere of matching area (see aerialPerspectiveLightRadiance): a volume point has
// no normal, so the solid angle is all that survives of an area light's shape. Built on the CPU in
// Renderer::Impl::buildAerialPerspectiveLights; shared with C++, 16-byte aligned.
struct AerialPerspectiveLight {
  vec3 position;           // World units
  // Distance past which this light is culled from a cluster. Derived from the light's own power and
  // the cutoff irradiance, so a bright light reaches further than a dim one rather than every light
  // paying for the brightest one's range.
  float influenceRadius;

  vec3 radiance;           // Emitted radiance, already scaled by the light's volumetricRadianceScale
  // Radius of the sphere whose solid angle stands in for this emitter's. Equal to the radius for a
  // sphere light and sqrt(area / pi) for the area types, which matches the subtended solid angle in
  // the far field exactly and saturates correctly in the near field instead of diverging.
  float equivalentRadius;

  vec3 coneAxis;           // Shaping axis, normalized; zero vector when the light is unshaped
  float cosConeAngle;      // Cosine of the cone half-angle; ignored when coneAxis is zero

  float coneSoftness;
  float focusExponent;
  // 1 = fade the in-scatter to zero at influenceRadius with (1 - (d/R)^2)^2
  // (rtx.atmosphere.aerialPerspectiveLocalLightWindow); 0 = the plain inverse-square tail.
  float falloffWindow;
  float padLight1;
};

// Tile size of the light cull grid, in froxels, on both screen axes. Eight is the lighting pass's
// thread group size in X and Y, so a whole group shares one cluster and its list reads are uniform.
#define AERIAL_PERSPECTIVE_LIGHT_TILE_SIZE 8u

// Lights any one cluster may hold. The lighting pass's RIS walks the whole list per froxel, so this
// bounds its cost; the CPU sorts lights by importance, so an overflowing cluster keeps the brightest.
#define AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER 16u

// Stride of one cluster's record in the cull buffer: a count followed by that many light indices.
#define AERIAL_PERSPECTIVE_LIGHT_CLUSTER_STRIDE (AERIAL_PERSPECTIVE_MAX_LIGHTS_PER_CLUSTER + 1u)

NUMOS_SHARED_END
