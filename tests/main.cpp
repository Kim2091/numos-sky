// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// NUMOS unit tests. No framework: each CHECK prints on failure and the exit code counts failures.
// Optional argument: path to an rtx.conf to load (reports how many NUMOS keys it set).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "numos/gpu.h"
#include "numos/lib/aerial_perspective_light.h"
#include "numos/lib/ground_probe.h"
#include "numos/lib/atmosphere_args.h"
#include "numos/log.h"
#include "numos/renderer.h"
#include "numos/seed.h"
#include "numos/settings.h"
#include "numos/shaders.h"
#include "numos/weather.h"

#include "../src/ground_field.h"
#include "../src/night_gain.h"

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

using namespace numos;

static void testInfos() {
  const auto& infos = settingInfos();
  CHECK(infos.size() == 197 + 7 * kMaxMoons);
  CHECK(findSetting("rtx.atmosphere.sunSize") != nullptr);
  CHECK(findSetting("rtx.atmosphere.moon3.rotation") != nullptr);
  CHECK(findSetting("rtx.atmosphere.nope") == nullptr);
  // Keys are unique.
  for (size_t i = 0; i < infos.size(); ++i) {
    CHECK(findSetting(infos[i].key) == &infos[i]);
  }
}

static void testRoundTrip() {
  Settings s;
  CHECK(std::fabs(s.sunSize - 0.545f) < 1e-6f);
  CHECK(setSetting(s, "rtx.atmosphere.sunSize", " 1.25 "));
  CHECK(s.sunSize == 1.25f);
  CHECK(setSetting(s, "rtx.atmosphere.aerialPerspective", "False"));
  CHECK(!s.aerialPerspective);
  CHECK(setSetting(s, "rtx.atmosphere.rayleighScattering", "0.1, 0.2, 0.3"));
  CHECK(s.rayleighScattering.y == 0.2f);
  CHECK(setSetting(s, "rtx.atmosphere.moon2.enabled", "True"));
  CHECK(s.moons[2].enabled && !s.moons[1].enabled);
  CHECK(s.moons[0].phase < 0.0f);
  CHECK(setSetting(s, "rtx.atmosphere.moon0.phase", "0.25"));
  CHECK(s.moons[0].phase == 0.25f);
  CHECK(!setSetting(s, "rtx.atmosphere.sunSize", "abc"));
  // Clamped to minValue (worldScale has min 0.001).
  CHECK(setSetting(s, "rtx.atmosphere.worldScale", "0.0001"));
  CHECK(std::fabs(s.worldScale - 0.001f) < 1e-9f);

  // writeConfig -> applyConfig reproduces the settings.
  Settings t;
  const std::string conf = writeConfig(s);
  applyConfig(t, conf);
  for (const SettingInfo& info : settingInfos()) {
    if (!(info.flags & NUMOS_SETTING_NO_SAVE)) {
      CHECK(formatSetting(s, info) == formatSetting(t, info));
    }
  }
}

static void testParsing() {
  Settings s;
  // Floats: '+' and 'f' accepted; NaN / inf rejected and the value kept (NaN would slip past the clamp).
  CHECK(setSetting(s, "rtx.atmosphere.sunSize", "+1.5f"));
  CHECK(s.sunSize == 1.5f);
  CHECK(!setSetting(s, "rtx.atmosphere.sunSize", "nan"));
  CHECK(!setSetting(s, "rtx.atmosphere.sunSize", "inf"));
  CHECK(!setSetting(s, "rtx.atmosphere.sunSize", "+-1"));
  CHECK(s.sunSize == 1.5f);
  CHECK(!setSetting(s, "rtx.atmosphere.rayleighScattering", "0.1, nan, 0.3"));
  CHECK(!setSetting(s, "rtx.atmosphere.rayleighScattering", "0.1, 0.2"));
  CHECK(!setSetting(s, "rtx.atmosphere.rayleighScattering", "0.1, 0.2, 0.3, 0.4"));
  // Integers over 2^24 in an unbounded range survive exactly: they must not round through float,
  // where INT32_MAX / UINT32_MAX would round out of range.
  CHECK(setSetting(s, "rtx.atmosphere.cloudProfilingMode", "2147483647"));
  CHECK(s.cloudProfilingMode == 2147483647);
  CHECK(setSetting(s, "rtx.atmosphere.cloudProfilingMode", "-2147483648"));
  CHECK(s.cloudProfilingMode == INT32_MIN);
  CHECK(setSetting(s, "rtx.atmosphere.cloudViewSamples", "4294967295"));
  CHECK(s.cloudViewSamples == 4294967295u);
  CHECK(setSetting(s, "rtx.atmosphere.cloudViewSamples", "+16777217"));
  CHECK(s.cloudViewSamples == 16777217u);
  CHECK(!setSetting(s, "rtx.atmosphere.cloudViewSamples", "-1"));
  CHECK(!setSetting(s, "rtx.atmosphere.cloudViewSamples", "2.5"));
  // Clamped to the setting's integer range (dayOfYear 1..365).
  CHECK(setSetting(s, "rtx.atmosphere.dayOfYear", "9999"));
  CHECK(s.dayOfYear == 365);
  // Bools in any case.
  CHECK(setSetting(s, "rtx.atmosphere.aerialPerspective", "FALSE"));
  CHECK(!s.aerialPerspective);
  CHECK(setSetting(s, "rtx.atmosphere.aerialPerspective", "true"));
  CHECK(s.aerialPerspective);
  CHECK(!setSetting(s, "rtx.atmosphere.aerialPerspective", "yes"));

  // Config text: BOM, CRLF, comments, blank lines, a duplicate key (last wins), a bad value (skipped).
  Settings c;
  ExplicitSettings e;
  const size_t n = applyConfig(c, "\xEF\xBB\xBFrtx.atmosphere.sunSize = 0.7\r\n"
                                  "# rtx.atmosphere.dayOfYear = 10\r\n"
                                  "\r\n"
                                  "  rtx.atmosphere.dayOfYear=20   # comment\r\n"
                                  "rtx.atmosphere.dayOfYear = 30\r\n"
                                  "rtx.atmosphere.calendarYear = abc\r\n"
                                  "rtx.someOtherOption = 1", &e);
  CHECK(n == 3);
  CHECK(c.sunSize == 0.7f);
  CHECK(c.dayOfYear == 30);
  CHECK(c.calendarYear == Settings{}.calendarYear);
  CHECK(e.size() == settingInfos().size());
  CHECK(e[size_t(findSetting("rtx.atmosphere.sunSize") - settingInfos().data())]);
  CHECK(!e[size_t(findSetting("rtx.atmosphere.calendarYear") - settingInfos().data())]);

  // Floats are written in their shortest exact form.
  Settings f;
  CHECK(setSetting(f, "rtx.atmosphere.sunSize", "0.1"));
  CHECK(formatSetting(f, *findSetting("rtx.atmosphere.sunSize")) == "0.1");
}

static void testMigration() {
  Settings s;
  ExplicitSettings e;
  applyConfig(s, "rtx.atmosphere.cloudAltitude = 2.0\n"
                 "rtx.atmosphere.cloudScale = 0.704   # legacy units per cm\n"
                 "rtx.sceneScale = 0.1\n", &e);
  migrateDeprecated(s, e, 0.1f);
  CHECK(std::fabs(s.cloudBaseHeightMeters - 2000.0f) < 1e-3f);
  CHECK(std::fabs(s.unitsPerMeter - 70.4f) < 1e-3f);
  CHECK(s.cloudScale == Settings{}.cloudScale);

  // An explicitly set replacement wins over the deprecated key.
  Settings t;
  ExplicitSettings et;
  applyConfig(t, "rtx.atmosphere.cloudAltitude = 2.0\nrtx.atmosphere.cloudBaseHeightMeters = 900\n", &et);
  migrateDeprecated(t, et, 0.0f);
  CHECK(t.cloudBaseHeightMeters == 900.0f);

  // A game moon's angularRadius (which holds a diameter) becomes angularDiameter, once.
  Settings mr;
  ExplicitSettings emr;
  applyConfig(mr, "rtx.atmosphere.moon1.angularRadius = 2.5\n"
                  "rtx.atmosphere.moon2.angularRadius = 3\nrtx.atmosphere.moon2.angularDiameter = 1.5\n", &emr);
  migrateDeprecated(mr, emr, 0.0f);
  CHECK(mr.moons[1].angularDiameter == 2.5f);
  CHECK(mr.moons[2].angularDiameter == 1.5f);
  CHECK(mr.moons[1].angularRadius == Settings{}.moons[1].angularRadius);
  CHECK(writeConfig(mr).find("angularRadius") == std::string::npos);

  // cloudLayer2Color becomes layer 2's albedo, as its ratio to cloudColor (at most 1).
  Settings l2;
  ExplicitSettings el2;
  applyConfig(l2, "rtx.atmosphere.cloudColor = 1, 1, 0.5\n"
                  "rtx.atmosphere.cloudLayer2Color = 0.8, 0.9, 0.62\n"
                  "rtx.atmosphere.cloudLayer2StepMax = 16\n", &el2);
  migrateDeprecated(l2, el2, 0.0f);
  CHECK(std::fabs(l2.cloudLayer2Albedo.x - 0.8f) < 1e-6f && std::fabs(l2.cloudLayer2Albedo.y - 0.9f) < 1e-6f
        && l2.cloudLayer2Albedo.z == 1.0f);
  CHECK(l2.cloudLayer2Color.x == Settings{}.cloudLayer2Color.x);
  CHECK(writeConfig(l2).find("cloudLayer2Step") == std::string::npos);

  // The deprecated local-light glow scale follows the clouds' scale (reported, not migrated: it is
  // the near field's own figure); deprecated aerial perspective keys still parse.
  Settings u;
  ExplicitSettings eu;
  applyConfig(u, "rtx.atmosphere.aerialPerspectiveLocalLightUnitsPerMeter = 70\n"
                 "rtx.atmosphere.aerialPerspectiveLutResolution = 64\n", &eu);
  migrateDeprecated(u, eu, 0.0f);
  CHECK(u.worldScale == 1.0f);
  CHECK(u.aerialPerspectiveFroxelUnitsPerMeter == Settings{}.aerialPerspectiveFroxelUnitsPerMeter);
  CHECK(u.aerialPerspectiveLocalLightUnitsPerMeter == Settings{}.aerialPerspectiveLocalLightUnitsPerMeter);
  // Both deprecated scales: cloudScale sets unitsPerMeter (the datums are converted with it), whatever
  // the line order; the aerial perspective's value is dropped.
  Settings u2;
  ExplicitSettings eu2;
  applyConfig(u2, "rtx.atmosphere.cloudScale = 0.5\nrtx.atmosphere.aerialPerspectiveScale = 0.25\n", &eu2);
  migrateDeprecated(u2, eu2, 0.0f);
  CHECK(std::fabs(u2.unitsPerMeter - 50.0f) < 1e-4f);
  CHECK(u2.aerialPerspectiveScale == Settings{}.aerialPerspectiveScale);
  // The aerial perspective's alone still migrates.
  Settings v2;
  ExplicitSettings ev2;
  applyConfig(v2, "rtx.atmosphere.aerialPerspectiveScale = 0.25\n", &ev2);
  migrateDeprecated(v2, ev2, 0.0f);
  CHECK(std::fabs(v2.unitsPerMeter - 25.0f) < 1e-4f);

  // One world scale. The clouds' game units per NUMOS metre are kept exactly; unitsPerMeter stays.
  {
    // Fallout: New Vegas: the true measurement with a 7.04x cloud compression.
    Settings w;
    ExplicitSettings ew;
    applyConfig(w, "rtx.atmosphere.unitsPerMeter = 70.4\nrtx.atmosphere.cloudWorldCompression = 7.04\n", &ew);
    migrateDeprecated(w, ew, 0.0f);
    CHECK(w.unitsPerMeter == 70.4f);
    CHECK(std::fabs(w.worldScale - 1.0f / 7.04f) < 1e-6f);
    CHECK(w.cloudWorldCompression == Settings{}.cloudWorldCompression);
    const std::string saved = writeConfig(w);
    CHECK(saved.find("rtx.atmosphere.worldScale") != std::string::npos);
    CHECK(saved.find("cloudWorldCompression") == std::string::npos);
  }
  {
    // A short shared figure, the clouds' own 100, the near field's own 70.
    Settings w;
    ExplicitSettings ew;
    applyConfig(w, "rtx.atmosphere.unitsPerMeter = 14.29\nrtx.atmosphere.cloudUnitsPerMeter = 100\n"
                   "rtx.atmosphere.aerialPerspectiveFroxelUnitsPerMeter = 70\n", &ew);
    migrateDeprecated(w, ew, 0.0f);
    CHECK(w.unitsPerMeter == 14.29f);
    CHECK(std::fabs(w.unitsPerMeter * w.worldScale - 100.0f) < 1e-3f);
    CHECK(w.cloudUnitsPerMeter == Settings{}.cloudUnitsPerMeter);
    CHECK(w.aerialPerspectiveFroxelUnitsPerMeter == Settings{}.aerialPerspectiveFroxelUnitsPerMeter);
  }
  {
    // No measurement anywhere: the clouds' own becomes the shared one.
    Settings w;
    ExplicitSettings ew;
    applyConfig(w, "rtx.atmosphere.cloudUnitsPerMeter = 70\nrtx.atmosphere.cloudWorldCompression = 2\n", &ew);
    migrateDeprecated(w, ew, 0.0f);
    CHECK(w.unitsPerMeter == 70.0f);
    CHECK(std::fabs(w.worldScale - 0.5f) < 1e-6f);
  }
  {
    // The measurement inherited from the host (rtx.sceneScale 0.1 = 10 units per metre).
    Settings w;
    ExplicitSettings ew;
    applyConfig(w, "rtx.atmosphere.cloudUnitsPerMeter = 70\n", &ew);
    migrateDeprecated(w, ew, 0.1f);
    CHECK(w.unitsPerMeter == 0.0f);
    CHECK(std::fabs(w.worldScale - 7.0f) < 1e-4f);
  }
  {
    // An explicit worldScale wins; the deprecated keys are only reset.
    Settings w;
    ExplicitSettings ew;
    applyConfig(w, "rtx.atmosphere.worldScale = 0.25\nrtx.atmosphere.cloudWorldCompression = 3\n"
                   "rtx.atmosphere.aerialPerspectiveWorldCompression = 2\n", &ew);
    migrateDeprecated(w, ew, 0.0f);
    CHECK(w.worldScale == 0.25f);
    CHECK(w.cloudWorldCompression == Settings{}.cloudWorldCompression);
    CHECK(w.aerialPerspectiveWorldCompression == Settings{}.aerialPerspectiveWorldCompression);
  }
  {
    // An explicit worldScale equal to its default (1) is a choice too: "set" means present, not
    // different, so 1.0 must not be overridden by the deprecated keys.
    Settings w;
    ExplicitSettings ew;
    applyConfig(w, "rtx.atmosphere.unitsPerMeter = 14.29\nrtx.atmosphere.cloudUnitsPerMeter = 100\n"
                   "rtx.atmosphere.worldScale = 1\n", &ew);
    migrateDeprecated(w, ew, 0.0f);
    CHECK(w.worldScale == 1.0f);
    CHECK(w.cloudUnitsPerMeter == Settings{}.cloudUnitsPerMeter);
  }
  {
    // Migrate once, write back, drop the deprecated keys: the saved result is a fixed point, and the
    // deprecated keys are exactly the ones isDeprecated() names.
    Settings w;
    ExplicitSettings ew;
    const char* legacy = "rtx.atmosphere.unitsPerMeter = 14.29\nrtx.atmosphere.cloudUnitsPerMeter = 100\n"
                         "rtx.atmosphere.cloudAltitude = 2\nrtx.atmosphere.seaLevelWorldKm = 0.5\n";
    applyConfig(w, legacy, &ew);
    for (size_t i = 0; i < settingInfos().size(); ++i) {
      if (ew[i]) {
        const std::string& key = settingInfos()[i].key;
        CHECK(isDeprecated(settingInfos()[i]) == (key != "rtx.atmosphere.unitsPerMeter"));
      }
    }
    migrateDeprecated(w, ew, 0.0f);
    const std::string saved = writeConfig(w);
    CHECK(saved.find("cloudUnitsPerMeter") == std::string::npos && saved.find("cloudAltitude") == std::string::npos);
    Settings again;
    ExplicitSettings eAgain;
    applyConfig(again, saved, &eAgain);
    migrateDeprecated(again, eAgain, 0.0f);
    for (const SettingInfo& info : settingInfos()) {
      CHECK(formatSetting(again, info) == formatSetting(w, info));
    }
    // And the migrated worldScale, saved, is then an explicit choice like any other.
    CHECK(std::fabs(again.unitsPerMeter * again.worldScale - 100.0f) < 1e-3f);
  }
}

static void testPasses() {
  CHECK(passCount() == 26);
  const PassInfo& p = passInfo(PassId::transmittance_lut);
  CHECK(std::string(p.name) == "transmittance_lut");
  CHECK(p.localSize[0] == 16 && p.localSize[1] == 16);
  CHECK(passInfo(PassId::cloud_nvdf_jfa).pushConstantSize == 16);
#ifdef NUMOS_TEST_SPIRV_DIR
  for (uint32_t i = 0; i < passCount(); ++i) {
    CHECK(!loadPassSpirv(PassId(i), NUMOS_TEST_SPIRV_DIR).empty());
  }
#endif
}

// An IGpu that records nothing and checks resource lifetime: no double or unknown destroy, no
// destroyed handle bound or written, nothing left alive after releaseGpuResources.
class LifetimeGpu final : public IGpu {
public:
  std::unordered_set<GpuHandle> live;
  size_t created = 0;
  size_t dispatches = 0;
  static constexpr GpuHandle kRing = 1;  // the host's constant ring, not NUMOS's to destroy

  bool known(GpuHandle h) const { return h == 0 || h == kRing || live.count(h) != 0; }
  GpuHandle make() { ++created; live.insert(m_next); return m_next++; }

  GpuHandle createTexture(const TextureDesc&) override { return make(); }
  GpuHandle createBuffer(const BufferDesc&) override { return make(); }
  GpuHandle createSampler(const SamplerDesc&) override { return make(); }
  void destroy(GpuHandle h) override { CHECK(h != 0 && live.erase(h) == 1); }
  ConstantsRegion uploadConstants(const void*, size_t size) override { return { kRing, 0, size }; }
  void updateBuffer(GpuHandle b, size_t, size_t, const void*) override { CHECK(b != 0 && known(b)); }
  void copyBuffer(GpuHandle d, size_t, GpuHandle s, size_t, size_t) override { CHECK(known(d) && known(s)); }
  void copyTexture(GpuHandle d, GpuHandle s) override { CHECK(known(d) && known(s)); }
  void clearTexture(GpuHandle t, const float*) override { CHECK(t != 0 && known(t)); }
  void uploadTexture(GpuHandle t, uint32_t, const void*, size_t) override { CHECK(t != 0 && known(t)); }
  void dispatch(const DispatchDesc& d) override {
    ++dispatches;
    CHECK(d.pass < passCount() && d.bindingCount == passInfo(d.pass).bindingCount);
    for (uint32_t i = 0; i < d.bindingCount; ++i) {
      CHECK(d.bindings[i].slot == passInfo(d.pass).bindings[i].slot);
      if (d.bindings[i].kind != BindingKind::AccelerationStructure) {
        CHECK(known(d.bindings[i].resource) && known(d.bindings[i].sampler));
      }
    }
  }
  void barrier() override {}
  bool readBuffer(GpuHandle b, void*, size_t) override { CHECK(known(b)); return false; }

private:
  GpuHandle m_next = 2;
};

static std::vector<char> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void testGpuLifetime() {
  LifetimeGpu gpu;
  GpuHandle depth = gpu.make();  // host-owned linear view Z
  const GpuHandle hostOwned = depth;
  Renderer renderer;
  renderer.setSettings(Settings{});

#ifdef NUMOS_TEST_SPIRV_DIR
  const std::string assets = std::string(NUMOS_TEST_SPIRV_DIR) + "/../../assets/nightsky/";
  const std::vector<char> stars = readFile(assets + "stars.bin"), milkyWay = readFile(assets + "milkyway.bin");
  const std::vector<char> low = readFile(assets + "starlight_low.bin"), moon = readFile(assets + "moon.bin");
  const bool haveNightSky = !stars.empty() && !milkyWay.empty() && !low.empty() && !moon.empty();
  if (haveNightSky) {
    NightSkyAssets a;
    a.stars = stars.data(); a.starsSize = stars.size();
    a.milkyWay = milkyWay.data(); a.milkyWaySize = milkyWay.size();
    a.starlightLow = low.data(); a.starlightLowSize = low.size();
    a.moon = moon.data(); a.moonSize = moon.size();
    CHECK(renderer.setNightSkyAssets(a));
  }
#else
  const bool haveNightSky = false;
#endif

  // Column-major identity view and a plain perspective projection.
  const float view[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 2, 0, 1 };
  const float proj[16] = { 1, 0, 0, 0,  0, 1.777f, 0, 0,  0, 0, 1, 1,  0, 0, -0.1f, 0 };
  uint32_t frame = 0;
  auto run = [&](uint32_t frames, uint32_t width) {
    for (uint32_t i = 0; i < frames; ++i) {
      ++frame;
      FrameInputs in;
      in.frameIndex = frame;
      in.deltaSeconds = 1.0f / 60.0f;
      in.cameraCut = frame == 1;
      in.lastCameraCutFrame = 1;
      in.camera = makeCamera(view, proj, view, proj, 0.0f, 0.0f, width, width / 2, 0.1f, false);
      in.cameraPosition = Vector3(0.0f, 2.0f, 0.0f);
      in.cameraPositionGame = in.cameraPosition;
      in.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
      std::memcpy(in.viewToWorld, &in.camera.viewToWorld, sizeof(in.viewToWorld));
      std::memcpy(in.projectionToView, &in.camera.projectionToView, sizeof(in.projectionToView));
      in.hostUnitsPerMeter = 1.0f;
      in.renderWidth = width;
      in.renderHeight = width / 2;
      renderer.beginFrame(gpu, in);
      renderer.dispatchPostDepth(gpu, depth, 1e9f);
    }
  };
  auto nightSkyUp = [&] {
    const FrameOutputs& o = renderer.outputs();
    return o.nightSkyStars != 0 && gpu.live.count(o.nightSkyStars) && o.nightSkyMoon != 0 && gpu.live.count(o.nightSkyMoon);
  };

  run(8, 64);
  CHECK(gpu.dispatches > 0);
  // Resource re-creation paths: render extent, aerial perspective LUT size.
  run(2, 96);
  Settings s;
  s.aerialPerspectiveLutResolution = 64;
  renderer.setSettings(s);
  run(2, 96);
  if (haveNightSky) {
    run(16, 96);
    CHECK(nightSkyUp());
  }

  renderer.releaseGpuResources(gpu);
  CHECK(gpu.live.size() == 1 && gpu.live.count(hostOwned) == 1);

  // Re-initialisation after a release (device reset): everything comes back, the night sky included.
  run(24, 64);
  CHECK(renderer.outputs().transmittanceLut != 0 && gpu.live.count(renderer.outputs().transmittanceLut));
  if (haveNightSky) {
    CHECK(nightSkyUp());
  }
  renderer.releaseGpuResources(gpu);
  CHECK(gpu.live.size() == 1);
}

// World Scale is one similarity transform at the host boundary: a scene shrunk by k in game units,
// rendered with worldScale and hazeWorldScale k, must give NUMOS exactly the same world. Every NUMOS-space quantity in
// AtmosphereArgs comes out the same, every game-space one scaled by k, and what NUMOS hands back
// (the lightning light, the aerial perspective's local lights) is the full-scale answer shrunk.
class CaptureGpu final : public IGpu {
public:
  std::unordered_map<GpuHandle, std::vector<uint8_t>> buffers;
  GpuHandle createTexture(const TextureDesc&) override { return m_next++; }
  GpuHandle createBuffer(const BufferDesc&) override { return m_next++; }
  GpuHandle createSampler(const SamplerDesc&) override { return m_next++; }
  void destroy(GpuHandle) override {}
  ConstantsRegion uploadConstants(const void*, size_t size) override { return { 1, 0, size }; }
  void updateBuffer(GpuHandle b, size_t offset, size_t size, const void* data) override {
    std::vector<uint8_t>& bytes = buffers[b];
    bytes.resize(std::max(bytes.size(), offset + size));
    std::memcpy(bytes.data() + offset, data, size);
  }
  void copyBuffer(GpuHandle, size_t, GpuHandle, size_t, size_t) override {}
  void copyTexture(GpuHandle, GpuHandle) override {}
  void clearTexture(GpuHandle, const float*) override {}
  void uploadTexture(GpuHandle, uint32_t, const void*, size_t) override {}
  void dispatch(const DispatchDesc&) override {}
  void barrier() override {}
  bool readBuffer(GpuHandle, void*, size_t) override { return false; }

private:
  GpuHandle m_next = 2;
};

struct ScaledFrame {
  AtmosphereArgs args;
  EmittedLight lightning;
  Vector3 cameraPosition;
  std::vector<AerialPerspectiveLight> apLights;
};

static ScaledFrame renderScaledScene(float k) {
  Settings s;
  s.unitsPerMeter = 10.0f;
  s.worldScale = k;
  s.hazeWorldScale = k;
  s.groundLevelWorldUnits = 50.0f * k;           // game units: scale with the scene
  s.cloudVerticalOffsetWorldUnits = -200.0f * k;
  s.fogVisibilityMeters = 800.0f;                // NUMOS metres: do not
  s.fogNearFadeMeters = 10.0f;
  s.lightningEnable = true;
  s.lightningSceneLight = true;
  s.aerialPerspectiveLocalLights = true;
  Renderer renderer;
  renderer.setSettings(s);
  CaptureGpu gpu;

  const Vector3 camera(1000.0f * k, 300.0f * k, -400.0f * k);
  const float nearPlane = 0.1f * k;
  const float view[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -camera.x, -camera.y, -camera.z, 1 };
  const float proj[16] = { 1, 0, 0, 0,  0, 1.777f, 0, 0,  0, 0, 1, 1,  0, 0, -nearPlane, 0 };
  // Two lamps ahead of the camera, a sphere and a rect.
  LocalLight lamps[2];
  lamps[0].type = LocalLight::Type::Sphere;
  lamps[0].position = camera + Vector3(30.0f, -10.0f, 400.0f) * k;
  lamps[0].radiance = Vector3(5e4f, 4e4f, 3e4f);
  lamps[0].radius = 2.0f * k;
  lamps[1].type = LocalLight::Type::Rect;
  lamps[1].position = camera + Vector3(-60.0f, 5.0f, 900.0f) * k;
  lamps[1].radiance = Vector3(2e4f, 2e4f, 2e4f);
  lamps[1].dimensions = Vector2(4.0f * k, 1.0f * k);
  // Ground 120 units below the camera, with roofs 40 units above it on every fourth point. All inside
  // one ground cell (cells are 20000 units at k = 1), so no point can change cell under the scaling.
  std::vector<Vector3> ground;
  for (int i = 0; i < 16; ++i) {
    const float lift = i % 4 == 3 ? 40.0f : 0.0f;
    ground.push_back(Vector3(2000.0f + 150.0f * float(i), 180.0f + lift, 600.0f + 120.0f * float(i)) * k);
  }

  for (uint32_t frame = 1; frame <= 3; ++frame) {
    FrameInputs in;
    in.frameIndex = frame;
    in.deltaSeconds = 1.0f / 60.0f;
    in.cameraCut = frame == 1;
    in.lastCameraCutFrame = 1;
    in.camera = makeCamera(view, proj, view, proj, 0.0f, 0.0f, 128, 64, nearPlane, false);
    in.cameraPosition = camera;
    in.cameraPositionGame = camera;
    in.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
    std::memcpy(in.viewToWorld, &in.camera.viewToWorld, sizeof(in.viewToWorld));
    std::memcpy(in.projectionToView, &in.camera.projectionToView, sizeof(in.projectionToView));
    in.renderWidth = 128;
    in.renderHeight = 64;
    in.volumetricsHandoffWorldUnits = 30.0f * k;
    in.localLights = lamps;
    in.localLightCount = 2;
    in.groundPoints = ground.data();
    in.groundPointCount = uint32_t(ground.size());
    // A host-placed strike on the last frame: render space into the cloud frame and back out.
    in.lightningStrike = frame == 3;
    in.lightningStrikeAtPosition = true;
    in.lightningStrikePosition = camera + Vector3(2000.0f, 15000.0f, 8000.0f) * k;
    renderer.beginFrame(gpu, in);
    renderer.dispatchPostDepth(gpu, 0, 1e9f);
  }
  const FrameOutputs& o = renderer.outputs();
  ScaledFrame f;
  f.args = o.args;
  f.lightning = o.lights[FrameOutputs::kLightningLight];
  f.cameraPosition = camera;
  const std::vector<uint8_t>& bytes = gpu.buffers[o.apLights];
  f.apLights.resize(o.args.apLocalLightCount);
  CHECK(bytes.size() >= f.apLights.size() * sizeof(AerialPerspectiveLight));
  if (bytes.size() >= f.apLights.size() * sizeof(AerialPerspectiveLight)) {
    std::memcpy(f.apLights.data(), bytes.data(), f.apLights.size() * sizeof(AerialPerspectiveLight));
  }
  return f;
}

static bool nearlyEqual(float a, float b) {
  return std::fabs(a - b) <= 1e-4f * std::max(std::max(std::fabs(a), std::fabs(b)), 1e-3f);
}

static bool nearlyEqual(const Vector3& a, const Vector3& b) {
  return nearlyEqual(a.x, b.x) && nearlyEqual(a.y, b.y) && nearlyEqual(a.z, b.z);
}

static void testWorldScaleInvariance() {
  const float k = 0.0625f;
  const ScaledFrame full = renderScaledScene(1.0f);
  const ScaledFrame small = renderScaledScene(k);

  // The game-space words of AtmosphereArgs scale by k; every other word is identical.
  const size_t scaled[] = {
    offsetof(AtmosphereArgs, worldUnitsPerKm), offsetof(AtmosphereArgs, apWorldUnitsPerKm),
    offsetof(AtmosphereArgs, cameraPosition), offsetof(AtmosphereArgs, cameraPosition) + 4,
    offsetof(AtmosphereArgs, cameraPosition) + 8,
    offsetof(AtmosphereArgs, apFroxelRange), offsetof(AtmosphereArgs, apFroxelLinearDepth),
    offsetof(AtmosphereArgs, apStartDistance),
  };
  const uint8_t* a = reinterpret_cast<const uint8_t*>(&full.args);
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&small.args);
  size_t mismatches = 0;
  for (size_t offset = 0; offset + 4 <= sizeof(AtmosphereArgs); offset += 4) {
    float fa, fb;
    std::memcpy(&fa, a + offset, 4);
    std::memcpy(&fb, b + offset, 4);
    const bool isScaled = std::find(std::begin(scaled), std::end(scaled), offset) != std::end(scaled);
    const bool ok = isScaled ? nearlyEqual(fa * k, fb)
                             : (std::memcmp(a + offset, b + offset, 4) == 0 || nearlyEqual(fa, fb));
    if (!ok) {
      std::printf("FAILED world scale invariance: AtmosphereArgs word at byte %zu: %g (full) vs %g (k = %g)\n",
                  offset, fa, fb, k);
      ++mismatches;
    }
  }
  CHECK(mismatches == 0);
  // The scene actually exercised the paths compared below.
  CHECK(full.args.apLocalLightCount == 3 && small.args.apLocalLightCount == 3);  // two lamps and the flash
  CHECK(full.args.lightningFlashIntensity > 0.0f && full.lightning.active && small.lightning.active);
  CHECK(full.args.fogExtinctionPerKm > 0.0f && full.args.cameraAltitudeKm > 0.0f);

  // The flash as a host light: placed and sized in the shrunk scene, exactly as bright.
  CHECK(nearlyEqual((full.lightning.position - full.cameraPosition) * k, small.lightning.position - small.cameraPosition));
  CHECK(nearlyEqual(full.lightning.radius * k, small.lightning.radius));
  CHECK(nearlyEqual(full.lightning.radiance, small.lightning.radiance));

  // The aerial perspective's lights: geometry shrinks, radiance and ranking do not.
  for (size_t i = 0; i < full.apLights.size() && i < small.apLights.size(); ++i) {
    const AerialPerspectiveLight& la = full.apLights[i];
    const AerialPerspectiveLight& lb = small.apLights[i];
    const Vector3 pa(la.position.x, la.position.y, la.position.z), pb(lb.position.x, lb.position.y, lb.position.z);
    CHECK(nearlyEqual((pa - full.cameraPosition) * k, pb - small.cameraPosition));
    CHECK(nearlyEqual(la.influenceRadius * k, lb.influenceRadius));
    CHECK(nearlyEqual(la.equivalentRadius * k, lb.equivalentRadius));
    CHECK(nearlyEqual(Vector3(la.radiance.x, la.radiance.y, la.radiance.z),
                      Vector3(lb.radiance.x, lb.radiance.y, lb.radiance.z)));
  }
}

// The automatic ground level's field (src/ground_field.h): a low quantile sees past roofs, the
// smoothing is a normalized Gaussian, unmeasured places fall back to the session mean, and the
// field depends on where, not when.
static void testGroundField() {
  GroundField f;
  f.configure(100.0f, 200.0f);
  // One cell of ground at 10 with a third of its samples on roofs at 25: the low quantile is ground.
  for (int i = 0; i < 60; ++i) {
    f.addSample(30.0f + float(i % 7), 40.0f + float(i % 5), i % 3 == 0 ? 25.0f : 10.0f, 0.0, true);
  }
  f.update(0.0, 0.0f, 0.0f, true);  // as in a camera cut's window: measurements apply at once
  float h = 0.0f;
  CHECK(f.evaluate(50.0f, 50.0f, 0.0, h) && std::fabs(h - 10.0f) < 1e-3f);

  // Two plateaus, 0 west of x = 0 and 100 east of it: halfway at the step, each value away from it.
  GroundField g;
  g.configure(100.0f, 200.0f);
  for (int iz = -10; iz < 10; ++iz) {
    for (int ix = -10; ix < 10; ++ix) {
      for (int s = 0; s < 8; ++s) {
        g.addSample(100.0f * float(ix) + 50.0f, 100.0f * float(iz) + 50.0f, ix < 0 ? 0.0f : 100.0f, 0.0, true);
      }
    }
  }
  CHECK(g.evaluate(0.0f, 0.0f, 0.0, h) && std::fabs(h - 50.0f) < 1.0f);
  CHECK(g.evaluate(-600.0f, 0.0f, 0.0, h) && h < 2.0f);
  CHECK(g.evaluate(600.0f, 0.0f, 0.0, h) && h > 98.0f);
  // Far from everything: the session mean (50).
  CHECK(g.evaluate(1e5f, -3e4f, 0.0, h) && std::fabs(h - 50.0f) < 1e-2f);

  // A new cell fades in rather than appearing at once; after a cut it applies immediately.
  GroundField n;
  n.configure(100.0f, 200.0f);
  for (int i = 0; i < 8; ++i) {
    n.addSample(50.0f, 50.0f, 7.0f, 10.0, false);
  }
  CHECK(!n.evaluate(50.0f, 50.0f, 10.0, h));
  CHECK(n.evaluate(50.0f, 50.0f, 10.0 + GroundField::kFadeInSeconds, h) && std::fabs(h - 7.0f) < 1e-4f);
  // A refined measurement moves the published value at the given rate, and all the way on a snap.
  for (int i = 0; i < GroundField::kSamplesPerCell; ++i) {
    n.addSample(50.0f, 50.0f, 27.0f, 20.0, false);
  }
  n.update(20.0, 1.0f, 5.0f, false);
  CHECK(n.evaluate(50.0f, 50.0f, 20.0, h) && std::fabs(h - 12.0f) < 1e-3f);
  n.update(20.0, 0.0f, 0.0f, true);
  CHECK(n.evaluate(50.0f, 50.0f, 20.0, h) && std::fabs(h - 27.0f) < 1e-3f);
  // A different grid is a different field.
  n.configure(50.0f, 200.0f);
  CHECK(n.cellCount() == 0 && !n.evaluate(50.0f, 50.0f, 20.0, h));
}

// The ground level in the renderer: host ground points and readback probe hits both reach it, in the
// world frame (world origin included); jumping does not move it; with Auto off the configured one.
class ProbeGpu final : public IGpu {
public:
  float groundHeight = 0.0f;  // render-space height (y) of every probe hit
  Vector3 around;             // hits are spread around this render-space point
  GpuHandle createTexture(const TextureDesc&) override { return m_next++; }
  GpuHandle createBuffer(const BufferDesc&) override { return m_next++; }
  GpuHandle createSampler(const SamplerDesc&) override { return m_next++; }
  void destroy(GpuHandle) override {}
  ConstantsRegion uploadConstants(const void*, size_t size) override { return { 1, 0, size }; }
  void updateBuffer(GpuHandle, size_t, size_t, const void*) override {}
  void copyBuffer(GpuHandle, size_t, GpuHandle, size_t, size_t) override {}
  void copyTexture(GpuHandle, GpuHandle) override {}
  void clearTexture(GpuHandle, const float*) override {}
  void uploadTexture(GpuHandle, uint32_t, const void*, size_t) override {}
  void dispatch(const DispatchDesc& d) override {
    if (d.pass == uint32_t(PassId::ground_probe)) {
      ++probeDispatches;
      CHECK(d.pushConstantSize == sizeof(GroundProbeArgs));
    }
  }
  void barrier() override {}
  bool readBuffer(GpuHandle, void* dst, size_t size) override {
    // Every readback is the probe batch: hits on flat ground, a quarter of them on roofs.
    float* hits = static_cast<float*>(dst);
    for (size_t i = 0; i < size / (4 * sizeof(float)); ++i) {
      hits[i * 4 + 0] = around.x + float(i % 8) * 30.0f;
      hits[i * 4 + 1] = groundHeight + (i % 4 == 3 ? 12.0f : 0.0f);
      hits[i * 4 + 2] = around.z + float(i / 8) * 30.0f;
      hits[i * 4 + 3] = 1.0f;
    }
    return true;
  }
  uint32_t probeDispatches = 0;

private:
  GpuHandle m_next = 2;
};

static void testGroundLevel() {
  auto run = [](Settings s, bool tlas, bool hostPoints, float cameraY, Vector3 origin, uint32_t frames,
                ProbeGpu& gpu) -> FrameOutputs {
    s.unitsPerMeter = 1.0f;
    Renderer renderer;
    renderer.setSettings(s);
    const Vector3 camera(100.0f, cameraY, 50.0f);
    const float view[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -camera.x, -camera.y, -camera.z, 1 };
    const float proj[16] = { 1, 0, 0, 0,  0, 1.777f, 0, 0,  0, 0, 1, 1,  0, 0, -0.1f, 0 };
    std::vector<Vector3> points;
    for (int i = 0; i < 32; ++i) {
      points.push_back(Vector3(camera.x + float(i % 8) * 25.0f, gpu.groundHeight, camera.z + float(i / 8) * 25.0f));
    }
    gpu.around = camera;
    for (uint32_t frame = 1; frame <= frames; ++frame) {
      FrameInputs in;
      in.frameIndex = frame;
      in.deltaSeconds = 1.0f / 60.0f;
      in.cameraCut = frame == 1;
      in.lastCameraCutFrame = 1;
      in.camera = makeCamera(view, proj, view, proj, 0.0f, 0.0f, 64, 32, 0.1f, false);
      in.cameraPosition = camera;
      in.cameraPositionGame = camera;
      in.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
      std::memcpy(in.viewToWorld, &in.camera.viewToWorld, sizeof(in.viewToWorld));
      std::memcpy(in.projectionToView, &in.camera.projectionToView, sizeof(in.projectionToView));
      in.renderWidth = 64;
      in.renderHeight = 32;
      in.hasWorldOrigin = true;
      in.worldOrigin = origin;
      in.opaqueTlas = tlas ? 999 : 0;
      in.groundPoints = hostPoints ? points.data() : nullptr;
      in.groundPointCount = hostPoints ? uint32_t(points.size()) : 0u;
      renderer.beginFrame(gpu, in);
      renderer.dispatchPostDepth(gpu, 0, 1e9f);
    }
    return renderer.outputs();
  };

  Settings s;
  s.groundLevelWorldUnits = -500.0f;
  {
    // Host points: the level is the ground, and the sky's altitude is the camera's height above it.
    ProbeGpu gpu;
    gpu.groundHeight = 40.0f;
    const FrameOutputs o = run(s, false, true, 70.0f, Vector3(0.0f), 4, gpu);
    CHECK(o.groundLevelMeasured && std::fabs(o.groundLevelWorldUnits - 40.0f) < 1e-2f);
    CHECK(std::fabs(o.args.cameraAltitudeKm - 0.030f) < 1e-5f);
    CHECK(gpu.probeDispatches == 0);
  }
  {
    // Probes against a TLAS, read back, in a host world whose origin moves the render frame up by 1000:
    // the level is a world height, 1040.
    ProbeGpu gpu;
    gpu.groundHeight = 40.0f;
    const FrameOutputs o = run(s, true, false, 70.0f, Vector3(0.0f, 1000.0f, 0.0f), 6, gpu);
    CHECK(gpu.probeDispatches > 0);
    CHECK(o.groundLevelMeasured && std::fabs(o.groundLevelWorldUnits - 1040.0f) < 1e-2f);
    CHECK(std::fabs(o.args.cameraAltitudeKm - 0.030f) < 1e-5f);
  }
  {
    // A jump does not move the ground level, only the camera's altitude above it.
    ProbeGpu gpu;
    gpu.groundHeight = 40.0f;
    const FrameOutputs o = run(s, false, true, 75.0f, Vector3(0.0f), 4, gpu);
    CHECK(std::fabs(o.groundLevelWorldUnits - 40.0f) < 1e-2f && std::fabs(o.args.cameraAltitudeKm - 0.035f) < 1e-5f);
  }
  {
    // Auto off: the configured level, and no probes.
    Settings manual = s;
    manual.groundLevelAuto = false;
    ProbeGpu gpu;
    gpu.groundHeight = 40.0f;
    const FrameOutputs o = run(manual, true, true, 70.0f, Vector3(0.0f), 4, gpu);
    CHECK(!o.groundLevelMeasured && o.groundLevelWorldUnits == -500.0f && gpu.probeDispatches == 0);
    CHECK(std::fabs(o.args.cameraAltitudeKm - 0.570f) < 1e-5f);
  }
}

// A probe GPU whose readbacks come back two frames after their copy, holding the ground as it was when
// the batch was cast (as a real GPU does): batches in flight across a teleport carry the old world.
class DelayedProbeGpu final : public IGpu {
public:
  float groundHeight = 0.0f;  // render-space height of every hit cast now
  Vector3 around;             // hits are cast around this render-space point
  uint32_t frame = 0;         // the test advances it once per frame
  GpuHandle createTexture(const TextureDesc&) override { return m_next++; }
  GpuHandle createBuffer(const BufferDesc&) override { return m_next++; }
  GpuHandle createSampler(const SamplerDesc&) override { return m_next++; }
  void destroy(GpuHandle) override {}
  ConstantsRegion uploadConstants(const void*, size_t size) override { return { 1, 0, size }; }
  void updateBuffer(GpuHandle, size_t, size_t, const void*) override {}
  void copyBuffer(GpuHandle dst, size_t, GpuHandle, size_t, size_t) override {
    m_copies[dst] = Copy { frame, groundHeight, around };
  }
  void copyTexture(GpuHandle, GpuHandle) override {}
  void clearTexture(GpuHandle, const float*) override {}
  void uploadTexture(GpuHandle, uint32_t, const void*, size_t) override {}
  void dispatch(const DispatchDesc&) override {}
  void barrier() override {}
  bool readBuffer(GpuHandle handle, void* dst, size_t size) override {
    const auto it = m_copies.find(handle);
    if (it == m_copies.end() || frame < it->second.frame + 2u) {
      return false;
    }
    float* hits = static_cast<float*>(dst);
    for (size_t i = 0; i < size / (4 * sizeof(float)); ++i) {
      hits[i * 4 + 0] = it->second.around.x + float(i % 8) * 30.0f;
      hits[i * 4 + 1] = it->second.height;
      hits[i * 4 + 2] = it->second.around.z + float(i / 8) * 30.0f;
      hits[i * 4 + 3] = 1.0f;
    }
    return true;
  }

private:
  struct Copy { uint32_t frame; float height; Vector3 around; };
  std::unordered_map<GpuHandle, Copy> m_copies;
  GpuHandle m_next = 2;
};

// A world change discards the ground field: nothing measured in the old world (cells near or far, the
// session mean, probes still in flight) reaches the new one. A camera cut alone keeps it.
static void testGroundReset() {
  enum class Cut { WorldChangedFlag, AnchorJump, CameraCutOnly };
  // Frames 1-40 in world A: ground 40 at the camera and, through host points, a wide plateau at 40
  // around it. Frame 41: the cut; from then on only probes, at the new ground. Returns whether every
  // measured level after the cut was the new world's, and the final level.
  auto run = [](Cut cut, float newGround, bool& allNew, bool& gap, float& finalLevel, bool& finalMeasured) {
    Settings s;
    s.unitsPerMeter = 1.0f;           // 1000 world units per km; ground sigma 4000, cells 2000
    s.groundLevelWorldUnits = -500.0f;
    Renderer renderer;
    renderer.setSettings(s);
    DelayedProbeGpu gpu;
    const float proj[16] = { 1, 0, 0, 0,  0, 1.777f, 0, 0,  0, 0, 1, 1,  0, 0, -0.1f, 0 };
    std::vector<Vector3> plateau;
    allNew = true;
    gap = false;
    for (uint32_t frame = 1; frame <= 80; ++frame) {
      const bool after = frame > 40;
      // The anchor jump moves the camera 6 km (a teleport); the other cuts stay put.
      const Vector3 camera(after && cut == Cut::AnchorJump ? 6100.0f : 100.0f, after ? newGround + 30.0f : 70.0f, 50.0f);
      const float view[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -camera.x, -camera.y, -camera.z, 1 };
      plateau.clear();
      if (!after) {
        for (int iz = -4; iz <= 4; ++iz) {
          for (int ix = -4; ix <= 4; ++ix) {
            plateau.push_back(Vector3(camera.x + 2000.0f * float(ix), 40.0f, camera.z + 2000.0f * float(iz)));
          }
        }
      }
      gpu.frame = frame;
      gpu.groundHeight = after ? newGround : 40.0f;
      gpu.around = camera;
      FrameInputs in;
      in.frameIndex = frame;
      in.deltaSeconds = 1.0f / 60.0f;
      in.cameraCut = frame == 1 || (frame == 41 && cut != Cut::AnchorJump);
      in.worldChanged = frame == 41 && cut == Cut::WorldChangedFlag;
      in.lastCameraCutFrame = frame >= 41 ? 41u : 1u;
      in.camera = makeCamera(view, proj, view, proj, 0.0f, 0.0f, 64, 32, 0.1f, false);
      in.cameraPosition = camera;
      in.cameraPositionGame = camera;
      in.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
      std::memcpy(in.viewToWorld, &in.camera.viewToWorld, sizeof(in.viewToWorld));
      std::memcpy(in.projectionToView, &in.camera.projectionToView, sizeof(in.projectionToView));
      in.renderWidth = 64;
      in.renderHeight = 32;
      in.opaqueTlas = 999;
      in.groundPoints = plateau.empty() ? nullptr : plateau.data();
      in.groundPointCount = uint32_t(plateau.size());
      renderer.beginFrame(gpu, in);
      renderer.dispatchPostDepth(gpu, 0, 1e9f);
      const FrameOutputs& o = renderer.outputs();
      if (after && o.groundLevelMeasured && std::fabs(o.groundLevelWorldUnits - newGround) > 1e-2f) {
        allNew = false;
      }
      gap = gap || (after && !o.groundLevelMeasured);
      finalLevel = o.groundLevelWorldUnits;
      finalMeasured = o.groundLevelMeasured;
    }
  };

  bool allNew = false, gap = false, measured = false;
  float level = 0.0f;
  // The host's flag: an interior at the same coordinates, its floor at 400.
  run(Cut::WorldChangedFlag, 400.0f, allNew, gap, level, measured);
  CHECK(allNew && measured && std::fabs(level - 400.0f) < 1e-2f);
  // A teleport (the anchor jumps 6 km, within the old plateau's kernel): no host flag needed.
  run(Cut::AnchorJump, 400.0f, allNew, gap, level, measured);
  CHECK(allNew && measured && std::fabs(level - 400.0f) < 1e-2f);
  // A camera cut alone is a view change over the same world: the field is kept, so the level stays
  // measured through the cut (no fallback to the configured -500) and still averages the plateau.
  run(Cut::CameraCutOnly, 40.0f, allNew, gap, level, measured);
  CHECK(allNew && !gap && measured && std::fabs(level - 40.0f) < 1e-2f);
}

// The screen march rate: the deprecated switches migrate to fixed rates, Auto follows the resolutions.
static void testMarchRate() {
  {
    Settings s;
    ExplicitSettings e;
    applyConfig(s, "rtx.atmosphere.cloudQuarterMarch = False\n", &e);
    migrateDeprecated(s, e, 0.0f);
    CHECK(s.cloudMarchRate == 1 && s.cloudQuarterMarch == Settings{}.cloudQuarterMarch);
    Settings t;
    ExplicitSettings et;
    applyConfig(t, "rtx.atmosphere.cloudQuarterMarch = True\nrtx.atmosphere.cloudSixteenthMarch = True\n", &et);
    migrateDeprecated(t, et, 0.0f);
    CHECK(t.cloudMarchRate == 3);
    Settings u;
    ExplicitSettings eu;
    applyConfig(u, "rtx.atmosphere.cloudSixteenthMarch = False\n", &eu);
    migrateDeprecated(u, eu, 0.0f);
    CHECK(u.cloudMarchRate == 0);
    Settings v;
    ExplicitSettings ev;
    applyConfig(v, "rtx.atmosphere.cloudQuarterMarch = True\nrtx.atmosphere.cloudMarchRate = 0\n", &ev);
    migrateDeprecated(v, ev, 0.0f);
    CHECK(v.cloudMarchRate == 0);
    CHECK(writeConfig(v).find("cloudQuarterMarch") == std::string::npos);
  }
  auto stride = [](Settings s, uint32_t renderHeight, uint32_t outputHeight) {
    ProbeGpu gpu;
    Renderer renderer;
    s.unitsPerMeter = 1.0f;
    renderer.setSettings(s);
    const float view[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, -2, 0, 1 };
    const float proj[16] = { 1, 0, 0, 0,  0, 1.777f, 0, 0,  0, 0, 1, 1,  0, 0, -0.1f, 0 };
    FrameInputs in;
    in.frameIndex = 1;
    in.deltaSeconds = 1.0f / 60.0f;
    in.cameraCut = true;
    in.camera = makeCamera(view, proj, view, proj, 0.0f, 0.0f, renderHeight * 16 / 9, renderHeight, 0.1f, false);
    in.cameraPosition = Vector3(0.0f, 2.0f, 0.0f);
    in.cameraPositionGame = in.cameraPosition;
    in.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
    std::memcpy(in.viewToWorld, &in.camera.viewToWorld, sizeof(in.viewToWorld));
    std::memcpy(in.projectionToView, &in.camera.projectionToView, sizeof(in.projectionToView));
    in.renderWidth = renderHeight * 16 / 9;
    in.renderHeight = renderHeight;
    in.outputWidth = outputHeight * 16 / 9;
    in.outputHeight = outputHeight;
    renderer.beginFrame(gpu, in);
    renderer.dispatchPostDepth(gpu, 0, 1e9f);
    return renderer.outputs().cloudMarchStride;
  };
  Settings s;
  CHECK(stride(s, 1080, 1080) == 2);  // native 1080p: quarter
  CHECK(stride(s, 1440, 2160) == 2);  // DLSS Quality at 4K (1.5x): quarter
  CHECK(stride(s, 720, 1080) == 2);   // DLSS Quality at 1080p: quarter, at the line floor
  CHECK(stride(s, 1253, 2160) == 1);  // Balanced (1.72x): full rate
  CHECK(stride(s, 1080, 2160) == 1);  // Performance (2x): full rate
  CHECK(stride(s, 720, 2160) == 1);   // Ultra Performance (3x): full rate
  CHECK(stride(s, 600, 600) == 1);    // native but below 720 lines: full rate
  Settings fixed = s;
  fixed.cloudMarchRate = 3;
  CHECK(stride(fixed, 720, 2160) == 4);  // a fixed rate is kept
  Settings off = s;
  off.cloudTemporalHistoryWeight = 0.0f;
  CHECK(stride(off, 1080, 1080) == 1);   // no temporal filter, no sparse march
}

// Night gain: none by day or at 0 stops, the full gain at night, and the sky never brightens while the
// sun sets (twilight from the renderer's relativeSkyLuminance fit, 10^(0.45 h)).
static void testNightGain() {
  CHECK(nightGainInForce(1.0f, 2e-6f, 0.0f) == 1.0f);
  for (float night : { 2e-6f, 2e-7f, 6e-8f }) {
    for (float stops : { 1.0f, 2.0f, 4.0f, 10.0f, 12.0f }) {
      CHECK(std::fabs(nightGainInForce(1.0f, night, stops) - 1.0f) < 1e-3f);
      CHECK(std::fabs(nightGainInForce(1e-12f, night, stops) - std::exp2(stops)) < 1e-3f * std::exp2(stops));
      float previous = 1e30f;
      for (float h = 10.0f; h > -30.0f; h -= 0.01f) {
        const float twilight = h >= 0.0f ? 1.0f : std::pow(10.0f, 0.45f * h);
        const float total = twilight + night * nightGainInForce(twilight, night, stops);
        CHECK(total <= previous * (1.0f + 1e-5f));
        previous = total;
      }
    }
  }
}

static void testConfigFile(const char* path) {
  std::ifstream f(path);
  if (!f) {
    std::printf("could not open %s\n", path);
    ++g_failures;
    return;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  Settings s;
  ExplicitSettings e;
  const size_t n = applyConfig(s, ss.str(), &e);
  migrateDeprecated(s, e, 0.0f);
  std::printf("%s: %zu NUMOS settings applied\n", path, n);
}

static void testWeather() {
  const Settings base;
  // The settings' own weather round-trips.
  Settings applied = base;
  applyWeather(weatherFromSettings(base), applied);
  CHECK(applied.cloudCoverageMean == base.cloudCoverageMean && applied.cloudDepthMeters == base.cloudDepthMeters);

  // Colour settings travel per channel: a tinted aerosol round-trips and blends channel by channel.
  {
    Settings dusty = base;
    dusty.aerosolTint = Vector3(1.0f, 0.8f, 0.5f);
    Settings back = base;
    applyWeather(weatherFromSettings(dusty), back);
    CHECK(back.aerosolTint.x == 1.0f && back.aerosolTint.y == 0.8f && back.aerosolTint.z == 0.5f);
    const WeatherSnapshot half = blendWeather(weatherFromSettings(base), weatherFromSettings(dusty), 0.5f);
    CHECK(std::fabs(half.aerosolTintB - 0.75f) < 1e-6f && half.airTintG == 1.0f);
  }

  WeatherSnapshot a = weatherPreset(WeatherPreset::Clear, base);
  WeatherSnapshot b = weatherPreset(WeatherPreset::Storm, base);
  CHECK(a.cloudCoverageMean == 0.0f && b.lightningStrikesPerMinute > 0.0f);
  const WeatherSnapshot mid0 = blendWeather(a, b, 0.0f);
  const WeatherSnapshot mid1 = blendWeather(a, b, 1.0f);
  CHECK(mid0.cloudDepthMeters == a.cloudDepthMeters && mid1.cloudDepthMeters == b.cloudDepthMeters);
  CHECK(std::fabs(blendWeather(a, b, 0.5f).cloudCoverageMean - 0.5f * b.cloudCoverageMean) < 1e-6f);

  // Wind direction goes the short way round 0/360.
  a.cloudWindDirection = 350.0f;
  b.cloudWindDirection = 10.0f;
  CHECK(std::fabs(blendWeather(a, b, 0.5f).cloudWindDirection - 0.0f) < 1e-3f
        || std::fabs(blendWeather(a, b, 0.5f).cloudWindDirection - 360.0f) < 1e-3f);

  // Fog visibility blends as extinction: none -> 300 m passes 600 m halfway, never a 150 m jump.
  a.fogVisibilityMeters = 0.0f;
  b.fogVisibilityMeters = 300.0f;
  CHECK(std::fabs(blendWeather(a, b, 0.5f).fogVisibilityMeters - 600.0f) < 1e-2f);
  CHECK(blendWeather(a, b, 0.0f).fogVisibilityMeters == 0.0f);

  // Layer 2 fades in where Fog puts it: on through the transition, coverage from 0, and the fog
  // bank's own height and depth from the first frame (not sweeping down from the settings' deck).
  {
    const WeatherSnapshot fair = weatherPreset(WeatherPreset::Fair, base);
    const WeatherSnapshot fog = weatherPreset(WeatherPreset::Fog, base);
    CHECK(fair.cloudLayer2Enable == 0.0f && fog.cloudLayer2Enable == 1.0f && fog.cloudLayer2CoverageMean > 0.5f);
    const WeatherSnapshot q = blendWeather(fair, fog, 0.25f);
    CHECK(q.cloudLayer2Enable == 1.0f);
    CHECK(std::fabs(q.cloudLayer2CoverageMean - 0.25f * fog.cloudLayer2CoverageMean) < 1e-6f);
    CHECK(q.cloudLayer2BaseHeightMeters == fog.cloudLayer2BaseHeightMeters
          && q.cloudLayer2DepthMeters == fog.cloudLayer2DepthMeters);
    const WeatherSnapshot back = blendWeather(fog, fair, 0.75f);
    CHECK(back.cloudLayer2Enable == 1.0f && back.cloudLayer2BaseHeightMeters == fog.cloudLayer2BaseHeightMeters);
    CHECK(blendWeather(fog, fair, 1.0f).cloudLayer2Enable == 0.0f);
    Settings s = base;
    applyWeather(fog, s);
    CHECK(s.cloudLayer2Enable && s.cloudLayer2DepthMeters == fog.cloudLayer2DepthMeters);
  }

  // Names round-trip, case-insensitively.
  for (uint32_t i = 0; i < uint32_t(WeatherPreset::Count); ++i) {
    WeatherPreset p;
    CHECK(weatherPresetFromName(weatherPresetName(WeatherPreset(i)), p) && p == WeatherPreset(i));
  }
  WeatherPreset p;
  CHECK(weatherPresetFromName("partlycloudy", p) && p == WeatherPreset::PartlyCloudy);
  CHECK(!weatherPresetFromName("hail", p));

  // The blender eases to the target and stays there.
  WeatherBlender blender(weatherPreset(WeatherPreset::Clear, base));
  blender.setTarget(weatherPreset(WeatherPreset::Overcast, base), 10.0f);
  blender.update(5.0f);
  CHECK(blender.transitioning());
  CHECK(std::fabs(blender.current().cloudCoverageMean - 0.5f * weatherPreset(WeatherPreset::Overcast, base).cloudCoverageMean) < 1e-5f);
  blender.update(6.0f);
  CHECK(!blender.transitioning());
  CHECK(blender.current().cloudCoverageMean == weatherPreset(WeatherPreset::Overcast, base).cloudCoverageMean);

  // Every field names a float Settings field or a colour component (the X-lists would not compile
  // otherwise) and is listed once: 28 scalars, then air and aerosol tint RGB.
  size_t n = 0;
  const WeatherFieldInfo* f = weatherFields(n);
  CHECK(n == 34 && f[0].member == &WeatherSnapshot::cloudCoverageMean && f[33].member == &WeatherSnapshot::aerosolTintB);
}

// Seeds: a configuration, weather and simulation state survive the trip; damage is rejected.
static void testSeed() {
  {
    std::unordered_set<uint32_t> hashes;
    for (const SettingInfo& info : settingInfos()) {
      uint32_t h = 2166136261u;
      for (char c : info.key) h = (h ^ uint8_t(c)) * 16777619u;
      CHECK(hashes.insert(h).second);
    }
  }
  Settings s;
  s.cloudCoverageMean = 0.71f;
  s.cloudLayer2Enable = true;
  s.rayleighScattering = Vector3(1.0f, 2.0f, 3.0f);
  s.moons[1].enabled = !s.moons[1].enabled;
  WeatherSnapshot w = weatherPreset(WeatherPreset::Storm, s);
  SimulationState sim;
  sim.cloudAdvectOffset[0] = 12.5f;
  sim.timeOfDayHours = 18.25f;
  sim.animationSeconds = 1234.5;
  const std::string seed = encodeSeed(s, &w, &sim);
  std::printf("  seed (%zu chars): %s\n", seed.size(), seed.c_str());
  SeedContents c;
  CHECK(decodeSeed(seed, c));
  CHECK(std::memcmp(&c.settings, &s, sizeof(Settings)) == 0 || writeConfig(c.settings) == writeConfig(s));
  CHECK(c.hasWeather && c.hasSimulation && c.unknownEntries == 0);
  CHECK(std::memcmp(&c.weather, &w, sizeof(w)) == 0);
  CHECK(c.simulation.cloudAdvectOffset[0] == 12.5f && c.simulation.timeOfDayHours == 18.25f
        && c.simulation.animationSeconds == 1234.5);
  // Defaults: a short seed, decoding to defaults.
  const std::string plain = encodeSeed(Settings{});
  CHECK(plain.size() < 24);
  CHECK(decodeSeed(plain, c) && !c.hasWeather && writeConfig(c.settings) == writeConfig(Settings{}));
  // One wrong character fails the checksum.
  std::string bad = seed;
  bad[bad.size() / 2] = bad[bad.size() / 2] == 'A' ? 'B' : 'A';
  std::string error;
  CHECK(!decodeSeed(bad, c, &error) && !error.empty());
  CHECK(!decodeSeed("hello", c));
}

int main(int argc, char** argv) {
  setLogCallback([](LogLevel, const char* m, void*) { std::printf("  log: %s\n", m); }, nullptr);
  std::printf("sizeof(numos::AtmosphereArgs) = %zu, settings = %zu\n", sizeof(AtmosphereArgs), settingInfos().size());
  testInfos();
  testRoundTrip();
  testParsing();
  testMigration();
  testPasses();
  testWeather();
  testSeed();
  testGpuLifetime();
  testWorldScaleInvariance();
  testGroundField();
  testGroundLevel();
  testGroundReset();
  testMarchRate();
  testNightGain();
  if (argc > 1) {
    testConfigFile(argv[1]);
  }
  std::printf(g_failures ? "%d FAILURES\n" : "all tests passed\n", g_failures);
  return g_failures;
}