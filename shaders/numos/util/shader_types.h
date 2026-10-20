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

#ifdef __cplusplus

// C++ side of the types shared with Slang (NUMOS: rewritten without DXVK/MathLib).
// Everything lives in namespace numos so a host's own vec3/mat4 cannot collide.
// Layout must match the Slang side under -fvk-use-scalar-layout; the generated
// layout checks (numos/lib/layout_checks.h) verify the shared structs.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

#define NUMOS_SHARED_BEGIN namespace numos {
#define NUMOS_SHARED_END }

namespace numos {

// Scalars

typedef uint32_t uint;
typedef uint16_t half;

// Vectors

struct vec2 {
  float x;
  float y;

  vec2() = default;
  constexpr vec2(float _x, float _y) : x(_x), y(_y) {}

  vec2 operator+(const vec2& o) const { return vec2(x + o.x, y + o.y); }
  vec2 operator-(const vec2& o) const { return vec2(x - o.x, y - o.y); }
  vec2 operator*(float s) const { return vec2(x * s, y * s); }
  friend vec2 operator*(float s, const vec2& v) { return vec2(s * v.x, s * v.y); }
};

struct vec3 {
  float x;
  float y;
  float z;

  vec3() = default;
  constexpr explicit vec3(float _s) : x(_s), y(_s), z(_s) {}
  constexpr vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

  vec3 operator+(const vec3& o) const { return vec3(x + o.x, y + o.y, z + o.z); }
  vec3 operator-(const vec3& o) const { return vec3(x - o.x, y - o.y, z - o.z); }
  vec3 operator*(float s) const { return vec3(x * s, y * s, z * s); }
  vec3 operator*(const vec3& o) const { return vec3(x * o.x, y * o.y, z * o.z); }
  friend vec3 operator*(float s, const vec3& v) { return vec3(s * v.x, s * v.y, s * v.z); }
};

struct alignas(16) vec4 {
  float x;
  float y;
  float z;
  float w;

  vec4() = default;
  constexpr explicit vec4(float _s) : x(_s), y(_s), z(_s), w(_s) {}
  constexpr vec4(vec2 _xy, float _z, float _w) : x(_xy.x), y(_xy.y), z(_z), w(_w) {}
  constexpr vec4(vec3 _xyz, float _w) : x(_xyz.x), y(_xyz.y), z(_xyz.z), w(_w) {}
  constexpr vec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

  vec4 operator+(const vec4& o) const { return vec4(x + o.x, y + o.y, z + o.z, w + o.w); }
  vec4 operator-(const vec4& o) const { return vec4(x - o.x, y - o.y, z - o.z, w - o.w); }
  vec4 operator*(float s) const { return vec4(x * s, y * s, z * s, w * s); }
  friend vec4 operator*(float s, const vec4& v) { return vec4(s * v.x, s * v.y, s * v.z, s * v.w); }
};

// MathLib name used by camera.h (VolumeDefinitionCamera::worldPlanes); same layout as vec4.
using float4 = vec4;

struct f16vec2 { uint16_t x, y; };
struct f16vec3 { uint16_t x, y, z; };
struct f16vec4 { uint16_t x, y, z, w; };
struct ivec2 { int x, y; };
struct uvec2 { uint x, y; };
struct uvec3 { uint x, y, z; };
struct uvec4 { uint x, y, z, w; };
struct u16vec2 { uint16_t x, y; };
struct u16vec3 { uint16_t x, y, z; };
struct u16vec4 { uint16_t x, y, z, w; };

// Matrices: column-major, four columns (matches -matrix-layout-column-major).

struct alignas(16) mat4 {
  vec4 m[4];

  mat4() = default;
  // 16 floats, column-major (column c at data[4c .. 4c+3]); same memory as DXVK's Matrix4.
  explicit mat4(const float* data) { std::memcpy(m, data, sizeof(m)); }

  vec4& operator[](size_t index) { return m[index]; }
  const vec4& operator[](size_t index) const { return m[index]; }
};

struct alignas(16) mat4x3 {
  vec3 m[4];

  mat4x3() = default;
  // First three rows of a column-major 4x4 (data as for mat4).
  explicit mat4x3(const float* data) {
    for (uint32_t i = 0; i < 4; i++) {
      std::memcpy(&m[i], data + 4 * i, sizeof(float) * 3);
    }
  }

  vec3& operator[](size_t index) { return m[index]; }
  const vec3& operator[](size_t index) const { return m[index]; }
};

inline float dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

inline float uintBitsToFloat(uint32_t x) {
  float y;
  std::memcpy(&y, &x, sizeof(y));
  return y;
}

inline int32_t floatBitsToInt(float x) {
  int32_t y;
  std::memcpy(&y, &x, sizeof(y));
  return y;
}

} // namespace numos

#else // __cplusplus

#define NUMOS_SHARED_BEGIN
#define NUMOS_SHARED_END

#include "numos/util/shader_types.slangh"

#endif // __cplusplus
