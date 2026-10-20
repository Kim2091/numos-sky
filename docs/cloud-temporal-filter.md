# Screen clouds: temporal filter and sparse-march reconstruction

`shaders/numos/passes/cloud_render.comp.slang`, 2026-10-11. Replaces the 2026-10-05 filter (a fixed
exponential history clipped to the fresh neighbourhood every frame) and the 2026-10-05 / 10-10 cell-march
fill.

## What was wrong

In-game evidence (sunset, fine bright cloud rims, written without a GPU run):

- With the clouds completely still the image still shimmered. `cloudTemporalHistoryWeight` 0 gave much
  more, finer shimmer; 0.98 helped but did not cure it. The prev-projection jitter fix in the host changed
  nothing.
- At DLSS Ultra Performance the clouds showed hard staircase edges a couple of render pixels wide. Turning
  the quarter march off fixed it; the sixteenth march was worse.

Causes, from the code:

1. **A fixed exponential history never converges.** With weight w the history keeps a residual of the
   input noise of sqrt((1 - w) / (1 + w)) (0.16 sigma at 0.95, 0.10 at 0.98), and every frame moves a pixel
   by (1 - w) of its fresh sample's deviation. Under a grazing sun the light lottery is strongly
   right-skewed (rare bright samples carry much of the mean), so that per-frame move is large exactly on
   rims.
2. **Per-frame clipping to raw samples re-injects noise.** The history was clipped every frame to
   mean +- 1.25 sigma (transmittance) / 4 sigma (radiance) of the fresh 3x3 neighbourhood of raw jittered
   samples, and since the rim fix also to the range of their radiance per opacity. The box moves with the
   samples' noise, so at high-contrast detail the clip moved the history every frame.
3. **The cell march aliased with the host's camera jitter.** Cloud rays used the jittered camera. A pixel
   of a 2x2 (4x4) cell is marched every 4th (16th) frame, and the host's base-2 Halton jitter read at
   stride 4 (16) stays inside one quarter (sixteenth) of the pixel, a different one for each position in
   the cell. Each pixel's history converged to the value at a fixed sub-pixel spot, with a 2-pixel
   (4-pixel) periodic pattern of spots: stairs along every edge, which an upscaler at 3x per axis turns
   into blocks.
4. **The cell fill was not a reconstruction.** Non-marched pixels took a Gaussian of the cells' samples
   and leaned on the history with phase-dependent weights (1.5 / 0.83, 2.5 / 0.9), and the clip box was
   shared by a whole cell: whenever history was rejected or clipped (wind, edges, the rim clamp) the cell
   structure showed, every frame, and the host's upscaler is told to trust the current frame on cloud
   pixels (its bias mask), so nothing hid it.

The march's own jitter (`fastJitter`: blue-noise slice `frameIdx & 63`) was also read at the frame index,
so a quarter-march pixel saw only 16 of the 64 slices; it now walks them by its own sample count.

## Design

Per pixel the pass keeps three things (state texture, bindings 28 / 29, RGBA16F, ping-ponged like the
colour and depth histories):

- **History** (premultiplied radiance, transmittance): the running mean of the values the pixel was given.
  A new value with weight c enters as `(H * N + F * c) / (N + c)`.
- **N**, the history's length in samples. A pixel's own march counts 1. A value interpolated from
  neighbouring marches counts `kCloudInterpolatedMovingWeight` (0.25) while the history is being
  resampled, 0 in a still view, and fully while N < 1 (a reset history fills from it at once).
  N is capped by how the history was reprojected:
  - texel-exact (camera and clouds still: `stillness` 1) at `cloudTemporalStillSamples` (default 64), so a
    still view converges to the mean of each pixel's own marches; the per-frame change falls as 1 / N;
  - resampled (camera or wind moving: the reprojection lands off texel centres, every bilinear resample
    blurs) at about `1 / (1 - cloudTemporalHistoryWeight)` frames, converted to samples by the
    effective samples a frame (`(1 + (A - 1) * 0.25) / A` for cell area A), the old filter's time constant;
  - inside the deck also by the motion-ambiguity lag cap (unchanged, now a length in frames
    `1 + lag / ambiguity`).
- **A fast history** (`kCloudTemporalFastSamples` = 4) of luminance and transmittance, fed the same values
  with the same weights.

**Change test instead of a per-frame clip.** The long and the fast history average the same inputs, so
they differ only by noise unless the content changed (a lighting change, cloud evolving, history from
another surface the depth tests let through). The expected noise of their difference is the fresh
neighbourhood's spread times `sqrt(1 / N_fast + 1 / N)`; with a tolerance of 4 of those plus 2 %
luminance / 0.01 transmittance, an excess e > 1 shrinks N by e^2 and clips the history toward the fresh
neighbourhood in proportion (`saturate(e - 1)`). The clip (`cloudTemporalClip`) is the rim-fix clip
unchanged: transmittance to the box, radiance rescaled by the opacity ratio, then kept inside the fresh
samples' radiance-per-opacity range, so it never extrapolates a brightness. It just no longer runs on an
unchanged view. This is the ReLAX-style anti-lag (slow vs fast history) rather than SVGF moments: it needs
no per-pixel variance history, and the neighbourhood spread is a conservative noise estimate (it includes
signal at edges, which only makes the test less eager there).

**Fill for a short history.** The new value is `lerp(wide, best, saturate(N / 4))`: a Gaussian of the
fresh samples (sigma one lattice spacing) while the history is short, the pixel's own march (or the
lattice interpolation, below) once it has a few samples. The spatial denoise pass still works where N is
below 8 (the fraction of `depth.w`, as before).

**Reconstruction of the sparse lattice.** The quarter / sixteenth march samples form a lattice of spacing
2 / 4 shifted by the frame's phase. Each pixel gathers the samples of the 3x3 cells around its own that
see its surface (`cloudSameSurface`, unchanged) into a `CloudNeighbourhood`: its own march if it is the
cell's pixel this frame, the bilinear interpolation of the lattice at the pixel (tent weights over the
samples' actual offsets, radius one spacing: the four lattice points around the pixel, never the nearest
copied), the wide Gaussian, and the moments for the change test. The full-rate path is the same with
spacing 1. No weights depend on the phase, and no box is shared by a cell.

**Rays.** Every march of a pixel goes through it at its own sub-pixel offset, R2 over the pixel's own
sample count, rotated per pixel by blue noise (Cranley-Patterson), along the **unjittered** camera, and the
history is reprojected with the unjittered matrices (`prevWorldToProjection`). The offsets cover the pixel
evenly whatever the host's jitter does, so each pixel converges to its box-filtered value and the layer
carries no per-frame jitter. A still camera reprojects texel-exactly, which is what `stillness` measures.
The march's view and light jitter (`fastJitter`) take the same own sample count.

## Choosing the march rate (2026-10-11)

`rtx.atmosphere.cloudMarchRate`: 0 Auto (default), 1 Full, 2 Quarter, 3 Sixteenth. It replaces the
`cloudQuarterMarch` / `cloudSixteenthMarch` switches. An explicit old switch migrates once to the fixed rate it
asked for (`cloudSixteenthMarch = True` -> 3, `cloudQuarterMarch = True / False` -> 2 / 1; a lone
`cloudSixteenthMarch = False` asked for nothing and leaves Auto). The performance preset no longer forces the
sixteenth march. `FrameOutputs::cloudMarchStride` reports the stride in use (1, 2, 4).

In-game evidence after the filter rewrite: the quarter march is fine at native and DLSS Quality, noisy and "not
worth using" at Balanced and below, and the full-rate layer scales cleanly even at Ultra Performance. Auto
encodes why, with two tests on the stride k (a pixel's own march every k^2 frames):

1. **Motion.** While the history is resampled it keeps about `F = 1 / (1 - cloudTemporalHistoryWeight)` frames
   (20 at 0.95). The change test compares the long history with a 4-sample fast history; to tell noise from
   change, the window must hold at least that many of the pixel's own marches: `F / k^2 >= 4`. So k <= 2 at the
   default weight, and the sixteenth march (16 frames per own sample, 1.25 in the window) never qualifies at any
   allowed weight (max 0.98, F = 50): it stays a manual choice.

2. **Magnification.** The layer reaches the screen at the upscaler's ratio `s = outputHeight / renderHeight`, and
   the upscaler is told to trust it, so its noise and its reconstruction show at s output pixels per render pixel.
   A stride-k march costs what a full-rate march at 1/k of the resolution costs, and in motion it is about as
   noisy: k^2 fewer own samples at the same pixel area, against k^2 larger pixels with the full count (the noise
   energy per unit of screen, `sigma^2 * area / samples`, is the same). In a still view it converges to the full
   resolution, which the coarse full-rate march never does. So the sparse march is acceptable exactly where that
   equal-cost full-rate march would be:
   - no coarser than the coarsest upscale the full-rate layer holds up at. That is the host's own limit, DLSS
     Ultra Performance, 3x (and in game the full-rate layer scaled cleanly there): `k * s <= 3`;
   - no fewer lines than that ratio leaves at the lowest output the renderer targets (1080p / 3):
     `renderHeight / k >= 360`.

Auto takes the largest k passing both. Results at the defaults:

| Output | Mode | s | Stride |
|---|---|---|---|
| any >= 720 lines | native / DLAA | 1 | 2 (quarter) |
| 1080p, 1440p, 4K | DLSS Quality | 1.5 | 2 |
| any | Balanced | 1.72 | 1 (full) |
| any | Performance | 2 | 1 |
| any | Ultra Performance | 3 | 1 |
| < 720 lines | native | 1 | 1 |

This matches the visual verdict without being fitted to it: the anchor is the host's upscale range and the
filter's own fast-history length.

The host reports the output size in `FrameInputs::outputWidth / outputHeight` (0 = no upscaling). Remix passes
its final output extent.

**Switching.** A DLSS mode change changes the render resolution, which reallocates the cloud targets and starts
a fresh history (the history is per render pixel; there is nothing valid to keep). A rate change at the same
resolution (an option change) keeps the history: the resolve handles any lattice spacing, and N counts samples,
not frames, so it stays meaningful; only the cap's conversion (samples per frame) changes with the stride.

## What it costs and what to check in game

- One more RGBA16F pair at the cloud render resolution; per pixel a few more ALU in the resolve. The
  quarter march still does its four pixels per lane from groupshared memory.
- In game (not run; no GPU was available when this was written):
  - a still view at sunset on bright rims: should settle within about a second (full rate) or four
    (quarter march, an own sample every 4 frames) and stop shimmering;
  - DLSS Ultra Performance with the quarter and the sixteenth march: no stairs or blocks; edges as soft as
    the full-rate path's;
  - wind on: no ghosting behind moving cloud edges (the change test and the moving cap), detail no softer
    than before;
  - flying through cloud: no worse smearing (lag cap unchanged);
  - lightning: flashes still follow (history and fast history scaled);
  - camera turning: no trails at the screen edge (reprojection out of view drops the history).
- Knobs: `cloudTemporalStillSamples` (still-view length), `cloudTemporalHistoryWeight` (moving length),
  and the constants at the top of the temporal section (`kCloudTemporalChangeSigmas`,
  `kCloudInterpolatedMovingWeight`, `kCloudTemporalStillTexels`).

## Risks

- The change test uses the neighbourhood's spread as its noise estimate. Where the march noise is far
  more skewed than that spread suggests, a rare bright sample can still trip it and shorten one pixel's
  history (a brief local return to the old filter's behaviour, not a pop).
- A slow lighting change in a still view is followed only as fast as the test detects it: with N = 64 a
  drift smaller than the tolerance lags by up to 64 own samples (about a second at full rate, four with
  the quarter march). Lower `cloudTemporalStillSamples` if sunsets lag.
- The cloud layer is now pixel-centred and box-filtered, not jittered with the host's primary rays. At a
  geometry silhouette the cloud ray and the primary ray can see different sides within a pixel; the
  surface-distance tests treat it as before.
