// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "ground_field.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace numos {

uint64_t GroundField::key(int32_t ix, int32_t iz) {
  return (uint64_t(uint32_t(ix)) << 32) | uint64_t(uint32_t(iz));
}

void GroundField::configure(float cellSize, float sigma) {
  if (cellSize != m_cellSize || sigma != m_sigma) {
    reset();
    m_cellSize = cellSize;
    m_sigma = sigma;
  }
}

void GroundField::reset() {
  m_cells.clear();
}

void GroundField::addSample(float x, float z, float height, double nowSeconds, bool immediate) {
  if (!(m_cellSize > 0.0f) || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(height)) {
    return;
  }
  const double fx = std::floor(double(x) / m_cellSize);
  const double fz = std::floor(double(z) / m_cellSize);
  if (std::fabs(fx) > 2e9 || std::fabs(fz) > 2e9) {
    return;
  }
  const uint64_t k = key(int32_t(fx), int32_t(fz));
  auto it = m_cells.find(k);
  if (it == m_cells.end()) {
    if (m_cells.size() >= kMaxCells) {
      evictOldest();
    }
    Cell cell;
    cell.fadeStart = immediate ? -1e300 : nowSeconds;
    it = m_cells.emplace(k, cell).first;
  }
  Cell& cell = it->second;
  if (immediate) {
    cell.fadeStart = -1e300;
  }
  cell.samples[cell.count % kSamplesPerCell] = height;
  ++cell.count;
  cell.lastSample = nowSeconds;

  // The low quantile of what the ring holds.
  const uint32_t n = std::min<uint32_t>(cell.count, kSamplesPerCell);
  std::array<float, kSamplesPerCell> sorted = cell.samples;
  const uint32_t q = uint32_t(std::floor(kLowQuantile * float(n - 1u) + 0.5f));
  std::nth_element(sorted.begin(), sorted.begin() + q, sorted.begin() + n);
  cell.statistic = sorted[q];
  if (!cell.hasPublished) {
    cell.published = cell.statistic;
    cell.hasPublished = true;
  }
}

void GroundField::update(double nowSeconds, float dtSeconds, float maxRate, bool snap) {
  const float step = std::max(dtSeconds, 0.0f) * std::max(maxRate, 0.0f);
  for (auto& [k, cell] : m_cells) {
    (void)k;
    if (snap) {
      cell.published = cell.statistic;
      cell.fadeStart = -1e300;
    } else if (weight(cell, nowSeconds) < 1.0f) {
      // Still fading in: the fade is what keeps the field smooth, so the value need not lag as well
      // (a cell whose first hits were a roof would otherwise crawl down at the rate limit).
      cell.published = cell.statistic;
    } else {
      cell.published += std::min(std::max(cell.statistic - cell.published, -step), step);
    }
  }
}

float GroundField::weight(const Cell& cell, double nowSeconds) const {
  const float samples = std::min(float(cell.count) / float(kConfidentSamples), 1.0f);
  const double age = nowSeconds - cell.fadeStart;
  const float fade = float(std::min(std::max(age / kFadeInSeconds, 0.0), 1.0));
  return samples * fade;
}

bool GroundField::evaluate(float x, float z, double nowSeconds, float& height) const {
  if (m_cells.empty() || !(m_cellSize > 0.0f) || !(m_sigma > 0.0f)) {
    return false;
  }
  // The session mean: what the field relaxes to far from every measurement.
  double meanSum = 0.0, meanWeight = 0.0;
  for (const auto& [k, cell] : m_cells) {
    (void)k;
    const float w = weight(cell, nowSeconds);
    meanSum += double(w) * cell.published;
    meanWeight += w;
  }
  if (!(meanWeight > 0.0)) {
    return false;
  }
  const double mean = meanSum / meanWeight;

  const float reach = kKernelSigmas * m_sigma;
  const int32_t x0 = int32_t(std::floor((x - reach) / m_cellSize));
  const int32_t x1 = int32_t(std::floor((x + reach) / m_cellSize));
  const int32_t z0 = int32_t(std::floor((z - reach) / m_cellSize));
  const int32_t z1 = int32_t(std::floor((z + reach) / m_cellSize));
  const double inv2Sigma2 = 1.0 / (2.0 * double(m_sigma) * double(m_sigma));
  double sum = kPriorWeight * mean;
  double total = kPriorWeight;
  for (int32_t iz = z0; iz <= z1; ++iz) {
    for (int32_t ix = x0; ix <= x1; ++ix) {
      const auto it = m_cells.find(key(ix, iz));
      if (it == m_cells.end()) {
        continue;
      }
      const double dx = (double(ix) + 0.5) * m_cellSize - x;
      const double dz = (double(iz) + 0.5) * m_cellSize - z;
      const double d2 = dx * dx + dz * dz;
      if (d2 > double(reach) * double(reach)) {
        continue;
      }
      const double w = std::exp(-d2 * inv2Sigma2) * weight(it->second, nowSeconds);
      sum += w * it->second.published;
      total += w;
    }
  }
  height = float(sum / total);
  return true;
}

void GroundField::evictOldest() {
  // Forget the least recently measured quarter at once, so a full field does not scan per sample.
  std::vector<double> times;
  times.reserve(m_cells.size());
  for (const auto& [k, cell] : m_cells) {
    (void)k;
    times.push_back(cell.lastSample);
  }
  const size_t drop = std::max<size_t>(times.size() / 4, 1);
  std::nth_element(times.begin(), times.begin() + (drop - 1), times.end());
  const double cutoff = times[drop - 1];
  for (auto it = m_cells.begin(); it != m_cells.end();) {
    it = it->second.lastSample <= cutoff ? m_cells.erase(it) : std::next(it);
  }
}

} // namespace numos
