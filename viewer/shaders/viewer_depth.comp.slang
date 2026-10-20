// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// NUMOS viewer: linear view Z of a flat ground plane (the "scene" the cloud pass clamps against).
#include "viewer_common.h"
#include "numos/util/camera.slangh"

layout(binding = VIEWER_DEPTH_ARGS)
ConstantBuffer<ViewerArgs> viewer;

layout(binding = VIEWER_DEPTH_CAMERA)
ConstantBuffer<Camera> camera;

layout(binding = VIEWER_DEPTH_OUTPUT, r32f)
RWTexture2D<float> linearViewZ;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
  if (pixel.x >= viewer.outputWidth || pixel.y >= viewer.outputHeight)
  {
    return;
  }

  const vec2 uv = cameraPixelCoordinateToScreenUV(camera, ivec2(pixel));
  const CameraDirections ray = cameraScreenUVToDirection(camera, uv, false);
  const vec3 origin = cameraGetWorldPosition(camera);
  const float upOrigin = viewer.zUp != 0 ? origin.z : origin.y;
  const float upDir = viewer.zUp != 0 ? ray.worldDirection.z : ray.worldDirection.y;

  float depth = viewer.missLinearViewZ;
  if (upDir < -1e-6f)
  {
    const float t = (viewer.groundHeight - upOrigin) / upDir;
    if (t > 0.0f)
    {
      // Linear view Z is distance along the camera's forward axis.
      depth = t * abs(ray.viewDirection.z);
    }
  }
  linearViewZ[pixel] = depth;
}
