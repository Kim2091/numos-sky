// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Night gain (rtx.atmosphere.nightGainStops; docs/DESIGN.md "Night gain"): the factor the natural night
// light (moons, moonlit sky, starlight, airglow, zodiacal light) is multiplied by this frame.
//
// `twilight`: the sun's sky relative to daylight (relativeSkyLuminance of the sun's elevation; 1 by day).
// `night`: the natural night sky relative to daylight, physical (moons and night glow). `stops`: the setting.
//
// The gain G = 2^stops must not make the sky brighten after sunset, and must not touch the day. In stops
// above the boosted night, x = log2(twilight / (night * G)), the gain is complete at x <= 0 (the twilight
// has fallen to the boosted night's level) and absent at x >= X, with a smoothstep in between; X = the
// gain's own size in stops (at least 1). Over the blend the boosted night stays at least as many stops
// under the twilight as the gain still has to go, so the total sky keeps falling while the sun sets, and
// at x <= 0 it settles on night * G. A full moon at +10 stops completes near a sun depression of 6 degrees;
// at +2 stops near 11; a moonless night later, as its level is lower. The day (twilight 1) keeps no gain.

#include <algorithm>
#include <cmath>

namespace numos {

// The share of a night gain of `stops` in force, from 0 by day to 1 at night: the blend above. A gain of 0
// or less blends over the 1 stop above the natural night.
inline float nightGainFade(float twilight, float night, float stops) {
  if (!(night > 0.0f) || !(twilight > 0.0f)) {
    return 0.0f;
  }
  const float g = std::max(stops, 0.0f);
  const float x = std::log2(twilight / night) - g;
  // The blend spans the gain's own size in stops, but never reaches into the day: it is over by the time
  // the twilight is back at daylight (1), so the day keeps no gain. Only a boosted night within a few
  // stops of daylight (a full moon at +16) then brightens by a fraction of a percent near sunset.
  const float width = std::max(std::min(g, std::log2(1.0f / night) - g), 1.0f);
  const float t = std::min(std::max(x / width, 0.0f), 1.0f);
  const float s = t * t * (3.0f - 2.0f * t);
  return 1.0f - s;
}

inline float nightGainInForce(float twilight, float night, float stops) {
  const float g = std::min(std::max(stops, 0.0f), 16.0f);
  if (!(g > 0.0f) || !(night > 0.0f) || !(twilight > 0.0f)) {
    return 1.0f;
  }
  return std::exp2(g * nightGainFade(twilight, night, g));
}

// The night gains in force this frame, each in stops on top of Night Gain: Star Gain for the star field,
// Moonlight Gain for the moons' light, and Moon Disc Gain for the discs on top of the moonlight.
//
// Each light's total gain (Night Gain plus its own, within -16..16 stops) fades in by the rule above for a
// gain of that size, against the same natural night: 2^(g x fade). The airglow and zodiacal light keep
// Night Gain alone, exactly as nightGainInForce gives it. A gain of 0 or less fades over the stop above
// the natural night; a dimming light only falls as the sun sets. The disc gain fades with the moonlight.
// So every factor is 1 by day and reaches its full value at night, and each light's factor depends only
// on its own settings. The total sky still never brightens after sunset: it is the lights' shares of
// the night, each summed with the twilight, and every one of those sums falls as the sun sets.
//
// `night`: the natural night sky relative to daylight, as above (moons and night glow, physical).
struct NightGains {
  float night = 1.0f;      // airglow and zodiacal light: Night Gain
  float stars = 1.0f;      // the star field and its light: Night Gain + Star Gain
  float moonlight = 1.0f;  // the moons' light: Night Gain + Moonlight Gain
  float moonDisc = 1.0f;   // the visible moon discs, on top of the moonlight: Moon Disc Gain
};

inline NightGains nightGainsInForce(float twilight, float night, float nightStops, float starStops,
                                    float moonlightStops, float moonDiscStops) {
  const auto clampStops = [](float v) { return std::min(std::max(v, -16.0f), 16.0f); };
  const float n = std::min(std::max(nightStops, 0.0f), 16.0f);
  const auto inForce = [&](float stops) {
    return stops > 0.0f ? nightGainInForce(twilight, night, stops)
                        : std::exp2(stops * nightGainFade(twilight, night, 0.0f));
  };
  const float moonlight = clampStops(n + moonlightStops);
  NightGains r;
  r.night = nightGainInForce(twilight, night, n);
  r.stars = starStops == 0.0f ? r.night : inForce(clampStops(n + starStops));
  r.moonlight = moonlightStops == 0.0f ? r.night : inForce(moonlight);
  r.moonDisc = std::exp2(clampStops(moonDiscStops) * nightGainFade(twilight, night, std::max(moonlight, 0.0f)));
  return r;
}

} // namespace numos
