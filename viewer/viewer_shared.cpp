// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "viewer_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "numos/shaders.h"

namespace numos::viewer {

namespace {

const PassBindingInfo kViewerBindings[] = {
#define NUMOS_PASS_BINDING(slot, set, kind, dim, fmt, var) { slot, set, BindingKind::kind, BindingDim::dim, fmt, var },
#include "generated/viewer_passes.inc"
};
const PassInfo kViewerPasses[] = {
#define NUMOS_PASS(name, file, hash, lx, ly, lz, push, bBegin, bCount) \
  { #name, file, hash, { lx, ly, lz }, push, kViewerBindings + (bBegin), bCount },
#include "generated/viewer_passes.inc"
};

} // namespace

void check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) {
    throw std::runtime_error(std::string(what) + " failed: " + std::to_string(int(r)));
  }
}

std::vector<uint32_t> readSpirv(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("missing " + path);
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), words.size() * 4);
  return words;
}

Mat4 lookTo(const Vector3& eye, const Vector3& forward, const Vector3& up) {
  const Vector3 z = normalize(forward);
  const Vector3 x = normalize(cross(up, z));
  const Vector3 y = cross(z, x);
  Mat4 v = Mat4::identity();
  v.m[0] = x.x; v.m[4] = x.y; v.m[8] = x.z;  v.m[12] = -dot(x, eye);
  v.m[1] = y.x; v.m[5] = y.y; v.m[9] = y.z;  v.m[13] = -dot(y, eye);
  v.m[2] = z.x; v.m[6] = z.y; v.m[10] = z.z; v.m[14] = -dot(z, eye);
  return v;
}

Mat4 perspective(float fovY, float aspect, float zn, float zf) {
  const float h = 1.0f / std::tan(fovY * 0.5f);
  Mat4 p;
  p.m[0] = h / aspect;
  p.m[5] = h;
  p.m[10] = zf / (zf - zn);
  p.m[11] = 1.0f;
  p.m[14] = -zn * zf / (zf - zn);
  return p;
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
      return argv[++i];
    };
    if (a == "--conf") o.conf = next();
    else if (a == "--set") o.sets.push_back(next());
    else if (a == "--sun-elevation") o.sunElevation = std::stof(next());
    else if (a == "--sun-speed") o.sunSpeed = std::stof(next());
    else if (a == "--sun-azimuth") o.sunAzimuth = std::stof(next());
    else if (a == "--yaw") o.yaw = std::stof(next());
    else if (a == "--pitch") o.pitch = std::stof(next());
    else if (a == "--eye-height") o.eyeHeight = std::stof(next());
    else if (a == "--fly-speed") o.flySpeed = std::stof(next());
    else if (a == "--fov-y") o.fovY = std::stof(next());
    else if (a == "--width") o.width = uint32_t(std::stoul(next()));
    else if (a == "--height") o.height = uint32_t(std::stoul(next()));
    else if (a == "--frames") o.frames = uint32_t(std::stoul(next()));
    else if (a == "--exposure") o.exposure = std::stof(next());
    else if (a == "--accumulate") o.accumulate = uint32_t(std::stoul(next()));
    else if (a == "--secondary-sky") o.secondarySky = true;
    else if (a == "--timings") o.timings = true;
    else if (a == "--out") o.out = next();
    else if (a == "--hdr-out") o.hdrOut = next();
    else if (a == "--root") o.root = next();
    else if (a == "--night-sky") o.nightSky = next();
    else if (a == "--validate") o.validate = true;
    else if (a == "--no-ground") o.noGround = true;
    else if (a == "--weather") o.weather = next();
    else if (a == "--weather-to") o.weatherTo = next();
    else if (a == "--weather-seconds") o.weatherSeconds = std::stof(next());
    else if (a == "--sky-ambient-check") o.skyAmbientCheck = uint32_t(std::stoul(next()));
    else if (a == "--lightning-at") o.lightningAt.push_back(uint32_t(std::stoul(next())));
    else if (a == "--sequence-dir") o.sequenceDir = next();
    else if (a == "--seed") o.seed = next();
    else if (a == "--print-seed") o.printSeed = true;
    else if (a == "--sequence-every") o.sequenceEvery = std::max(1u, uint32_t(std::stoul(next())));
    else if (a == "--interactive") o.interactive = true;
    else if (a == "--lightning-pos") {
      float v[3] = {};
      if (std::sscanf(next().c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) {
        throw std::runtime_error("--lightning-pos wants x,y,z");
      }
      o.lightningAtPosition = true;
      o.lightningPosition = Vector3(v[0], v[1], v[2]);
    }
    else if (a == "--light") {
      float v[7] = {};
      if (std::sscanf(next().c_str(), "%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7) {
        throw std::runtime_error("--light wants x,y,z,r,g,b,radius");
      }
      LocalLight l;
      l.type = LocalLight::Type::Sphere;
      l.position = Vector3(v[0], v[1], v[2]);
      l.radiance = Vector3(v[3], v[4], v[5]);
      l.radius = v[6];
      o.lights.push_back(l);
    }
    else throw std::runtime_error("unknown option " + a);
  }
  return o;
}

Settings startSettings(const Options& opt, SeedContents& seed) {
  Settings settings;
  ExplicitSettings explicitlySet;
  if (!opt.conf.empty()) {
    std::ifstream f(opt.conf);
    std::stringstream ss;
    ss << f.rdbuf();
    const size_t n = applyConfig(settings, ss.str(), &explicitlySet);
    std::printf("loaded %zu NUMOS settings from %s\n", n, opt.conf.c_str());
  }
  std::string overrides;
  if (!opt.seed.empty()) {
    std::string error;
    if (!decodeSeed(opt.seed, seed, &error)) {
      throw std::runtime_error("--seed: " + error);
    }
    settings = seed.settings;
    std::printf("seed: %s%s, %u unknown entries skipped\n", seed.hasWeather ? "weather, " : "",
                seed.hasSimulation ? "clocks" : "no clocks", seed.unknownEntries);
  } else {
    overrides = "rtx.atmosphere.timeCycleEnable = False\n";
    overrides += "rtx.atmosphere.sunElevation = " + std::to_string(opt.sunElevation) + "\n";
    overrides += "rtx.atmosphere.sunRotation = " + std::to_string(opt.sunAzimuth) + "\n";
  }
  for (const std::string& s : opt.sets) {
    overrides += s + "\n";
  }
  applyConfig(settings, overrides, &explicitlySet);
  migrateDeprecated(settings, explicitlySet, 0.0f);
  return settings;
}

void loadNightSky(const std::string& dir, NightSkyFiles& files, Renderer& renderer) {
  auto load = [&](const char* name, std::vector<char>& bytes) {
    std::ifstream f(dir + "/" + name, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  };
  load("stars.bin", files.stars);
  load("milkyway.bin", files.milkyWay);
  load("starlight_low.bin", files.starlightLow);
  load("moon.bin", files.moon);
  NightSkyAssets assets;
  if (!files.stars.empty()) { assets.stars = files.stars.data(); assets.starsSize = files.stars.size(); }
  if (!files.milkyWay.empty()) { assets.milkyWay = files.milkyWay.data(); assets.milkyWaySize = files.milkyWay.size(); }
  if (!files.starlightLow.empty()) { assets.starlightLow = files.starlightLow.data(); assets.starlightLowSize = files.starlightLow.size(); }
  if (!files.moon.empty()) { assets.moon = files.moon.data(); assets.moonSize = files.moon.size(); }
  // Game moons' own surfaces, if any (build_nightsky_assets.py --custom-moon).
  for (uint32_t i = 0; i < kMaxMoons; ++i) {
    load(("moon_game" + std::to_string(i) + ".bin").c_str(), files.gameMoons[i]);
    if (!files.gameMoons[i].empty()) {
      assets.gameMoons[i] = files.gameMoons[i].data();
      assets.gameMoonSizes[i] = files.gameMoons[i].size();
    }
  }
  renderer.setNightSkyAssets(assets);
}

void writePpm(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
  std::ofstream f(path, std::ios::binary);
  f << "P6\n" << width << " " << height << "\n255\n";
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    f.put(char(rgba[i * 4 + 0])).put(char(rgba[i * 4 + 1])).put(char(rgba[i * 4 + 2]));
  }
}

ViewerPasses registerViewerPasses(vulkan::VulkanGpu& gpu, const std::string& root) {
  ViewerPasses p;
  p.composite = gpu.registerPass(kViewerPasses[0], readSpirv(root + "/build/viewer_spirv/" + kViewerPasses[0].spirvFile));
  p.depth = gpu.registerPass(kViewerPasses[1], readSpirv(root + "/build/viewer_spirv/" + kViewerPasses[1].spirvFile));
  return p;
}

ViewerArgs makeViewerArgs(const FrameOutputs& outs0, const ViewerFrameParams& p) {
  ViewerArgs va {};
  va.groundAlbedo = p.groundAlbedo;
  // No ground lifts the plane out of reach above the camera, so the depth pass misses everywhere.
  va.groundHeight = p.noGround ? 1e30f : 0.0f;
  va.sunRadiance = outs0.lights[FrameOutputs::kSunLight].radiance;
  va.missLinearViewZ = kMissLinearViewZ;
  va.sunDirection = outs0.lights[FrameOutputs::kSunLight].direction;
  va.sunCloudShadowed = outs0.lights[FrameOutputs::kSunLight].cloudShadowed ? 1u : 0u;
  {
    float best = 0.0f;
    va.moonDirection = vec3(0.0f, -1.0f, 0.0f);
    for (uint32_t i = 0; i < kMaxMoons; ++i) {
      const EmittedLight& l = outs0.lights[FrameOutputs::kFirstMoonLight + i];
      const float y = l.active ? l.radiance.x * 0.2126f + l.radiance.y * 0.7152f + l.radiance.z * 0.0722f : 0.0f;
      if (y > best) {
        best = y;
        va.moonRadiance = l.radiance;
        va.moonDirection = l.direction;
        va.moonCloudShadowed = l.cloudShadowed ? 1u : 0u;
      }
    }
  }
  {
    const EmittedLight& flash = outs0.lights[FrameOutputs::kLightningLight];
    if (flash.active && flash.type == EmittedLight::Type::Sphere) {
      va.flashPosition = flash.position;
      va.flashRadius = flash.radius;
      va.flashRadiance = flash.radiance;
    }
  }
  va.zUp = 0;
  // NUMOS radiance is pre-exposed by radianceScale; the viewer's exposure stays a physical exposure.
  va.exposure = p.exposure / outs0.radianceScale;
  va.frameIdx = p.frameIdx;
  va.outputWidth = p.width;
  va.outputHeight = p.height;
  va.secondarySky = p.secondarySky ? 1u : 0u;
  va.skyAmbientCheck = p.skyAmbientCheck;
  va.accumFrames = p.accumFrames;
  return va;
}

void dispatchViewerDepth(vulkan::VulkanGpu& gpu, const ViewerPasses& passes, const ViewerArgs& va,
                         const Camera& camera, GpuHandle depth, uint32_t width, uint32_t height) {
  const IGpu::ConstantsRegion args = gpu.uploadConstants(&va, sizeof(va));
  const IGpu::ConstantsRegion cam = gpu.uploadConstants(&camera, sizeof(camera));
  Binding b[3];
  b[0].slot = VIEWER_DEPTH_ARGS; b[0].kind = BindingKind::ConstantBuffer; b[0].resource = args.buffer; b[0].offset = args.offset; b[0].size = args.size;
  b[1].slot = VIEWER_DEPTH_CAMERA; b[1].kind = BindingKind::ConstantBuffer; b[1].resource = cam.buffer; b[1].offset = cam.offset; b[1].size = cam.size;
  b[2].slot = VIEWER_DEPTH_OUTPUT; b[2].kind = BindingKind::StorageTexture; b[2].resource = depth; b[2].mipCount = 1;
  DispatchDesc d;
  d.pass = passes.depth;
  d.bindings = b;
  d.bindingCount = 3;
  d.groupsX = (width + 7) / 8;
  d.groupsY = (height + 7) / 8;
  gpu.dispatch(d);
}

void dispatchViewerComposite(vulkan::VulkanGpu& gpu, const ViewerPasses& passes, const ViewerArgs& va,
                             const Camera& camera, const FrameOutputs& outs, GpuHandle depth,
                             GpuHandle output, GpuHandle accum, uint32_t width, uint32_t height) {
  // Bindings in the table's order.
  const IGpu::ConstantsRegion args = gpu.uploadConstants(&va, sizeof(va));
  const IGpu::ConstantsRegion cam = gpu.uploadConstants(&camera, sizeof(camera));
  const IGpu::ConstantsRegion atm = gpu.uploadConstants(&outs.args, sizeof(outs.args));
  std::unordered_map<uint32_t, Binding> bySlot;
  auto tex = [&](uint32_t slot, GpuHandle h) { Binding b; b.slot = slot; b.resource = h; bySlot[slot] = b; };
  auto cb = [&](uint32_t slot, const IGpu::ConstantsRegion& r) {
    Binding b; b.slot = slot; b.resource = r.buffer; b.offset = r.offset; b.size = r.size; bySlot[slot] = b;
  };
  tex(VIEWER_TRANSMITTANCE_LUT, outs.transmittanceLut);
  tex(VIEWER_MULTISCATTERING_LUT, outs.multiscatteringLut);
  tex(VIEWER_SKY_VIEW_LUT, outs.skyViewLut);
  tex(VIEWER_SKY_VIEW_SAMPLER, outs.skyViewSampler);
  tex(VIEWER_CLOUD_SKY_TRANSMITTANCE, outs.cloudSkyTransmittanceLut);
  tex(VIEWER_CLOUD_SHADOW_MAP, outs.cloudShadowMap);
  tex(VIEWER_CLOUD_NOISE_SAMPLER, outs.cloudNoiseSampler);
  tex(VIEWER_CLOUD_SECONDARY_LUT, outs.cloudSecondaryLut);
  tex(VIEWER_BLUE_NOISE, 0);
  cb(VIEWER_ARGS, args);
  cb(VIEWER_CAMERA, cam);
  cb(VIEWER_ATMOSPHERE_ARGS, atm);
  tex(VIEWER_AP_NEAR_SCATTER, outs.apNearScatter);
  tex(VIEWER_AP_FAR_SCATTER, outs.apFarScatter);
  tex(VIEWER_AP_FAR_TRANSMITTANCE, outs.apFarTransmittance);
  tex(VIEWER_AP_FROXEL_AUX, outs.apFroxelAux);
  tex(VIEWER_AP_LIGHTS, outs.apLights);
  tex(VIEWER_AP_LIGHT_CLUSTERS, outs.apLightClusters);
  tex(VIEWER_AP_SAMPLER, outs.linearClampSampler);
  tex(VIEWER_CLOUD_RENDER, outs.cloudRender);
  tex(VIEWER_CLOUD_DEPTH, outs.cloudDepth);
  tex(VIEWER_LINEAR_VIEW_Z, depth);
  tex(VIEWER_OUTPUT, output);
  tex(VIEWER_ACCUM, accum);
  tex(VIEWER_NIGHT_SKY_STAR_CELLS, outs.nightSkyStarCells);
  tex(VIEWER_NIGHT_SKY_STARS, outs.nightSkyStars);
  tex(VIEWER_NIGHT_SKY_MILKY_WAY, outs.nightSkyMilkyWay);
  tex(VIEWER_NIGHT_SKY_STARLIGHT_LOW, outs.nightSkyStarlightLow);
  tex(VIEWER_NIGHT_SKY_MOON, outs.nightSkyMoon);
  const PassInfo& pi = kViewerPasses[0];
  std::vector<Binding> b;
  for (uint32_t i = 0; i < pi.bindingCount; ++i) {
    Binding x = bySlot[pi.bindings[i].slot];
    x.kind = pi.bindings[i].kind;
    if (x.kind == BindingKind::StorageTexture) x.mipCount = 1;
    b.push_back(x);
  }
  DispatchDesc d;
  d.pass = passes.composite;
  d.bindings = b.data();
  d.bindingCount = uint32_t(b.size());
  d.groupsX = (width + 7) / 8;
  d.groupsY = (height + 7) / 8;
  gpu.dispatch(d);
}

std::string pipelineCachePath(const std::string& root) {
  return root + "/build/numos_viewer_pipeline_cache.bin";
}

std::vector<uint8_t> readPipelineCache(const std::string& path) {
  // Each shader change adds its pipelines and nothing removes old ones, so a cache past this size
  // is dropped; the run then saves only the pipelines it uses.
  constexpr std::uintmax_t kMaxBytes = 128u << 20;
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxBytes) {
    return {};
  }
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writePipelineCache(const std::string& path, const std::vector<uint8_t>& data,
                        const std::vector<uint8_t>& previous) {
  if (data.empty() || data.size() == previous.size()) {
    return;
  }
  // Written beside the file and moved over it, so an interrupted write leaves the old cache.
  const std::string temp = path + ".tmp";
  {
    std::ofstream f(temp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!f) {
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
}

} // namespace numos::viewer
