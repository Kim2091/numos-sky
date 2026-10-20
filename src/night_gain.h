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

inline float nightGainInForce(float twilight, float night, float stops) {
  const float g = std::min(std::max(stops, 0.0f), 16.0f);
  if (!(g > 0.0f) || !(night > 0.0f) || !(twilight > 0.0f)) {
    return 1.0f;
  }
  const float x = std::log2(twilight / night) - g;
  // The blend spans the gain's own size in stops, but never reaches into the day: it is over by the time
  // the twilight is back at daylight (1), so the day keeps no gain. Only a boosted night within a few
  // stops of daylight (a full moon at +16) then brightens by a fraction of a percent near sunset.
  const float width = std::max(std::min(g, std::log2(1.0f / night) - g), 1.0f);
  const float t = std::min(std::max(x / width, 0.0f), 1.0f);
  const float s = t * t * (3.0f - 2.0f * t);
  return std::exp2(g * (1.0f - s));
}

} // namespace numos
