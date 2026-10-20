// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#ifndef NUMOS_GROUND_PROBE_H
#define NUMOS_GROUND_PROBE_H

#include "numos/util/shader_types.h"

NUMOS_SHARED_BEGIN

// Automatic ground datum (docs/DESIGN.md "Ground level"). The ground probe pass casts a handful of
// rays straight down against the host's TLAS from points scattered around the camera and writes
// where they hit; the CPU reads them back a few frames later (IGpu::readBuffer, never blocking) and
// folds them into a world-anchored, heavily low-passed ground-height field (src/ground_field.h).

#define GROUND_PROBE_BINDING_ACCELERATION_STRUCTURE 0
#define GROUND_PROBE_BINDING_HITS                   1

// Probes per dispatch (one dispatch per frame), and the pass's group size.
#define GROUND_PROBE_COUNT      64
#define GROUND_PROBE_GROUP_SIZE 64

// Push constants. All lengths in the host's world units, positions in render space.
struct GroundProbeArgs {
  vec3  originWorld;      // the rendered camera
  uint  probeCount;
  float radiusWorld;      // probes are spread over a disc of this radius around the camera ...
  float startAboveWorld;  // ... and start this far above it, along the up axis
  float lengthWorld;      // ray length, straight down
  uint  sequenceBase;     // index of this dispatch's first point in the R2 sequence
  uint  isZUp;            // the engine's up axis is Z
  uint  flipUpAxis;       // the engine's up axis points along -Y (-Z)
  float pad0;
  float pad1;
};

NUMOS_SHARED_END

#endif
