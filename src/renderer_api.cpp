// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Public numos::Renderer: forwards to Renderer::Impl (src/renderer.cpp).
#include "renderer_impl.h"

#include <cstring>

namespace numos {

Renderer::Renderer() : m_impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

void Renderer::setSettings(const Settings& settings) {
  m_impl->m_baseSettings = settings;
  m_impl->m_settings = settings;
}
bool Renderer::setNightSkyAssets(const NightSkyAssets& assets) { return m_impl->setNightSkyAssets(assets); }
const Settings& Renderer::settings() const { return m_impl->m_baseSettings; }

void Renderer::beginFrame(IGpu& gpu, const FrameInputs& inputs) { m_impl->beginFrame(gpu, inputs); }

void Renderer::dispatchPostDepth(IGpu& gpu, GpuHandle linearViewZ, float missLinearViewZ) {
  m_impl->dispatchPostDepth(gpu, linearViewZ, missLinearViewZ);
}

const FrameOutputs& Renderer::outputs() const { return m_impl->m_outputs; }

void Renderer::releaseGpuResources(IGpu& gpu) { m_impl->releaseGpuResources(gpu); }

void Renderer::requestLightningStrike() { Impl::s_lightningStrikeRequested.store(true); }

SimulationState Renderer::simulationState() const {
  SimulationState s;
  s.cloudAdvectOffset[0] = m_impl->m_cloudAdvectOffset.x;
  s.cloudAdvectOffset[1] = m_impl->m_cloudAdvectOffset.y;
  s.cloudEvolutionOffset[0] = m_impl->m_cloudEvolutionOffset.x;
  s.cloudEvolutionOffset[1] = m_impl->m_cloudEvolutionOffset.y;
  s.timeOfDayHours = m_impl->m_timeOfDayHours;
  s.animationSeconds = m_impl->m_animationSeconds;
  return s;
}

void Renderer::setSimulationState(const SimulationState& s) {
  m_impl->m_cloudAdvectOffset = Vector2(s.cloudAdvectOffset[0], s.cloudAdvectOffset[1]);
  m_impl->m_cloudEvolutionOffset = Vector3(s.cloudEvolutionOffset[0], s.cloudEvolutionOffset[1], 0.0f);
  m_impl->m_timeOfDayHours = s.timeOfDayHours;
  // Mark the authored time as seen, or the next frame re-seeds the clock from it (advanceTimeCycle).
  m_impl->m_lastAuthoredTimeOfDayHours = m_impl->m_baseSettings.timeOfDayHours;
  m_impl->m_animationSeconds = s.animationSeconds;
}

void Renderer::Impl::beginFrame(IGpu& gpu, const FrameInputs& inputs) {
  m_inputs = inputs;
  // This frame's effective settings: the host's, with its weather (if any) over them.
  m_settings = m_baseSettings;
  if (inputs.weather) {
    applyWeather(*inputs.weather, m_settings);
  }
  applyQualityLevel();
  PassRecorder rec(gpu);
  updateFrame(rec, inputs.deltaSeconds);
  publishOutputs();
  m_outputs.skyViewSampler = sampler(gpu, Filter::Linear, Filter::Nearest, AddressMode::ClampToEdge);
  m_outputs.cloudNoiseSampler = sampler(gpu, Filter::Linear, Filter::Linear, AddressMode::Repeat, SamplerDesc::kLodClampNone);
  m_outputs.linearClampSampler = m_outputs.skyViewSampler;
}

void Renderer::Impl::releaseGpuResources(IGpu& gpu) {
  for (GpuHandle h : m_ownedResources) {
    gpu.destroy(h);
  }
  m_ownedResources.clear();
  m_samplers.clear();
  // Everything else is re-created on the next frame.
  Impl fresh;
  fresh.m_baseSettings = m_baseSettings;
  fresh.m_settings = m_settings;
  // The night sky data survives: it is re-uploaded on the next frames. Reset the upload state too,
  // or uploadNightSky sees its generation as already started (buffers created, cursor at the end)
  // and marks it uploaded with no GPU buffers behind it.
  fresh.m_nightSky = std::move(m_nightSky);
  fresh.m_nightSky.uploadedGeneration = 0;
  fresh.m_nightSky.uploadingGeneration = 0;
  fresh.m_nightSky.cursor = 0;
  // So does the measured ground (CPU-side and world-anchored); its probes in flight do not.
  fresh.m_groundField = std::move(m_groundField);
  fresh.m_groundClockSeconds = m_groundClockSeconds;
  *this = std::move(fresh);
}

namespace {

// Column-major 4x4: element (row r, column c) at m[c * 4 + r].
void mul(const float a[16], const float b[16], float out[16]) {
  float r[16];
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1]
                     + a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
    }
  }
  std::memcpy(out, r, sizeof(r));
}

// General 4x4 inverse (cofactor expansion); column-major in and out.
void invert(const float m[16], float out[16]) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  det = det != 0.0f ? 1.0f / det : 0.0f;
  for (int i = 0; i < 16; ++i) {
    out[i] = inv[i] * det;
  }
}

mat4 toMat4(const float m[16]) { return mat4(m); }

} // namespace

Camera makeCamera(const float worldToView[16], const float viewToProjection[16],
                  const float prevWorldToView[16], const float prevViewToProjection[16],
                  float jitterX, float jitterY, uint32_t width, uint32_t height,
                  float nearPlane, bool rightHanded) {
  // Jitter in NDC, applied as a translation after the projection.
  float jitter[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  jitterX, jitterY, 0, 1 };
  float viewToWorld[16], projectionToView[16], viewToProjectionJittered[16], projectionToViewJittered[16];
  float worldToProjectionJittered[16], projectionToWorldJittered[16];
  float prevViewToWorld[16], prevWorldToProjection[16], prevProjectionToView[16];
  invert(worldToView, viewToWorld);
  invert(viewToProjection, projectionToView);
  mul(jitter, viewToProjection, viewToProjectionJittered);
  invert(viewToProjectionJittered, projectionToViewJittered);
  mul(viewToProjectionJittered, worldToView, worldToProjectionJittered);
  invert(worldToProjectionJittered, projectionToWorldJittered);
  invert(prevWorldToView, prevViewToWorld);
  mul(prevViewToProjection, prevWorldToView, prevWorldToProjection);
  invert(prevViewToProjection, prevProjectionToView);

  Camera c {};
  c.worldToView = toMat4(worldToView);
  c.viewToWorld = toMat4(viewToWorld);
  c.viewToProjection = toMat4(viewToProjection);
  c.projectionToView = toMat4(projectionToView);
  c.viewToProjectionJittered = toMat4(viewToProjectionJittered);
  c.projectionToViewJittered = toMat4(projectionToViewJittered);
  c.worldToProjectionJittered = toMat4(worldToProjectionJittered);
  c.projectionToWorldJittered = toMat4(projectionToWorldJittered);
  // No translated-world (camera-relative) frame: translation offset zero, so these equal the world ones.
  c.translatedWorldToView = c.worldToView;
  c.translatedWorldToProjectionJittered = c.worldToProjectionJittered;
  c.projectionToTranslatedWorld = c.projectionToWorldJittered;
  c.prevWorldToView = toMat4(prevWorldToView);
  c.prevViewToWorld = toMat4(prevViewToWorld);
  c.prevWorldToProjection = toMat4(prevWorldToProjection);
  float prevJittered[16];
  float prevViewToProjectionJittered[16];
  mul(jitter, prevViewToProjection, prevViewToProjectionJittered);
  mul(prevViewToProjectionJittered, prevWorldToView, prevJittered);
  c.prevWorldToProjectionJittered = toMat4(prevJittered);
  c.prevProjectionToView = toMat4(prevProjectionToView);
  float prevProjectionToViewJittered[16];
  invert(prevViewToProjectionJittered, prevProjectionToViewJittered);
  c.prevProjectionToViewJittered = toMat4(prevProjectionToViewJittered);
  c.prevTranslatedWorldToView = c.prevWorldToView;
  c.prevTranslatedWorldToProjection = c.prevWorldToProjection;
  float projectionToPrevProjection[16], tmp[16];
  mul(prevWorldToProjection, viewToWorld, tmp);
  mul(tmp, projectionToView, projectionToPrevProjection);
  c.projectionToPrevProjection = toMat4(projectionToPrevProjection);
  float projectionToPrevProjectionJittered[16];
  mul(prevJittered, projectionToWorldJittered, projectionToPrevProjectionJittered);
  c.projectionToPrevProjectionJittered = toMat4(projectionToPrevProjectionJittered);
  c.resolution = { width, height };
  c.nearPlane = nearPlane;
  c.flags = rightHanded ? rightHandedFlag : 0u;
  return c;
}

} // namespace numos
