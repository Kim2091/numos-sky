// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

// Pieces of the viewer that the headless run (main.cpp) and the interactive window (app_*.cpp)
// share: the command line, the start settings, the night sky files, the camera matrices and the
// viewer's own two passes (ground depth and composite).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "numos/renderer.h"
#include "numos/seed.h"
#include "numos/settings.h"
#include "numos_vulkan.h"
#include "shaders/viewer_common.h"

namespace numos::viewer {

void check(VkResult r, const char* what);
std::vector<uint32_t> readSpirv(const std::string& path);

// Column-major 4x4, element (row, col) at m[col * 4 + row].
struct Mat4 {
  float m[16] = {};
  static Mat4 identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
  }
};

// Left-handed view (+Z forward, +Y up), D3D convention.
Mat4 lookTo(const Vector3& eye, const Vector3& forward, const Vector3& up);
// D3D left-handed perspective, depth 0..1.
Mat4 perspective(float fovY, float aspect, float zn, float zf);

struct Options {
  std::string conf;
  std::vector<std::string> sets;
  float sunElevation = 25.0f, sunAzimuth = 135.0f;
  float yaw = 135.0f, pitch = 8.0f, eyeHeight = 2.0f;
  float fovY = 70.0f;  // vertical field of view, degrees
  float flySpeed = 0.0f;  // m/s along the view direction (60 fps): a moving camera for temporal tests
  uint32_t width = 1280, height = 720, frames = 48;
  float exposure = 0.12f;
  uint32_t accumulate = 0;  // average the last n frames in linear light (still-camera denoise)
  bool secondarySky = false;  // sky through the secondary-ray path (reflection dome LUT)
  bool timings = false;  // print per-zone GPU times of the last frame
  std::vector<LocalLight> lights;
  std::string out = "viewer.ppm";
  std::string hdrOut;  // linear accumulated radiance x exposure as PFM (needs --accumulate)
  std::string root = ".";
  std::string nightSky;  // --night-sky: night sky assets folder (default <root>/assets/nightsky)
  bool validate = false;
  std::string weather;    // --weather Preset: render that weather preset over the settings
  std::string weatherTo;  // --weather-to Preset: blend toward it over --weather-seconds from frame 1
  float weatherSeconds = 1.0f;
  bool noGround = false;  // no ground plane: every ray misses, as a skybox with no terrain below it
  uint32_t skyAmbientCheck = 0;  // --sky-ambient-check n: diagnostic output (viewer_composite.comp.slang)
  std::vector<uint32_t> lightningAt;  // --lightning-at f: force a strike at frame f (automatic strikes off)
  bool lightningAtPosition = false;   // --lightning-pos x,y,z: ... there (metres, render space)
  Vector3 lightningPosition;
  std::string sequenceDir;     // --sequence-dir: also write frames there (fNNNNN.ppm), for demo videos
  uint32_t sequenceEvery = 1;  // --sequence-every n: every nth frame
  std::string seed;            // --seed: replaces --conf and the sun options
  bool printSeed = false;      // --print-seed
  float sunSpeed = 0.0f;       // --sun-speed: degrees of sun elevation per second (fast time cycles)
  bool interactive = false;    // --interactive: open the window (app_main.cpp) instead of rendering headless
};

Options parse(int argc, char** argv);

// The settings a run starts from: --conf, or --seed, then the sun options (without a seed) and --set.
// Fills `seed` when --seed is given.
Settings startSettings(const Options& opt, SeedContents& seed);

// The night sky files of one folder. The renderer copies what it is given; the bytes are kept here
// so a caller can see which files were found.
struct NightSkyFiles {
  std::vector<char> stars, milkyWay, starlightLow, moon;
  std::vector<char> gameMoons[kMaxMoons];
};
// Loads the files of `dir` and hands them to the renderer.
void loadNightSky(const std::string& dir, NightSkyFiles& files, Renderer& renderer);

// Writes RGBA8 pixels as a binary PPM (alpha dropped).
void writePpm(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba);

// The Vulkan pipeline cache (VulkanGpu::pipelineCacheData) is kept in <root>/build between runs, so a
// run whose driver lost its own shader cache does not compile the large cloud passes again.
std::string pipelineCachePath(const std::string& root);
// The file's bytes, or nothing when it is missing or larger than 128 MB.
std::vector<uint8_t> readPipelineCache(const std::string& path);
// Replaces the file with `data` if it differs in size from `previous` (what the run loaded); the cache
// only grows, so an unchanged size means nothing new was compiled.
void writePipelineCache(const std::string& path, const std::vector<uint8_t>& data,
                        const std::vector<uint8_t>& previous);

struct ViewerPasses {
  uint32_t composite = 0;
  uint32_t depth = 0;
};
// Registers the viewer's passes from <root>/build/viewer_spirv.
ViewerPasses registerViewerPasses(vulkan::VulkanGpu& gpu, const std::string& root);

// The depth pass writes this linear view Z where the ray misses the ground.
constexpr float kMissLinearViewZ = 1e9f;

struct ViewerFrameParams {
  bool     noGround = false;
  float    exposure = 0.12f;      // physical exposure; the radiance scale is divided out here
  uint32_t frameIdx = 0;
  uint32_t width = 0, height = 0;
  bool     secondarySky = false;
  uint32_t skyAmbientCheck = 0;
  uint32_t accumFrames = 0;       // ViewerArgs::accumFrames
  vec3     groundAlbedo = vec3(0.18f, 0.17f, 0.15f);
};
// The composite's constants for this frame, from the renderer's outputs after beginFrame.
ViewerArgs makeViewerArgs(const FrameOutputs& outs, const ViewerFrameParams& p);

void dispatchViewerDepth(vulkan::VulkanGpu& gpu, const ViewerPasses& passes, const ViewerArgs& va,
                         const Camera& camera, GpuHandle depth, uint32_t width, uint32_t height);
void dispatchViewerComposite(vulkan::VulkanGpu& gpu, const ViewerPasses& passes, const ViewerArgs& va,
                             const Camera& camera, const FrameOutputs& outs, GpuHandle depth,
                             GpuHandle output, GpuHandle accum, uint32_t width, uint32_t height);

} // namespace numos::viewer
