// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

//!variant cloud_cumulus_noise_baker.comp
//!end-variants

/*
 * Cumulus turbulence volume bake.
 *
 * Fills the 128^3 RGBA8 volume sampleCloudDensityCumulus (cloud_nubis3_common.slangh) reads:
 *   R - billows: inverted Worley FBM (rounded lumps, 4 cells per volume plus two octaves).
 *   G - wisps: "Curly-Alligator", a curl-warped inverted alligator FBM (web-like strands with
 *       curly distortion; Nubis Cubed 2023 p.95), squeezed 2x vertically into sheared streaks.
 *   B, A - horizontal curl (x, z) of a periodic potential, encoded 0.5 + 0.25 * curl; the runtime
 *       distorts the detail lookups with it (Schneider 2015/2017).
 *
 * The volume is periodic in its own [0,1)^3 domain (all lattice periods are integers in
 * cycles-per-volume, and the curl potential is itself periodic), so the runtime's frac()-wrapped
 * sample at any frequency scale tiles without a seam. A fixed pattern with no inputs, baked once at
 * init.
 *
 * Dispatch: 8x8x8 threads, (16, 16, 16) groups -> 128^3 voxels.
 */

#include "numos/lib/atmosphere_common.slangh"

layout(binding = 1, rgba8)
RWTexture3D<float4> cloudDetailNoiseOutput;

// Must match Renderer::Impl::kCloudDetailNoise3DSize in src/renderer_impl.h.
static const float kDetailVolumeSize = 128.0f;

// Lattice frequencies are integer cycles per volume so every channel tiles.
//
// Nubis's own detail texture repeats every ~100 m of world (sample position * 0.01), so all of its
// erosion detail is below 100 m. This volume repeats every cloudNoiseTileKm / cloudDetailScale
// (~2.8 km at the defaults), so 6 cycles plus the octave stack put the wisp detail at ~465..116 m,
// close to the 128-texel Nyquist limit. cloudDetailScale scales the whole volume at run time.
static const float kLowFreqCycles  = 6.0f;

// ---------------------------------------------------------------------------
// Alligator-noise approximation (see file header). p is in lattice-cell
// units; period (integer per axis) wraps the CELL IDS so the field tiles.
// Value = strongest sparse bump minus the runner-up: amplitude-layered lumps
// with creases, in ~[0, 1).
// ---------------------------------------------------------------------------
float alligator3DPeriodic(vec3 p, float seed, vec3 period) {
  vec3 cell = floor(p);
  vec3 f = p - cell;
  float m1 = 0.0f;
  float m2 = 0.0f;
  [unroll]
  for (int dz = -1; dz <= 1; ++dz) {
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
      [unroll]
      for (int dx = -1; dx <= 1; ++dx) {
        vec3 offs = vec3(float(dx), float(dy), float(dz));
        vec3 cw = cell + offs;
        cw -= period * floor(cw / period);  // positive wrap of the cell id
        // Feature point + amplitude from three decorrelated hashes.
        vec3 feat = vec3(hash31(cw + vec3(seed + 11.317f, seed, seed)),
                         hash31(cw + vec3(seed, seed + 47.113f, seed)),
                         hash31(cw + vec3(seed, seed, seed + 73.951f)));
        // A wide amplitude range gives varied lump sizes; uniform lumps look synthetic. The kernel
        // exponent of 1.6 keeps the shoulders soft instead of forming hard plateaus.
        float amp = 0.25f + 0.75f * hash31(cw + vec3(seed + 5.771f, seed + 9.043f, seed + 1.229f));
        float d = length(offs + feat - f);
        // Smooth bump with ~1.4-cell reach so neighboring kernels contest.
        float k = amp * pow(saturate(1.0f - d * 0.7071f), 1.6f);
        m2 = max(m2, min(m1, k));  // runner-up before m1 updates
        m1 = max(m1, k);
      }
    }
  }
  // The gain stretches the typical contrast range toward [0, 1]; the erosion settings absorb the
  // rest of the calibration. A harder gain saturates lumps into flat plateaus with razor creases.
  return saturate((m1 - m2) * 1.25f);
}

float alligatorFbm3DPeriodic(vec3 p, int octaves, float seed, vec3 basePeriod) {
  float value = 0.0f;
  float amplitude = 0.5f;
  float total = 0.0f;
  vec3 pp = p;
  vec3 period = basePeriod;
  for (int i = 0; i < octaves; ++i) {
    value += amplitude * alligator3DPeriodic(pp, seed + 13.7f * float(i), period);
    total += amplitude;
    amplitude *= 0.5f;
    pp *= 2.0f;
    period *= 2.0f;
  }
  return value / max(total, 1e-4f);
}

// Central difference of one periodic Perlin FBM potential along one axis.
float cloudCurlPotentialDiff(vec3 p, vec3 axis, float seed, vec3 period) {
  const float e = 0.25f;
  return (fbmNoise3DPeriodic(p + axis * e, 2, seed, period)
        - fbmNoise3DPeriodic(p - axis * e, 2, seed, period)) / (2.0f * e);
}

// Periodic curl field for the wisp channel's domain warp and the B/A channels: the curl of a vector
// potential (A, B, C) built from three decorrelated periodic Perlin FBMs. It is divergence-free by
// construction and periodic because the potentials are. The bake runs once, so the 12 FBM
// evaluations per voxel do not matter.
vec3 curlNoise3DPeriodic(vec3 p, float seed, vec3 period) {
  const float sA = 12.9f;
  const float sB = 31.7f;
  const float sC = 67.1f;
  const vec3 ex = vec3(1, 0, 0);
  const vec3 ey = vec3(0, 1, 0);
  const vec3 ez = vec3(0, 0, 1);
  float dC_dy = cloudCurlPotentialDiff(p, ey, seed + sC, period);
  float dB_dz = cloudCurlPotentialDiff(p, ez, seed + sB, period);
  float dA_dz = cloudCurlPotentialDiff(p, ez, seed + sA, period);
  float dC_dx = cloudCurlPotentialDiff(p, ex, seed + sC, period);
  float dB_dx = cloudCurlPotentialDiff(p, ex, seed + sB, period);
  float dA_dy = cloudCurlPotentialDiff(p, ey, seed + sA, period);
  return vec3(dC_dy - dB_dz, dA_dz - dC_dx, dB_dx - dA_dy);
}

[shader("compute")]
[numthreads(8, 8, 8)]
void main(uint3 threadId : SV_DispatchThreadID) {
  if (any(threadId >= uint3(128u, 128u, 128u))) {
    return;
  }

  // Voxel center in the volume's own [0,1) periodic domain.
  vec3 p = (vec3(threadId) + 0.5f) / kDetailVolumeSize;

  const vec3 loPeriod = vec3(kLowFreqCycles);

  // Wisps: "Curly-Alligator", an inverted alligator noise (bright web-like strands where kernels
  // contest) sampled through a curl-noise domain warp so the strands curl (Nubis Cubed p.95: wisps
  // form where density falls and the surrounding air billows into the vapour). The warp amplitude
  // is in lattice cells, and the curl field has the same period, so the warped sample still tiles.
  //
  // The vertical cycle count is doubled (the volume's y axis is world-vertical), making the wisps
  // half as tall as they are wide, so edge erosion reads as sheared trailing streaks rather than
  // round webs. Integer per-axis periods keep the tiling exact.
  const vec3 kWispSqueeze = vec3(1.0f, 2.0f, 1.0f);
  const vec3 loPeriodW = loPeriod * kWispSqueeze;
  const float kCurlWarpCells = 0.55f;
  vec3 warpLo = curlNoise3DPeriodic(p * kLowFreqCycles * kWispSqueeze, 101.3f, loPeriodW) * kCurlWarpCells;
  float wispyLo = pow(saturate(1.0f - alligatorFbm3DPeriodic(p * kLowFreqCycles * kWispSqueeze + warpLo, 3, 23.1f, loPeriodW)), 2.0f);
  {
    const vec3 billowPeriod = vec3(4.0f);
    float billow = 1.0f - worleyFbm3DPeriodic(p * 4.0f, 3, 5.3f, billowPeriod) / 0.875f;
    vec3 curl = curlNoise3DPeriodic(p * 2.0f, 311.7f, vec3(2.0f));
    cloudDetailNoiseOutput[threadId] = saturate(vec4(billow, wispyLo, 0.5f + 0.25f * curl.x,
                                                     0.5f + 0.25f * curl.z));
  }
}
