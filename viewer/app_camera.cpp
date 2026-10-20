// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "app.h"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>

namespace numos::viewer {

namespace {

// Cursor position at the previous frame of mouse look.
double s_lastX = 0.0, s_lastY = 0.0;
bool   s_haveLast = false;

} // namespace

Vector3 FreeCamera::forward() const {
  const float yaw = yawDeg * kDegreesToRadians;
  const float pitch = pitchDeg * kDegreesToRadians;
  return Vector3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
}

Vector3 FreeCamera::right() const {
  // lookTo's x axis: cross(up, forward) in the left-handed view.
  const float yaw = yawDeg * kDegreesToRadians;
  return Vector3(std::cos(yaw), 0.0f, -std::sin(yaw));
}

void setMouseLook(FreeCamera& cam, GLFWwindow* window, bool on, bool toggled) {
  if (cam.mouseLook == on) {
    cam.lookToggled = on && (cam.lookToggled || toggled);
    return;
  }
  cam.mouseLook = on;
  cam.lookToggled = on && toggled;
  glfwSetInputMode(window, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  if (glfwRawMouseMotionSupported()) {
    glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, on ? GLFW_TRUE : GLFW_FALSE);
  }
  s_haveLast = false;
}

bool updateCamera(FreeCamera& cam, GLFWwindow* window, float dt, const CameraInput& input) {
  bool moved = false;

  // Right mouse button: look while held, unless the cursor is over the UI when it is pressed.
  const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
  if (rmb && !cam.mouseLook && input.mouseFree) {
    setMouseLook(cam, window, true, false);
  } else if (!rmb && cam.mouseLook && !cam.lookToggled) {
    setMouseLook(cam, window, false, false);
  }

  if (cam.mouseLook) {
    double x, y;
    glfwGetCursorPos(window, &x, &y);
    if (s_haveLast) {
      const float dx = float(x - s_lastX), dy = float(y - s_lastY);
      if (dx != 0.0f || dy != 0.0f) {
        cam.yawDeg = std::fmod(cam.yawDeg + dx * cam.lookDegPerPixel + 360.0f, 360.0f);
        cam.pitchDeg = std::clamp(cam.pitchDeg - dy * cam.lookDegPerPixel, -89.0f, 89.0f);
        moved = true;
      }
    }
    s_lastX = x;
    s_lastY = y;
    s_haveLast = true;
  }

  // Mouse wheel: speed, by a fixed ratio per notch.
  if (input.wheel != 0.0f && (input.mouseFree || cam.mouseLook)) {
    cam.speed = std::clamp(cam.speed * std::pow(1.25f, input.wheel), 0.1f, 100000.0f);
  }

  if (!input.keyboardFree && !cam.mouseLook) {
    return moved;
  }
  auto down = [&](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
  Vector3 move;
  const Vector3 f = cam.forward(), r = cam.right();
  if (down(GLFW_KEY_W)) move += f;
  if (down(GLFW_KEY_S)) move -= f;
  if (down(GLFW_KEY_D)) move += r;
  if (down(GLFW_KEY_A)) move -= r;
  if (down(GLFW_KEY_E)) move += Vector3(0.0f, 1.0f, 0.0f);
  if (down(GLFW_KEY_Q)) move -= Vector3(0.0f, 1.0f, 0.0f);
  const float len = length(move);
  if (len > 0.0f) {
    float speed = cam.speed;
    if (down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT)) speed *= 10.0f;
    if (down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL)) speed *= 0.1f;
    cam.position += move * (speed * dt / len);
    moved = true;
  }
  return moved;
}

} // namespace numos::viewer
