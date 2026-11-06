// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#ifndef NUMOS_CLOUD_LENS_H
#define NUMOS_CLOUD_LENS_H

#include "numos/util/shader_types.h"

NUMOS_SHARED_BEGIN

// Magnifier lenses the screen clouds are seen through (FrameInputs::lenses, docs/DESIGN.md "Lenses").
// Slot i of the previous half describes the same lens as slot i of the current half.
#define CLOUD_MAX_LENSES 4u

// One lens. Positions and lengths are in the host's world units, render space.
struct CloudLens {
  vec3  eyepieceCenter;
  float eyepieceRadius;   // a rectangle's half height

  vec3  axis;             // the optical axis, pointing out of the objective
  float magnification;

  vec3  right;            // a rectangular eyepiece's half-width direction, perpendicular to the axis
  float apertureAspect;   // a rectangle's half width over its half height; 0 for a circular eyepiece

  vec3  up;               // a rectangular eyepiece's half-height direction, perpendicular to the axis
  float reserved0;

  vec3  objectiveCenter;  // where the magnified rays start
  float reserved1;
};

// Constant block of the cloud screen pass (slot 30 of cloud_render.comp.slang).
struct CloudLensArgs {
  uint lensMask;          // the active slots of this frame
  uint previousLensMask;  // the active slots of the previous frame
  uint historyMask;       // the current slots whose previous slot describes the same lens, with history
  uint reserved;

  // [0, CLOUD_MAX_LENSES): this frame; [CLOUD_MAX_LENSES, 2 * CLOUD_MAX_LENSES): the previous frame.
  CloudLens lenses[2 * CLOUD_MAX_LENSES];
};

NUMOS_SHARED_END

#endif
