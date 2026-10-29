// Copyright 2026 Kim2091 (Sparkles)
// Portions Copyright (c) 2025 CattaRappa (sambow23)
// SPDX-License-Identifier: Apache-2.0 AND MIT
// The portions are under the MIT licence (licenses/LICENSE-MIT-contributors.txt);
// see THIRD_PARTY_NOTICES.md.

#pragma once

// Internal state of numos::Renderer (src/renderer.cpp).

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdint>
#include <sstream>
#include <vector>

#include "numos/log.h"
#include "numos/renderer.h"
#include "numos/weather.h"
#include "numos/lib/aerial_perspective_light.h"
#include "numos/lib/cloud_nvdf.h"
#include "numos/lib/generate_mipmap.h"
#include "numos/lib/aerial_perspective_binding_indices.h"
#include "numos/lib/multiscattering_lut_binding_indices.h"
#include "numos/lib/sky_view_lut_binding_indices.h"
#include "numos/lib/transmittance_lut_binding_indices.h"
#include "numos/lib/ground_probe.h"
#include "celestial.h"
#include "ground_field.h"
#include "pass_recorder.h"

namespace numos {

// ---- formatting and logging helpers ----

namespace detail {
inline void formatInto(std::ostringstream&) {}
template<typename T, typename... Rest>
void formatInto(std::ostringstream& s, const T& v, const Rest&... rest) {
  s << v;
  formatInto(s, rest...);
}
} // namespace detail

template<typename... Args>
std::string format(const Args&... args) {
  std::ostringstream s;
  detail::formatInto(s, args...);
  return s.str();
}

inline void logInfo(const std::string& m) { log(LogLevel::Info, m.c_str()); }
inline void logWarn(const std::string& m) { log(LogLevel::Warn, m.c_str()); }
inline void logErr(const std::string& m) { log(LogLevel::Error, m.c_str()); }

// Once per process (shared by every Renderer); atomic, so Renderers on different threads do not race.
#define NUMOS_ONCE(x)                                     \
  do {                                                    \
    static std::atomic<bool> numosOnceFlag { false };     \
    if (!numosOnceFlag.exchange(true)) {                  \
      x;                                                  \
    }                                                     \
  } while (0)

// Column-major 4x4 multiply helpers for the aerial-perspective frustum fit.
inline Vector4 mulColumnMajor(const float m[16], const Vector4& v) {
  return Vector4(m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
                 m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
                 m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
                 m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w);
}

struct Renderer::Impl {
  Impl();

  // ---- frame entry points (Renderer forwards here) ----
  void beginFrame(IGpu& gpu, const FrameInputs& inputs);
  void dispatchPostDepth(IGpu& gpu, GpuHandle linearViewZ, float missLinearViewZ);
  void releaseGpuResources(IGpu& gpu);

  // ---- per-frame work ----
  void initialize(PassRecorder& rec);
  void computeLuts(PassRecorder& rec);
  void updateFrame(PassRecorder& rec, float deltaTimeSeconds);
  void publishOutputs();

  bool needsLutRecompute() const;
  void setAerialPerspectiveCamera();

  struct CloudProfileState {
    uint32_t samples = 0u;
    uint32_t maxSamples = 0u;
    float sampleSpacingKm = 0.0f;
    uint32_t domePeriod    = 1u;
    uint32_t renderWidth   = 0u;
    uint32_t renderHeight  = 0u;
    uint32_t detailLod     = 0u;
    float    detailLodBias = 0.0f;
  };

  void ensureCloudRenderRT(PassRecorder& rec, uint32_t width, uint32_t height);
  void setCloudShadowCameraPosition(const Vector3& cameraWorldPosYUpKm);

  struct CloudAnchor {
    enum class Source : uint32_t {
      CameraViewMatrix = 0,
      CameraWorldOverride = 1,
    };
    Vector3 rawWorldUnits        { 0.0f, 0.0f, 0.0f };
    Vector3 rawWorldUnitsFreecam { 0.0f, 0.0f, 0.0f };
    Vector3 resolvedRawWorldUnits { 0.0f, 0.0f, 0.0f };
    Vector3 posYUpKm     { 0.0f, 0.0f, 0.0f };
    Vector3 prevPosYUpKm { 0.0f, 0.0f, 0.0f };
    Vector3 deltaKm      { 0.0f, 0.0f, 0.0f };
    Source source { Source::CameraViewMatrix };
    uint32_t staticFrameCount { 0u };
    bool everMoved { false };
    float cumulativeRotationRadians { 0.0f };
  };

  float worldUnitsPerKm() const;
  float hazeWorldUnitsPerKm() const;
  float resolveUnitsPerMeter() const;
  Vector3 cloudFrameCameraKm() const;
  Vector3 worldToCloudKm(const Vector3& renderPosition) const;
  Vector3 cloudKmToWorld(const Vector3& cloudKm) const;
  // Ground level (docs/DESIGN.md "Ground level"): manual, or measured into m_groundField.
  void updateGroundLevel(PassRecorder& rec, const Vector3& renderToWorld);
  void dispatchGroundProbes(PassRecorder& rec, const Vector3& renderToWorld, float sigmaWorld);

  AtmosphereArgs getAtmosphereArgs() const;
  void advanceCloudMotion(float dt);
  void advanceTimeCycle(float dt);
  void computeTimeCycleSunAngles(float timeOfDayHours, float& outElevationDeg, float& outAzimuthDeg) const;
  void resolveSunAngles(float& outElevationDeg, float& outAzimuthDeg) const;
  void updateRadianceScale();
  void updateCelestial();
  AtmosphereArgs getCloudArgs() const;
  bool setNightSkyAssets(const NightSkyAssets& assets);
  void uploadNightSky(PassRecorder& rec);
  void advanceLightning(float dt);
  void placeLightning();
  float lightningIntensityKm2() const;
  Vector3 lightningHue() const;
  Vector3 lightningRenderPosition() const;
  void syncDistantLights(const AtmosphereArgs& args);

  // ---- passes and bakes ----
  bool getCloudOffsetAtPlayer(float& offsetWorldUnits) const;
  void traceCloudPlacement(const AtmosphereArgs& args);
  bool m_traceCloudPlacement = false;
  uint32_t m_cloudPlacementTraceFrame = 0;

  void createLutResources(PassRecorder& rec);
  void dispatchTransmittanceLut(PassRecorder& rec);
  void dispatchMultiscatteringLut(PassRecorder& rec);
  void dispatchSkyViewLut(PassRecorder& rec);
  // Aerial perspective (docs/aerial-perspective-v2.md).
  struct ApGrid {
    uint32_t x = 0, y = 0, z = 0;
    float range = 0.0f;  // world units, forward distance
  };
  // Everything the froxel history depends on besides the camera.
  struct ApHistoryKey {
    uint32_t x, y, z;
    float range, unitsPerKm, fogExtinction, fogScaleHeight;
    // Traced and untraced visibility mean different things; neither may inherit the other's.
    uint32_t sceneShadow;
  };
  ApGrid apGrid() const;
  void ensureAerialPerspectiveResources(PassRecorder& rec, const ApGrid& grid);
  void buildAerialPerspectiveLights(PassRecorder& rec);
  void dispatchAerialPerspective(PassRecorder& rec);
  void dispatchAerialPerspectiveNear(PassRecorder& rec, GpuHandle linearViewZ, float missLinearViewZ);
  void dispatchCloudPlacementMapBake(PassRecorder& rec);
  bool needsCloudPlacementRebake() const;
  void cacheCloudPlacementBakeInputs();
  void dispatchCloudSkyTransmittanceLut(PassRecorder& rec);
  void dispatchCloudNvdfOccupancy(PassRecorder& rec);
  void dispatchCloudNvdfJfaPass(PassRecorder& rec, uint32_t mode, uint32_t jumpSizeVoxels,
                                uint32_t srcIdx, uint32_t dstIdx);
  void dispatchCloudNvdfResolve(PassRecorder& rec, uint32_t seedsIdx);
  void runCloudNvdfBakeFull(PassRecorder& rec);
  void stepCloudNvdfBake(PassRecorder& rec);
  bool needsCloudNvdfRebake() const;
  bool nvdfCoverageStale(float gap) const;
  void cacheCloudNvdfBakeInputs();
  void dispatchCloudDetailNoiseBake(PassRecorder& rec);
  void dispatchCloudDetailNoiseMips(PassRecorder& rec, const Texture& volume);
  void updateCloudShadowMap(PassRecorder& rec);
  void updateCloudLayer2Map(PassRecorder& rec);
  void dispatchCloudRender(PassRecorder& rec, GpuHandle linearViewZ);
  void dispatchCloudDenoise(PassRecorder& rec);
  // Scales this frame's effective settings by rtx.atmosphere.qualityLevel. High leaves them unchanged.
  void applyQualityLevel();
  // The screen march stride this frame (1, 2 or 4): cloudMarchRate, Auto resolved from the resolutions
  // and the quality level.
  uint32_t cloudMarchStride() const;
  bool cloudQuarterMarchActive() const { return cloudMarchStride() >= 2u; }
  bool cloudSixteenthMarchActive() const { return cloudMarchStride() >= 4u; }
  void dispatchCloudSampleStatistics(PassRecorder& rec);
  void dispatchCloudSecondaryLut(PassRecorder& rec);
  void generateMipmapGaussian(PassRecorder& rec, const Texture& texture);
  void resolveCloudInterleave(bool cloudInputsChanged);

  // Resource helpers.
  Texture createTexture(PassRecorder& rec, const char* name, TextureType type, Format format,
                        uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevels);
  void replaceTexture(PassRecorder& rec, Texture& slot, Texture created);
  Buffer createBuffer(PassRecorder& rec, const char* name, BufferUsage usage, size_t size);
  void replaceBuffer(PassRecorder& rec, Buffer& slot, Buffer created);
  GpuHandle sampler(IGpu& gpu, Filter filter, Filter mipFilter, AddressMode address, float maxLod = 0.0f);

  static constexpr uint32_t kTransmittanceLutWidth = 512;
  static constexpr uint32_t kTransmittanceLutHeight = 128;
  static constexpr uint32_t kMultiscatteringLutSize = 32;
  static constexpr uint32_t kSkyViewLutWidth = 512;
  static constexpr uint32_t kSkyViewLutHeight = 256;
  static constexpr uint32_t kCloudSkyTransmittanceLutWidth = 32;
  static constexpr uint32_t kCloudSkyTransmittanceLutHeight = 16;
  static constexpr uint32_t kCloudVoxelGridX = 256;
  static constexpr uint32_t kCloudVoxelGridY = 32;
  static constexpr uint32_t kCloudVoxelGridZ = 256;
  static constexpr uint32_t kCloudNvdfSizeXZ = 256;
  static constexpr uint32_t kCloudNvdfSizeY  = 64;
  static constexpr uint32_t kCloudNvdfJumpSchedule[] = { 128, 64, 32, 16, 8, 4, 2, 1, 1 };
  static constexpr uint32_t kCloudNvdfJumpPassCount =
      sizeof(kCloudNvdfJumpSchedule) / sizeof(kCloudNvdfJumpSchedule[0]);
  static constexpr uint32_t kCloudNvdfJumpPassesPerFrame = 2;
  static constexpr uint32_t kCloudDetailNoise3DSize = 128;
  static constexpr uint32_t kCloudDetailNoise3DMipLevels = 8;
  static constexpr uint32_t kCloudSecondaryLutWidth  = 256;
  static constexpr uint32_t kCloudSecondaryLutHeight = 256;
  static constexpr uint32_t kCloudSecondaryLutMipLevels = 6;
  static constexpr uint32_t kCloudPlacementMapSize = 512;
  // Must match lib/cloud_shadow_map.slangh.
  static constexpr uint32_t kCloudShadowMapSize = 256;
  static constexpr uint32_t kCloudShadowMapSlices = 32;
  static constexpr uint32_t kCloudShadowMapCascades = 4;
  static constexpr uint32_t kCloudShadowMapSlots = kCloudShadowMapCascades + 1;  // one spare
  // Layer 2 patch-field map; must match kCloudLayer2Map* in lib/cloud_nubis3_common.slangh.
  static constexpr uint32_t kCloudLayer2MapSize = 1024;
  static constexpr float kCloudLayer2MapCoarseTexelKm = 0.2f;
  static constexpr float kCloudLayer2MapRecentreKm = 2.0f;
  // Far-field LUT: sky-view parameterization x distance, out to 400 km (see aerial_perspective_far).
  static constexpr uint32_t kApFarSizeX = 128;
  static constexpr uint32_t kApFarSizeY = 64;
  static constexpr uint32_t kApFarSlices = 48;
  static constexpr float kApFarRangeKm = 400.0f;
  static constexpr float kApFarLinearDepthKm = 0.5f;
  // The near field's linear region is its range / this (2 m at the default 1 km).
  static constexpr float kApFroxelLinearDepthFraction = 500.0f;
  static constexpr float kRayleighScaleHeight = 8.0f;
  static constexpr float kMieScaleHeight = 1.2f;

  // ---- inputs and outputs ----
  Settings      m_baseSettings;  // as the host set them (Renderer::setSettings)
  Settings      m_settings;      // this frame's: m_baseSettings with FrameInputs::weather applied
  FrameInputs   m_inputs;
  FrameOutputs  m_outputs;
  std::vector<GpuHandle> m_ownedResources;  // everything created, for releaseGpuResources
  std::vector<std::pair<uint64_t, GpuHandle>> m_samplers;  // desc key -> sampler

  // ---- resources ----
  Texture m_transmittanceLut;
  Texture m_multiscatteringLut;
  Texture m_skyViewLut;
  Texture  m_apFarScatter;
  Texture  m_apFarTransmittance;
  Texture  m_apNearScatter;
  Texture  m_apFroxelLighting[2];  // history ping-pong: RGB local light, A sun visibility
  Texture  m_apFroxelAux[2];       // R sky visibility, G age, B local-light visibility, A sun visibility
  Texture  m_apLightInjection;     // host-written local light, same grid
  Texture  m_apNoDepth;            // 1x1 stand-in for linear view Z when the host passes none
  uint32_t m_apHistoryIndex = 0u;  // the pair holding the latest result
  bool     m_apHistoryValid = false;
  uint32_t m_apLastFrame = UINT32_MAX - 1u;  // frame index of the last froxel lighting dispatch
  // Set up by beginFrame, dispatched by dispatchPostDepth once linear view Z exists.
  bool     m_apNearPending = false;
  uint32_t m_apNearFrame = 0u;
  ApGrid   m_apNearGrid {};
  AtmosphereArgs m_apNearArgs {};
  ApHistoryKey m_apHistoryKey {};
  Buffer   m_apLightBuffer;
  Buffer   m_apLightClusterBuffer;
  uint32_t m_apLightCapacity = 0u;
  uint32_t m_apLightCount = 0u;
  uint32_t m_apLightClusterCapacity = 0u;
  // Far-field use (dispatchAerialPerspective): the integration counts the columns whose pixels take
  // the far field, and the count comes back through a readback ring a few frames later. While no
  // pixel has read it for kApFarIdleFrames, the far field updates only every kApFarIdlePeriod frames.
  static constexpr uint32_t kApFarUsageSlots = 3u;
  static constexpr uint32_t kApFarIdleFrames = 30u;
  static constexpr uint32_t kApFarIdlePeriod = 8u;
  Buffer   m_apFarUsage;
  Buffer   m_apFarUsageReadback[kApFarUsageSlots];
  bool     m_apFarUsagePending[kApFarUsageSlots] = {};
  uint32_t m_apFarIdleFrames = 0u;      // consecutive read-back frames without a far-field pixel
  uint32_t m_apFarUpdateFrame = 0u;     // frame index of the last far-field update
  bool     m_apFarValid = false;        // the far field holds a complete update
  bool     m_apFarUsageCounted = false; // the last integration counted into m_apFarUsage

  Texture m_cloudSkyTransmittanceLut;
  // Cloud sun-space shadow map and the state it was baked for (updateCloudShadowMap).
  Texture m_cloudShadowMap;
  Texture m_cloudHorizonMap;
  Texture m_cloudBeerMap;  // Beer form of the sun map, one layer per slot (cloudPerformancePreset 1)
  Texture m_cloudVerticalOd;  // cloud_vertical_od.comp: optical depth below, four shrinks, NVDF layout
  // Layer 2 patch-field map (cloud_layer2_map.comp) and what it was baked for (updateCloudLayer2Map).
  Texture m_cloudLayer2Map;
  struct CloudLayer2MapState {
    bool  valid = false;
    float centerX = 0.0f, centerZ = 0.0f;  // cloud frame, km
    float seed = 0.0f;
  } m_cloudLayer2MapState;
  // One cascade of one map, as baked (or being baked).
  struct CloudShadowCascade {
    bool    valid = false;
    bool    stale = false;  // the cloud field changed since the bake
    Vector3 dir { 0.0f, 1.0f, 0.0f };
    float   centerU = 0.0f, halfU = 1.0f, centerV = 0.0f, halfV = 1.0f;
    Vector2 bakeCameraCloudXZ { 0.0f, 0.0f };
    uint32_t slot = 0;
  };
  struct CloudShadowMapState {
    CloudShadowCascade cascade[2][kCloudShadowMapCascades];  // [0] sun map, [1] horizon map
    uint32_t spare[2] = { kCloudShadowMapCascades, kCloudShadowMapCascades };
    float    key[13] {};
    bool     keyValid = false;
    // The cascade being re-baked into its map's spare slot, a few rows per frame.
    bool     jobActive = false;
    uint32_t jobMap = 0, jobCascade = 0, jobRow = 0;
    CloudShadowCascade jobParams;
    // The sun's turn per frame, smoothed, for the re-bake rate (cloudShadowMapMaxLagDegrees).
    Vector3  lastSunDir { 0.0f, 0.0f, 0.0f };
    float    sunDegreesPerFrame = 0.0f;
  } m_cloudShadow;
  CloudShadowCascade cloudShadowCascadeFor(uint32_t map, uint32_t k, const AtmosphereArgs& current) const;
  void dispatchCloudShadowMapRows(PassRecorder& rec, uint32_t map, uint32_t k, const CloudShadowCascade& params,
                                  uint32_t slot, uint32_t row0, uint32_t rows);
  uint32_t m_cloudNvdfPublishCount = 0;
  Texture m_cloudNvdfOccupancy;
  Texture m_cloudNvdfJfa[2];
  Texture m_cloudNvdfSdf[2];
  uint32_t m_cloudNvdfSdfFront = 0;
  float    m_nvdfPublishedNominalCoverage = 0.25f;
  AtmosphereArgs m_nvdfPendingArgs = {};
  bool     m_nvdfNominalCoverageValid = false;

  float    m_missLinearViewZ { 1e9f };

  Texture m_cloudCumulusNoise3D;  // cumulus turbulence volume (kCloudDetailNoise3DSize, with mips)
  const Texture& cloudDensityNoise3D() const;

  CloudProfileState m_cloudProfileState;
  Buffer   m_cloudStatisticsGpu;
  Buffer   m_cloudStatisticsReadback;
  bool     m_cloudStatisticsPending = false;
  uint32_t m_cloudStatisticsFrame = 0u;
  uint32_t m_cloudStatisticsGroups = 0u;
  uint32_t m_cloudStatisticsCounter = 0u;
  CloudProfileState m_cloudStatisticsConfig;
  double   m_cloudSamplesMean = 0.0;
  double   m_cloudSamplesActiveMean = 0.0;
  double   m_cloudSamplesActivePercent = 0.0;
  uint32_t m_cloudSamplesMaximum = 0u;

  Texture  m_cloudRenderRT;
  Texture  m_cloudDepthRT;
  Texture  m_cloudRenderPrevious;
  Texture  m_cloudDepthPrevious;
  Texture  m_cloudStateRT;        // temporal filter state (cloud_render bindings 28 / 29)
  Texture  m_cloudStatePrevious;
  // Sixteenth march fresh samples, one texel per 4x4 cell: colour, depth, flash / layer-2 share.
  Texture  m_cloudFreshColor;
  Texture  m_cloudFreshDepth;
  Texture  m_cloudFreshAux;
  Texture  m_cloudDenoisedRT;  // cloudSpatialDenoise output; composited instead of m_cloudRenderRT
  bool     m_cloudDenoisedThisFrame = false;
  bool     m_cloudTemporalHistoryValid = false;  // survives lighting re-bakes; not camera cuts
  bool     m_cloudCompositeActive = true;
  uint32_t m_cloudLastRenderFrame = 0u;
  bool     m_cloudDomeHistoryValid = false;
  uint32_t m_cloudDomePeriodThisFrame    = 1u;
  // The dome has no history to blend with this frame (camera cut, or never baked).
  bool     m_cloudDomeResetThisFrame     = true;
  uint32_t m_cloudBakeWindowFrames = 0u;
  uint32_t m_cloudBakeInputChanges = 0u;
  uint32_t m_cloudDomeFullFrames = 0u;
  uint32_t m_cloudRenderExtentWidth = 0u;
  uint32_t m_cloudRenderExtentHeight = 0u;

  Texture m_cloudSecondaryLut;  // kCloudSecondaryLutMipLevels mips
  Texture m_cloudPlacementMap;
  Texture m_blueNoise;  // 128x128x64 R8 fallback when FrameInputs::blueNoise is 0
  GpuHandle blueNoise() const { return m_inputs.blueNoise ? m_inputs.blueNoise : m_blueNoise.handle; }

  uint32_t m_cloudRenderFrameIdx   { 0u };
  Vector3  m_cameraWorldPosYUpKm   { 0.0f, 0.0f, 0.0f };
  float    m_groundLevelYUpKm      { 0.0f };
  // Ground level: the field (CPU, world-anchored, survives releaseGpuResources), the probes in
  // flight (a readback ring: each slot remembers the render-to-world offset of its frame), and the
  // level in force (FrameOutputs::groundLevelWorldUnits / groundLevelMeasured).
  struct GroundProbeSlot {
    Buffer  readback;
    Vector3 renderToWorld { 0.0f, 0.0f, 0.0f };
    bool    pending = false;
    bool    stale = false;  // cast before a world change: its hits are dropped when they come back
  };
  static constexpr uint32_t kGroundProbeSlots = 3u;
  GroundField     m_groundField;
  Buffer          m_groundProbeHits;
  GroundProbeSlot m_groundProbeSlots[kGroundProbeSlots];
  uint32_t        m_groundProbeSequence = 0u;
  double          m_groundClockSeconds = 0.0;
  double          m_groundFastUntil = -1.0;
  float           m_groundLevelWorldUnits = 0.0f;  // engine units along its up axis
  bool            m_groundLevelMeasured = false;

  CloudAnchor m_cloudAnchor;
  Vector3 m_cloudHostWorldOrigin { 0.0f, 0.0f, 0.0f };
  Vector3 m_cloudAnchorPrevDirectionYUp { 0.0f, 0.0f, 1.0f };
  Vector3 m_cloudAnchorFirstRawWorldUnits { 0.0f, 0.0f, 0.0f };
  bool    m_cloudAnchorHasFirstSample     { false };
  bool    m_cloudAnchorCutThisFrame        { false };

  float    m_timeOfDayHours             { 12.0f };
  float    m_lastAuthoredTimeOfDayHours { -1.0f };

  // Pre-exposure (radianceAutoScale): this frame's power-of-two scale and last frame's.
  int32_t  m_radianceScaleLog2          { 0 };
  float    m_radianceScale              { 1.0f };
  float    m_prevRadianceScale          { 1.0f };
  double   m_animationSeconds = 0.0;  // sum of FrameInputs::deltaSeconds (star scintillation)
  float    m_lastCoverageMean = -1.0f;
  uint32_t m_coverageSteadyFrames = 0u;  // frames the cloud coverage has held still

  // Night sky geometry (updateCelestial). m_moons[i].illuminance is a fraction of the sun's
  // illuminance here; getAtmosphereArgs multiplies the sun's in.
  celestial::LocalFrame m_celestialFrame {};
  Vector3  m_eclipticPoleLocal          { 0.0f, 1.0f, 0.0f };
  MoonParams m_moons[MAX_MOONS] {};
  Vector3 m_moonUmbra[MAX_MOONS] {};  // per moon: the light the Earth's shadow takes (updateCelestial)
  uint32_t m_moonSkyLightMask           { 0u };
  uint32_t m_cloudKeyLight              { 0u };  // 0 = sun, i + 1 = moons[i]

  // Night sky data (setNightSkyAssets): the payloads after their headers, streamed to the GPU by
  // uploadNightSky a chunk at a time.
  struct NightSkyData {
    std::vector<uint8_t> starCells, starRecords, milkyWay, starlightLow;
    // The moon surface atlas: one 2:1 band per distinct map, each mip filtered within its band.
    std::vector<std::vector<uint8_t>> moonMips;
    int32_t moonLayers[1 + kMaxMoons];  // atlas band per moon (real moon, then game moons), -1 = none
    NightSkyData() { std::fill(std::begin(moonLayers), std::end(moonLayers), -1); }
    uint32_t cellsPerFace = 0, milkyWayWidth = 0, milkyWayHeight = 0;
    uint32_t lowWidth = 0, lowHeight = 0, moonWidth = 0, moonHeight = 0;
    uint64_t generation = 0;          // bumped by every setNightSkyAssets
    uint64_t uploadedGeneration = 0;  // the generation fully on the GPU
    uint64_t uploadingGeneration = 0;
    size_t   cursor = 0;              // bytes streamed of the current generation
  } m_nightSky;
  Buffer  m_nightSkyStarCells, m_nightSkyStarRecords, m_nightSkyMilkyWay;
  Texture m_nightSkyStarlightLow, m_nightSkyMoon;
  bool nightSkyStarsReady() const {
    return m_nightSky.uploadedGeneration != 0 && !m_nightSky.starRecords.empty() && !m_nightSky.milkyWay.empty()
        && !m_nightSky.starlightLow.empty();
  }
  bool nightSkyMoonReady() const { return m_nightSky.uploadedGeneration != 0 && !m_nightSky.moonMips.empty(); }

  Vector3 m_apCameraPosition { 0.0f, 0.0f, 0.0f };
  Vector3 m_apCameraForward { 0.0f, 0.0f, 1.0f };
  Vector3 m_apFrustumPlanes[4] {};

  Vector2  m_cloudAdvectOffset     { 0.0f, 0.0f };
  Vector2  m_cloudAdvectStep       { 0.0f, 0.0f };  // this frame's advection, for reprojection
  Vector3  m_cloudEvolutionOffset  { 0.0f, 0.0f, 0.0f };
  float    m_cloudBoilPhase        { 0.0f };

  Vector3  m_lightningStrikePosKm       { 0.0f, 0.0f, 0.0f };
  float    m_lightningEnvelope          { 0.0f };
  float    m_lightningEnvelopePrevious  { 0.0f };
  float    m_skyRelativeLuminance       { 1.0f };  // updateRadianceScale: the sky relative to daylight's
  float    m_nightGain                  { 1.0f };  // updateRadianceScale: nightGainStops faded through twilight
  int      m_lightningPulsesLeft        { 0 };
  float    m_lightningTimeToPulse       { 0.0f };
  uint32_t m_lightningRngState          { 0x9E3779B9u };
  bool     m_lightningPlacePending      { false };
  bool     m_lightningPlaceAtInput      { false };
  static std::atomic<bool> s_lightningStrikeRequested;

  AtmosphereArgs m_cachedArgs {};
  AtmosphereArgs m_cachedSkyViewKey = {};
  AtmosphereArgs m_cachedTransmittanceMsKey = {};
  AtmosphereArgs m_cachedVoxelGridKey = {};

  float    m_cloudAnchorSpeedEmaKm    = 0.0f;
  float    m_cachedPlacementCellSizeKm = 0.0f;
  float    m_cachedPlacementTileKm     = 0.0f;

  struct CloudNvdfBakeKey {
    float cellSizeKm       = 0.0f;
    float tileKm           = 0.0f;
    float columnFeather    = 0.0f;
    float columnTopShape   = 0.0f;
    float columnTopVar     = 0.0f;
    float columnBaseVar    = 0.0f;
    float nominalCoverage  = 0.0f;
    float thicknessQ       = 0.0f;
    uint32_t bodyModel     = 0;
    float billowStrength   = 0.0f;
    float billowSizeKm     = 0.0f;
    float billowGain       = 0.0f;
    float deckType         = 0.0f;
  };
  CloudNvdfBakeKey m_cachedNvdfKey = {};
  bool     m_nvdfBakeActive = false;
  uint32_t m_nvdfJumpIdx    = 0;

  bool m_initialized = false;
  bool m_lutsNeedRecompute = true;

};

} // namespace numos
