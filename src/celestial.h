// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The night sky's geometry, computed on the host once per frame: where the Moon and the stars are
// for a date and an observer, and how the Moon is lit. Positions use the low-precision formulae of
// the Astronomical Almanac (sun ~0.01 deg, moon ~0.3 deg in longitude, 0.2 deg in latitude), which
// is far below what anyone can see in a rendered sky.

#include "numos/math.h"

namespace numos::celestial {

// Days since J2000.0 (2000-01-01 12:00 UT) of `hours` on day `dayOfYear` (1 = 1 January) of `year`,
// with the hours read as UT. The sky only needs a consistent clock: local solar time at longitude 0.
double daysSinceJ2000(int year, int dayOfYear, double hours);

// The sun's apparent geocentric ecliptic longitude and the obliquity of the ecliptic (radians).
void sunEcliptic(double days, double& longitude, double& obliquity);

// The Moon's geocentric ecliptic longitude and latitude (radians) and distance (Earth radii).
void moonEcliptic(double days, double& longitude, double& latitude, double& distanceEarthRadii);

// Ecliptic -> equatorial unit vector (x toward RA 0h, z toward the north celestial pole).
Vector3 eclipticToEquatorial(double longitude, double latitude, double obliquity);

// The rotation from equatorial coordinates into the local Y-up frame, as the images of the
// equatorial axes.
struct LocalFrame {
  // Equatorial x / y / z axes in local Y-up coordinates. The local frame is left-handed (x east,
  // y up, z north: the frame the sun's azimuth, clockwise from +Z with east = 90, describes on a
  // D3D-style host), so the map from the right-handed equatorial frame is a rotation and a mirror,
  // and a cross product of local vectors points the opposite way to the physical one.
  Vector3 x, y, z;
  Vector3 toLocal(const Vector3& e) const { return x * e.x + y * e.y + z * e.z; }
};

// The north celestial pole in the local frame, for latitude `latitudeRad` with north at azimuth
// `northOffsetRad` (azimuth runs clockwise from +Z, the convention the sun's angles use).
Vector3 celestialPoleLocal(float latitudeRad, float northOffsetRad);

// The frame that puts the sun's catalogue position (sunEquatorial) exactly on the sun as drawn
// (sunLocal) and the celestial pole as close to its true place as that allows. With a sun driven by
// the date and latitude both land exactly (to the solar model's accuracy); with a sun placed by hand
// the sky still turns rigidly around it, so the moon's phase and place always agree with the sun on
// screen. Its determinant is -1 (see LocalFrame): east stays on the right of north.
LocalFrame alignedFrame(const Vector3& sunEquatorial, const Vector3& sunLocal, const Vector3& poleLocal);

// The Moon's orientation (equatorial unit vectors): its rotation axis, from Cassini's laws (the
// lunar equator 1.54 deg from the ecliptic, its ascending node at the orbit's descending node), and
// the direction from its centre through selenographic longitude 0 on its equator, which faces the
// Earth's mean direction (mean argument of latitude F from that node). The real direction to the
// Earth then wanders around it by the optical librations, ~8 deg in longitude and ~7 in latitude.
void moonAxes(double days, double obliquity, Vector3& pole, Vector3& primeMeridian);

// The Moon's light, all as fractions of the sun's illuminance at the top of the atmosphere, from
// its phase angle (sun-moon-observer, radians):
//   - disk-integrated illuminance at mean distance: V = -12.73 + 0.026|a| + 4e-9 a^4 (a in degrees;
//     Allen, Astrophysical Quantities) against the sun's -26.74 - 2.5e-6 at full moon, opposition
//     surge included;
//   - earthshine: the Earth's illuminance on the Moon (geometric albedo 0.367, Lambert phase of
//     the Earth seen from the Moon, whose phase angle is pi minus the Moon's) times the full-moon
//     fraction, i.e. what the earthlit disk would deliver to the observer;
//   - the share of the solar disk the Earth leaves visible from the Moon's centre (eclipses): disks
//     of 0.2666 deg (sun) and the Earth's apparent radius plus its atmosphere's ~1.5%, separated by
//     the phase angle.
float moonIlluminanceFraction(float phaseAngleRad);
float earthshineFraction(float phaseAngleRad, float distanceEarthRadii);
float sunVisibleFromMoon(float phaseAngleRad, float distanceEarthRadii);

// The Moon's colour relative to the sun's: lunar reflectance rises toward the red (B-V 0.92 against
// the sun's 0.65), normalised to unit luminance.
Vector3 lunarColour();

static constexpr double kMeanMoonDistanceEarthRadii = 60.27;

}  // namespace numos::celestial
