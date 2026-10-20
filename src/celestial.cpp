// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "celestial.h"

#include <cmath>

namespace numos::celestial {

namespace {
  constexpr double kDeg = 3.14159265358979323846 / 180.0;
  double sind(double deg) { return std::sin(deg * kDeg); }
  double cosd(double deg) { return std::cos(deg * kDeg); }

  bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

  // An orthonormal right-handed basis from a direction and a second, non-parallel one.
  void basis(const Vector3& a, const Vector3& b, Vector3& e1, Vector3& e2, Vector3& e3) {
    e1 = normalize(a);
    Vector3 o = b - e1 * dot(e1, b);
    if (dot(o, o) < 1e-12f) {
      o = std::abs(e1.y) < 0.9f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
      o = o - e1 * dot(e1, o);
    }
    e2 = normalize(o);
    e3 = cross(e1, e2);
  }
}

double daysSinceJ2000(int year, int dayOfYear, double hours) {
  double days = 0.0;
  for (int y = 2000; y < year; ++y) days += isLeap(y) ? 366.0 : 365.0;
  for (int y = year; y < 2000; ++y) days -= isLeap(y) ? 366.0 : 365.0;
  return days + double(dayOfYear - 1) + hours / 24.0 - 0.5;
}

void sunEcliptic(double days, double& longitude, double& obliquity) {
  const double L = 280.460 + 0.9856474 * days;
  const double g = 357.528 + 0.9856003 * days;
  longitude = (L + 1.915 * sind(g) + 0.020 * sind(2.0 * g)) * kDeg;
  obliquity = (23.439 - 0.0000004 * days) * kDeg;
}

void moonEcliptic(double days, double& longitude, double& latitude, double& distanceEarthRadii) {
  const double T = days / 36525.0;
  const double lon = 218.32 + 481267.881 * T
    + 6.29 * sind(135.0 + 477198.87 * T) - 1.27 * sind(259.3 - 413335.36 * T)
    + 0.66 * sind(235.7 + 890534.22 * T) + 0.21 * sind(269.9 + 954397.74 * T)
    - 0.19 * sind(357.5 + 35999.05 * T) - 0.11 * sind(186.5 + 966404.03 * T);
  const double lat = 5.13 * sind(93.3 + 483202.02 * T) + 0.28 * sind(228.2 + 960400.89 * T)
    - 0.28 * sind(318.3 + 6003.15 * T) - 0.17 * sind(217.6 - 407332.21 * T);
  const double parallax = 0.9508 + 0.0518 * cosd(135.0 + 477198.87 * T)
    + 0.0095 * cosd(259.3 - 413335.36 * T) + 0.0078 * cosd(235.7 + 890534.22 * T)
    + 0.0028 * cosd(269.9 + 954397.74 * T);
  longitude = lon * kDeg;
  latitude = lat * kDeg;
  distanceEarthRadii = 1.0 / std::sin(parallax * kDeg);
}

void moonAxes(double days, double obliquity, Vector3& pole, Vector3& primeMeridian) {
  const double T = days / 36525.0;
  const double node = 125.0445479 - 1934.1362891 * T;  // the orbit's mean ascending node
  const double F = 93.2720950 + 483202.0175233 * T;    // the mean argument of latitude
  constexpr double kInclination = 1.54242;
  pole = eclipticToEquatorial((node + 90.0) * kDeg, (90.0 - kInclination) * kDeg, obliquity);
  const Vector3 equatorNode = eclipticToEquatorial((node + 180.0) * kDeg, 0.0, obliquity);
  primeMeridian = equatorNode * float(cosd(F)) + cross(pole, equatorNode) * float(sind(F));
}

Vector3 eclipticToEquatorial(double longitude, double latitude, double obliquity) {
  const double cb = std::cos(latitude), sb = std::sin(latitude);
  const double cl = std::cos(longitude), sl = std::sin(longitude);
  const double ce = std::cos(obliquity), se = std::sin(obliquity);
  return Vector3(float(cb * cl), float(ce * cb * sl - se * sb), float(se * cb * sl + ce * sb));
}

Vector3 celestialPoleLocal(float latitudeRad, float northOffsetRad) {
  const Vector3 north(std::sin(northOffsetRad), 0.0f, std::cos(northOffsetRad));
  return north * std::cos(latitudeRad) + Vector3(0.0f, 1.0f, 0.0f) * std::sin(latitudeRad);
}

LocalFrame alignedFrame(const Vector3& sunEquatorial, const Vector3& sunLocal, const Vector3& poleLocal) {
  Vector3 a1, a2, a3, b1, b2, b3;
  basis(sunEquatorial, Vector3(0.0f, 0.0f, 1.0f), a1, a2, a3);
  basis(sunLocal, poleLocal, b1, b2, b3);
  b3 = b3 * -1.0f;  // the local frame is left-handed (LocalFrame)
  // R v = b1 (a1.v) + b2 (a2.v) + b3 (a3.v); its columns are the images of the equatorial axes.
  LocalFrame f;
  f.x = b1 * a1.x + b2 * a2.x + b3 * a3.x;
  f.y = b1 * a1.y + b2 * a2.y + b3 * a3.y;
  f.z = b1 * a1.z + b2 * a2.z + b3 * a3.z;
  return f;
}

float moonIlluminanceFraction(float phaseAngleRad) {
  const float a = std::abs(phaseAngleRad) / kDegreesToRadians;
  const float magnitude = -12.73f + 0.026f * a + 4e-9f * a * a * a * a;
  return std::pow(10.0f, -0.4f * (magnitude + 26.74f));
}

float earthshineFraction(float phaseAngleRad, float distanceEarthRadii) {
  const float earthPhase = kPi - std::abs(phaseAngleRad);
  const float lambertPhase = (std::sin(earthPhase) + (kPi - earthPhase) * std::cos(earthPhase)) / kPi;
  constexpr float kEarthGeometricAlbedo = 0.367f;
  const float earthOnMoon = kEarthGeometricAlbedo * lambertPhase / (distanceEarthRadii * distanceEarthRadii);
  return earthOnMoon * moonIlluminanceFraction(0.0f);
}

float sunVisibleFromMoon(float phaseAngleRad, float distanceEarthRadii) {
  const float rs = 0.2666f * kDegreesToRadians;
  const float re = std::asin(std::min(1.0f / distanceEarthRadii, 1.0f)) * 1.015f;
  const float d = std::abs(phaseAngleRad);
  if (d >= rs + re) return 1.0f;
  if (d <= re - rs) return 0.0f;
  const float a = rs * rs * std::acos(std::min(std::max((d * d + rs * rs - re * re) / (2.0f * d * rs), -1.0f), 1.0f))
                + re * re * std::acos(std::min(std::max((d * d + re * re - rs * rs) / (2.0f * d * re), -1.0f), 1.0f))
                - 0.5f * std::sqrt(std::max((-d + rs + re) * (d + rs - re) * (d - rs + re) * (d + rs + re), 0.0f));
  return std::max(1.0f - a / (kPi * rs * rs), 0.0f);
}

Vector3 lunarColour() {
  // Relative lunar reflectance at the sRGB primaries' wavelengths (~610 / 550 / 465 nm), normalised
  // to unit luminance (Rec. 709 weights).
  const Vector3 c(1.13f, 1.0f, 0.82f);
  return c * (1.0f / (0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z));
}

}  // namespace numos::celestial
