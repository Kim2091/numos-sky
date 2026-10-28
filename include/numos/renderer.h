// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

// NUMOS renderer: the per-frame entry point a host drives (DESIGN.md "Frame flow").
//
//   renderer.setSettings(settings);                 // any time; read at the next beginFrame
//   renderer.beginFrame(gpu, inputs);               // CPU state + pre-scene GPU work (LUTs, bakes)
//   ... host renders its scene, producing linear view Z ...
//   renderer.dispatchPostDepth(gpu, linearViewZ, miss); // screen clouds, near-field haze
//   const FrameOutputs& out = renderer.outputs();   // textures/samplers/args to bind, lights to add
//
// All GPU work is recorded through the IGpu passed in; NUMOS never touches the host API itself.
//
// Frame contract:
// - Call beginFrame and then dispatchPostDepth exactly once each per rendered frame, in that order,
//   with the same IGpu, both while that IGpu is recording the frame's command stream. The cloud
//   pass's temporal history assumes one dispatchPostDepth per beginFrame.
// - outputs() after beginFrame: args, the LUTs and maps, samplers, lights[], radianceScale, night sky.
//   After dispatchPostDepth: also cloudRender / cloudDepth and the aerial perspective's near field
//   (apNearScatter, apFroxelLighting / apFroxelAux) for this frame; between the two calls those hold
//   the previous frame's. The reference stays valid
//   for the Renderer's lifetime; its contents change at the next beginFrame.
// - Every GpuHandle in FrameOutputs belongs to NUMOS: never destroy or write it. Handles may change
//   from one frame to the next (a resolution or setting change re-creates a texture; the old one is
//   passed to IGpu::destroy, which defers the free), so read them again every frame.
// - NUMOS emits IGpu::barrier() between its own dependent passes, and once at the start of
//   dispatchPostDepth (which covers the host's linearViewZ write for a host that honours barrier()).
//   It emits none after its last pass: before the host's own shaders read the outputs, the host
//   needs a barrier (or automatic hazard tracking).
//
// Threading: a Renderer is not thread-safe. Call all its methods from one thread at a time, normally
// the thread recording the IGpu (IGpu calls happen synchronously inside beginFrame,
// dispatchPostDepth and releaseGpuResources, on the calling thread). Separate Renderer instances are
// independent, apart from requestLightningStrike (process-wide) and one-shot log messages. The
// settings functions (settings.h) are pure and may run on any thread.

#include <cstdint>
#include <memory>
#include <string>

#include "numos/gpu.h"
#include "numos/math.h"
#include "numos/settings.h"
#include "numos/lib/atmosphere_args.h"
#include "numos/util/camera.h"

namespace numos {

struct WeatherSnapshot;  // numos/weather.h

// A positional light for the aerial perspective's near field (local-light glow in the air and fog).
// NUMOS reduces every emitter to an area-matched sphere (AerialPerspectiveLight in
// lib/aerial_perspective_light.h).
struct LocalLight {
  enum class Type : uint8_t { Sphere, Rect, Disk, Cylinder };
  Type    type = Type::Sphere;
  Vector3 position;              // world units, render space
  // Physical, not pre-exposed (NUMOS applies FrameOutputs::radianceScale itself); already scaled for
  // volumetrics (Remix: radiance * volumetric scale).
  Vector3 radiance;
  float   radius = 0.0f;         // Sphere: radius. Cylinder: radius.
  Vector2 dimensions;            // Rect: full width/height. Disk: half extents.
  float   axisLength = 0.0f;     // Cylinder
  // Optional spot shaping.
  bool    shaped = false;
  Vector3 shapingDirection;
  float   cosConeAngle = 0.0f;
  float   coneSoftness = 0.0f;
  float   focusExponent = 0.0f;
};

// Copied by beginFrame. The pointers in it (localLights, groundPoints, weather) must stay valid until
// dispatchPostDepth of the same frame returns.
struct FrameInputs {
  uint32_t frameIndex = 0;          // increments by one per rendered frame
  float    deltaSeconds = 0.0f;     // seconds since the previous frame (drives wind, time of day, lightning)
  bool     cameraCut = false;       // history reset this frame (teleport, load)
  uint32_t lastCameraCutFrame = 0;  // frameIndex of the most recent cut
  // The world under the camera may be a different one this frame (a save loaded, an interior or
  // another worldspace entered), even if the camera barely moved: places at the same coordinates can
  // be unrelated. Discards what NUMOS measured about the world (the ground field). NUMOS also assumes
  // this on its own when the cloud anchor jumps (rtx.atmosphere.cloudAnchorCutKm); a camera cut alone
  // does not imply it.
  bool     worldChanged = false;

  // Camera. `camera` is the shader constant block NUMOS binds; makeCamera() builds one from plain
  // matrices for hosts that have no such struct. Positions are in render space (world units).
  // Matrices are column-major (element (row r, column c) at m[c * 4 + r]) and act on column vectors.
  Camera  camera {};
  Vector3 cameraPosition;           // the viewpoint being rendered (free camera included)
  Vector3 cameraPositionGame;       // the game's own camera (equals cameraPosition without a free camera)
  Vector3 cameraForward;            // normalized, render space
  // Must equal camera.viewToWorld / camera.projectionToView (unjittered); used on the CPU.
  float   viewToWorld[16] = {};
  float   projectionToView[16] = {};

  // Conventions. Every length and position in these inputs (and in FrameOutputs, linearViewZ and
  // the host shader API) is in the host's world units; NUMOS converts them with one factor, game
  // units per NUMOS metre = unitsPerMeter (or hostUnitsPerMeter) x rtx.atmosphere.worldScale
  // (FrameOutputs::args.worldUnitsPerKm / 1000). Radiances need no conversion.
  bool    zUp = false;              // engine up axis is Z (else Y)
  float   hostUnitsPerMeter = 0.0f; // fallback when rtx.atmosphere.unitsPerMeter is 0 (Remix: 100 * rtx.sceneScale)
  bool    hasWorldOrigin = false;   // host renders around a moving origin: world = render + worldOrigin
  Vector3 worldOrigin;

  uint32_t renderWidth = 0;         // internal render resolution (cloud screen pass extent)
  uint32_t renderHeight = 0;
  // The resolution the host's upscaler outputs (0: the render resolution, no upscaling). With the render
  // resolution it sets the automatic screen-cloud march rate (rtx.atmosphere.cloudMarchRate = 0).
  uint32_t outputWidth = 0;
  uint32_t outputHeight = 0;

  // Optional host resources.
  GpuHandle opaqueTlas = 0;         // scene TLAS for the aerial perspective's sun, sky and light visibility and the
                                    // ground probes (groundLevelAuto); 0 = unshadowed, no probes
  GpuHandle blueNoise = 0;          // Texture2DArray R8, 128x128x64 blue noise (Remix's asset); 0 = NUMOS's own copy
  const LocalLight* localLights = nullptr;
  uint32_t localLightCount = 0;
  // Optional points on the ground (render space), for rtx.atmosphere.groundLevelAuto: a host that
  // knows its terrain (a heightfield query under and around the player) passes some each frame. They
  // join NUMOS's own probes against opaqueTlas, or stand in for them without a TLAS.
  const Vector3* groundPoints = nullptr;
  uint32_t groundPointCount = 0;
  bool     hostSkyOverride = false; // a host sky (dome light) replaces the atmosphere: aerial perspective off
  float    volumetricsHandoffWorldUnits = 0.0f; // host froxel volumetrics range; aerial perspective starts here (0 = none)
  // The host writes FrameOutputs::apLightInjection each frame (its own local-light sampling, e.g.
  // Remix's volume ReSTIR); NUMOS adds it to the near field the next frame. See numos_host.slangh.
  bool     hostLocalLightInjection = false;
  bool     debugViewActive = false; // keep the cloud screen pass running for host debug views

  const WeatherSnapshot* weather = nullptr; // blended weather, or null to use Settings alone

  // A lightning strike this frame (a scripted strike, or a test), on top of the automatic ones
  // (lightningStrikesPerMinute); ignored while lightningEnable is off. At lightningStrikePosition
  // (render space) when lightningStrikeAtPosition, else placed as an automatic strike is.
  bool     lightningStrike = false;
  bool     lightningStrikeAtPosition = false;
  Vector3  lightningStrikePosition;
};

// A directional or point light NUMOS wants in the host's scene (sun, moons, lightning).
struct EmittedLight {
  enum class Type : uint8_t { Distant, Sphere };
  Type    type = Type::Distant;
  bool    active = false;           // false: remove/hide the host light for this slot
  Vector3 direction;                // Distant: propagation direction, render space
  float   halfAngle = 0.0f;         // Distant: angular radius (radians)
  Vector3 position;                 // Sphere: render space
  float   radius = 0.0f;            // Sphere
  Vector3 radiance;                 // pre-exposed (x FrameOutputs::radianceScale), Remix light convention
  bool    cloudShadowed = false;    // host shaders apply the cloud shadow (sampleCloudGroundShadowFromMap) to this light
};

struct FrameOutputs {
  AtmosphereArgs args {};           // bind as its own constant buffer for host shaders
  // Textures (0 when not produced this frame).
  GpuHandle transmittanceLut = 0, multiscatteringLut = 0, skyViewLut = 0;
  GpuHandle cloudSkyTransmittanceLut = 0;
  GpuHandle cloudShadowMap = 0;  // the cloud sun-space shadow map (Texture3D, RG16F); see numos_host.slangh
  GpuHandle cloudSecondaryLut = 0;  // with mips
  GpuHandle cloudRender = 0, cloudDepth = 0;
  // Aerial perspective (docs/aerial-perspective-v2.md): near-field in-scatter, far-field LUT, the
  // near field's radiance cache (for numosAirSegment) and the host's local-light injection target.
  GpuHandle apNearScatter = 0, apFarScatter = 0, apFarTransmittance = 0;
  GpuHandle apFroxelLighting = 0, apFroxelAux = 0, apLightInjection = 0;
  // Structured buffers for the composite's per-pixel local-light glow (AerialPerspectiveLight[],
  // and uint cluster lists whose per-tile part apLightTileOffset addresses). Always valid while the
  // aerial perspective runs; args.apLocalLightCount gates their use.
  GpuHandle apLights = 0, apLightClusters = 0;
  GpuHandle cloudNvdfSdf = 0, cloudDetailNoise3D = 0; // for debug views
  // Samplers.
  GpuHandle skyViewSampler = 0;     // linear, clamp
  GpuHandle cloudNoiseSampler = 0;  // linear, repeat, mips
  GpuHandle linearClampSampler = 0;
  bool cloudCompositeActive = false;
  // Pre-exposure (rtx.atmosphere.radianceAutoScale): every radiance NUMOS produces, in its textures,
  // through args in host shaders, and in lights[], is physical x radianceScale (a power of two, 1 in
  // daylight). Divide by it, or fold it into exposure; multiply the host's own lights by it.
  float radianceScale = 1.0f;
  // Night sky data (Renderer::setNightSkyAssets), for the host's sky shaders: the NightSky* globals
  // of numos_host.slangh. 0 until the assets are set and fully uploaded (args.nightSkyFlags bit 0).
  GpuHandle nightSkyStarCells = 0, nightSkyStars = 0, nightSkyMilkyWay = 0;  // structured buffers
  GpuHandle nightSkyStarlightLow = 0, nightSkyMoon = 0;                       // textures
  // Lights: [0] sun, [1..4] moons, [5] lightning. The lightning light is a sphere at the strike,
  // active only while a flash lasts (rtx.atmosphere.lightningSceneLight), carrying the same luminous
  // intensity as the flash NUMOS draws in the clouds and the haze; not cloudShadowed (it sits in the
  // cloud, which diffuses its light rather than blocking it).
  static constexpr uint32_t kSunLight = 0, kFirstMoonLight = 1, kLightningLight = 1 + kMaxMoons;
  EmittedLight lights[2 + kMaxMoons];
  // The ground level in force this frame, in the engine's own units along its up axis (the value
  // rtx.atmosphere.groundLevelWorldUnits would need to give the same result), and whether it was
  // measured from the scene (rtx.atmosphere.groundLevelAuto with data) or is the configured one.
  float groundLevelWorldUnits = 0.0f;
  // Photometry: NUMOS's illuminance unit in lux (and its radiance unit in cd/m^2): the real sun's
  // 1.27e5 lux over sunIlluminance x sunIntensity's luminance, the calibration the night sky is built
  // on. Physical: divide a host's pre-exposure out first. Hosts use it for photometers and to state
  // their own lights' calibration in physical units.
  float luxPerUnit = 1.0f;
  // rtx.atmosphere.nightGainStops as the factor in force this frame (1 by day, faded in through twilight).
  float nightGain = 1.0f;
  bool  groundLevelMeasured = false;
  // The screen clouds' march stride this frame: 1 full rate, 2 quarter, 4 sixteenth (cloudMarchRate, or
  // its Auto choice).
  uint32_t cloudMarchStride = 1;
};

// The night sky's data (assets/nightsky/*.bin, built by tools/build_nightsky_assets.py). The host
// loads the files and passes their bytes; NUMOS copies them and streams them to the GPU over the next
// few frames (a few MB per frame). Missing pieces switch off what needs them: no stars without
// `stars` and `milkyWay` and `starlightLow`; the moon falls back to a uniform albedo without `moon`.
struct NightSkyAssets {
  const void* stars = nullptr;        size_t starsSize = 0;
  const void* milkyWay = nullptr;     size_t milkyWaySize = 0;
  const void* starlightLow = nullptr; size_t starlightLowSize = 0;
  const void* moon = nullptr;         size_t moonSize = 0;
  // Optional surface maps for the game moons (Settings::moons[i]), in the moon.bin format; a game
  // moon without one wears `moon`. tools/build_nightsky_assets.py --custom-moon makes them.
  const void* gameMoons[kMaxMoons] = {}; size_t gameMoonSizes[kMaxMoons] = {};
};

// What the renderer's own clocks have accumulated: with the Settings and the weather it fixes the
// exact sky (where every cloud is and how it has evolved, the moon and stars), so a seed (seed.h)
// can carry it. Opaque units: read it from one Renderer and give it to another.
struct SimulationState {
  float  cloudAdvectOffset[2] = {};    // the wind's accumulated drift of the cloud field
  float  cloudEvolutionOffset[2] = {}; // the clouds' accumulated evolution
  float  timeOfDayHours = 12.0f;       // the running time of day (timeCycleEnable)
  double animationSeconds = 0.0;       // the animation clock (star scintillation)
};

// The host creates one compute pipeline per pass from the built SPIR-V (passInfo()/loadPassSpirv() in
// shaders.h); NUMOS only references passes by index, through DispatchDesc::pass.
class Renderer {
public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  void setSettings(const Settings& settings);
  // Returns false (and logs why) for data that does not parse; what parsed is still used.
  bool setNightSkyAssets(const NightSkyAssets& assets);
  const Settings& settings() const;

  void beginFrame(IGpu& gpu, const FrameInputs& inputs);
  // `linearViewZ`: Texture2D<float> at renderWidth x renderHeight (FrameInputs), the scene's distance
  // along the camera forward axis in world units (sign ignored); texels >= `missLinearViewZ` are sky.
  // The screen clouds read it, and the aerial perspective's near field keeps its samples in front of
  // the surfaces it shows (0: no depth, the near field is then sampled through walls).
  void dispatchPostDepth(IGpu& gpu, GpuHandle linearViewZ, float missLinearViewZ);
  const FrameOutputs& outputs() const;

  // Destroys, through IGpu::destroy, every GPU resource NUMOS created (call before destroying the
  // IGpu, or on a device reset). Host-owned handles (FrameInputs, linearViewZ) are untouched. The
  // Renderer stays usable: the next beginFrame re-creates everything and re-uploads the night sky.
  // Settings and night sky data are kept; all other frame state (cloud motion, temporal histories,
  // time-of-day progress, lightning) restarts as for a new Renderer.
  void releaseGpuResources(IGpu& gpu);

  // Manual lightning strike (UI "Test Strike"); thread-safe. Process-wide: the next frame of any
  // Renderer consumes it (and drops it while lightningEnable is off).
  static void requestLightningStrike();

  // The renderer's own clocks (SimulationState). A set takes effect from the next beginFrame; pass a
  // camera cut that frame, since the temporal histories do not follow the jump.
  SimulationState simulationState() const;
  void setSimulationState(const SimulationState& state);

  struct Impl;
private:
  std::unique_ptr<Impl> m_impl;
};

// Builds the Camera constant block from plain matrices (column-major, render space).
// `prev*` describe the previous frame (pass the current ones on the first frame).
Camera makeCamera(const float worldToView[16], const float viewToProjection[16],
                  const float prevWorldToView[16], const float prevViewToProjection[16],
                  float jitterX, float jitterY, uint32_t width, uint32_t height,
                  float nearPlane, bool rightHanded);

} // namespace numos
