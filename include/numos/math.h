// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0 AND Zlib
//
// This is an altered version of parts of DXVK's util_vector.h and util_math.h,
// Copyright (c) 2017-2021 Philip Rebohle, Copyright (c) 2019-2021 Joshua Ashton,
// under the zlib/libpng licence (licenses/LICENSE-DXVK-zlib.txt).

#pragma once

// CPU-side vector math for NUMOS. The formulas copy DXVK's util_vector.h / util_math.h
// operation for operation, so the ported code produces bit-identical floats to the Remix
// integration it came from.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "numos/util/shader_types.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // anonymous struct in union (x/y/z aliases of data[])
#endif

namespace numos {

static constexpr float kPi = 3.141592653589793f;
static constexpr float kDegreesToRadians = kPi / 180.0f;
static constexpr float kRadiansToDegrees = 180.0f / kPi;

struct Vector2 {
  union {
    float data[2];
    struct { float x, y; };
  };

  constexpr Vector2() : x(0.0f), y(0.0f) {}
  constexpr explicit Vector2(float s) : x(s), y(s) {}
  constexpr Vector2(float _x, float _y) : x(_x), y(_y) {}
  Vector2(const vec2& v) : x(v.x), y(v.y) {}
  operator vec2() const { return vec2(x, y); }

  float& operator[](uint32_t i) { return data[i]; }
  const float& operator[](uint32_t i) const { return data[i]; }

  bool operator==(const Vector2& o) const { return x == o.x && y == o.y; }
  bool operator!=(const Vector2& o) const { return !(*this == o); }
  Vector2 operator-() const { return Vector2(-x, -y); }
  Vector2 operator+(const Vector2& o) const { return Vector2(x + o.x, y + o.y); }
  Vector2 operator-(const Vector2& o) const { return Vector2(x - o.x, y - o.y); }
  Vector2 operator*(float s) const { return Vector2(s * x, s * y); }
  Vector2 operator*(const Vector2& o) const { return Vector2(x * o.x, y * o.y); }
  Vector2 operator/(float s) const { return Vector2(x / s, y / s); }
  Vector2 operator/(const Vector2& o) const { return Vector2(x / o.x, y / o.y); }
  Vector2& operator+=(const Vector2& o) { x += o.x; y += o.y; return *this; }
  Vector2& operator-=(const Vector2& o) { x -= o.x; y -= o.y; return *this; }
  Vector2& operator*=(float s) { x *= s; y *= s; return *this; }
  Vector2& operator/=(float s) { x /= s; y /= s; return *this; }
};

struct Vector3 {
  union {
    float data[3];
    struct { float x, y, z; };
    struct { float r, g, b; };
  };

  constexpr Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
  constexpr explicit Vector3(float s) : x(s), y(s), z(s) {}
  constexpr Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
  constexpr Vector3(const Vector2& v, float _z) : x(v.x), y(v.y), z(_z) {}
  Vector3(const vec3& v) : x(v.x), y(v.y), z(v.z) {}
  operator vec3() const { return vec3(x, y, z); }

  float& operator[](uint32_t i) { return data[i]; }
  const float& operator[](uint32_t i) const { return data[i]; }
  Vector2 xy() const { return Vector2(x, y); }

  bool operator==(const Vector3& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator!=(const Vector3& o) const { return !(*this == o); }
  Vector3 operator-() const { return Vector3(-x, -y, -z); }
  Vector3 operator+(const Vector3& o) const { return Vector3(x + o.x, y + o.y, z + o.z); }
  Vector3 operator-(const Vector3& o) const { return Vector3(x - o.x, y - o.y, z - o.z); }
  Vector3 operator*(float s) const { return Vector3(s * x, s * y, s * z); }
  Vector3 operator*(const Vector3& o) const { return Vector3(x * o.x, y * o.y, z * o.z); }
  Vector3 operator/(float s) const { return Vector3(x / s, y / s, z / s); }
  Vector3 operator/(const Vector3& o) const { return Vector3(x / o.x, y / o.y, z / o.z); }
  Vector3& operator+=(const Vector3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vector3& operator-=(const Vector3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vector3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  Vector3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
};

struct Vector4 {
  union {
    float data[4];
    struct { float x, y, z, w; };
  };

  constexpr Vector4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) {}
  constexpr explicit Vector4(float s) : x(s), y(s), z(s), w(s) {}
  constexpr Vector4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
  constexpr Vector4(const Vector3& v, float _w) : x(v.x), y(v.y), z(v.z), w(_w) {}
  Vector4(const vec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}
  operator vec4() const { return vec4(x, y, z, w); }

  float& operator[](uint32_t i) { return data[i]; }
  const float& operator[](uint32_t i) const { return data[i]; }
  Vector3 xyz() const { return Vector3(x, y, z); }

  Vector4 operator+(const Vector4& o) const { return Vector4(x + o.x, y + o.y, z + o.z, w + o.w); }
  Vector4 operator-(const Vector4& o) const { return Vector4(x - o.x, y - o.y, z - o.z, w - o.w); }
  Vector4 operator*(float s) const { return Vector4(s * x, s * y, s * z, s * w); }
};

inline Vector2 operator*(float s, const Vector2& v) { return v * s; }
inline Vector3 operator*(float s, const Vector3& v) { return v * s; }
inline Vector4 operator*(float s, const Vector4& v) { return v * s; }

inline float dot(const Vector2& a, const Vector2& b) { return a.x * b.x + a.y * b.y; }
inline float dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(const Vector4& a, const Vector4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Vector3 cross(const Vector3& a, const Vector3& b) {
  return Vector3(a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x, a.x * b.y - b.x * a.y);
}

template<typename V> float lengthSqr(const V& a) { return dot(a, a); }
template<typename V> float length(const V& a) { return std::sqrt(lengthSqr(a)); }
template<typename V> V normalize(const V& a) { return a * (1.0f / length(a)); }

inline Vector3 (min)(const Vector3& a, const Vector3& b) {
  return Vector3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
}
inline Vector3 (max)(const Vector3& a, const Vector3& b) {
  return Vector3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
}
inline Vector3 clamp(const Vector3& a, const Vector3& lo, const Vector3& hi) {
  return Vector3(std::clamp(a.x, lo.x, hi.x), std::clamp(a.y, lo.y, hi.y), std::clamp(a.z, lo.z, hi.z));
}

template<typename T>
constexpr T clamp(T n, T lo, T hi) {
  if (n < lo) return lo;
  if (n > hi) return hi;
  return n;
}

template<typename T, typename U = T>
constexpr T lerp(T a, U b, const float t) {
  return a + t * (b - a);
}

} // namespace numos

#ifdef _MSC_VER
#pragma warning(pop)
#endif
