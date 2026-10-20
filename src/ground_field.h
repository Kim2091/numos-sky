// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The automatic ground datum's field (docs/DESIGN.md "Ground level"): where the ground is, at the
// scale the cloud deck should follow it.
//
// Ground points (probe hits against the host's TLAS, or points a host pushes) land in world-anchored
// square cells. Each cell keeps its most recent samples and takes a LOW quantile of them, so it
// measures the ground rather than the roofs, canopies and rocks standing on it: a roof is a sample
// above the ground, never below it, and the ground between buildings and under trees is reached by
// most probes. The field at a point is the Gaussian-weighted mean of the cells' estimates (a
// normalized convolution: unmeasured cells carry no weight), pulled toward the mean of every
// cell measured this session where no cell is near, so it is defined everywhere and has no edges.
//
// Stability: the field depends on where you are, never on when, except while the measurement of a
// place improves. A cell's published estimate follows its statistic at a bounded rate, and a new
// cell's weight fades in over a few seconds, so a refinement moves the datum slowly; after a camera
// cut both happen at once (snap). A different world (a teleport, a load) is reset(), by the renderer.
//
// Units: the host's world units, in the atmosphere's Y-up frame (toYUp of a world position): x and z
// span the ground plane, heights run along the physical up axis. World-anchored: positions are
// world positions (render + world origin), so cells stay put when the render origin moves.

#include <array>
#include <cstdint>
#include <unordered_map>

namespace numos {

class GroundField {
public:
  static constexpr int    kSamplesPerCell = 64;
  static constexpr float  kLowQuantile = 0.25f;       // of a cell's samples
  static constexpr int    kConfidentSamples = 8;      // a cell's weight reaches 1 at this many samples
  static constexpr double kFadeInSeconds = 4.0;       // a new cell's weight fades in over this
  static constexpr float  kKernelSigmas = 3.0f;       // the Gaussian is cut at this many sigmas
  static constexpr float  kPriorWeight = 0.02f;       // weight of the session mean, against a cell's 1
  static constexpr size_t kMaxCells = 4096;           // oldest cells are forgotten past this

  // `cellSize` and `sigma` in world units. A change discards the field (cells live on the grid).
  void configure(float cellSize, float sigma);
  void reset();

  // A ground point. `immediate`: the cell needs no fade-in (after a camera cut).
  void addSample(float x, float z, float height, double nowSeconds, bool immediate);
  // Moves each cell's published estimate toward its statistic by at most `maxRate` world units per
  // second; all the way with `snap`, and for a cell still fading in.
  void update(double nowSeconds, float dtSeconds, float maxRate, bool snap);
  // The smoothed ground height at (x, z); false while nothing has been measured.
  bool evaluate(float x, float z, double nowSeconds, float& height) const;

  size_t cellCount() const { return m_cells.size(); }
  float cellSize() const { return m_cellSize; }
  float sigma() const { return m_sigma; }

private:
  struct Cell {
    std::array<float, kSamplesPerCell> samples {};
    uint32_t count = 0;        // samples ever added (the ring holds the last kSamplesPerCell)
    float    statistic = 0.0f; // kLowQuantile of the ring
    float    published = 0.0f; // what the field uses: follows the statistic at a bounded rate
    bool     hasPublished = false;
    double   fadeStart = 0.0;  // weight fades in from here; -inf-like for immediate cells
    double   lastSample = 0.0;
  };
  static uint64_t key(int32_t ix, int32_t iz);
  float weight(const Cell& cell, double nowSeconds) const;
  void evictOldest();

  std::unordered_map<uint64_t, Cell> m_cells;
  float m_cellSize = 0.0f;
  float m_sigma = 0.0f;
};

} // namespace numos
