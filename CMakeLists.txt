# Copyright 2026 Kim2091 (Sparkles)
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(numos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(NUMOS_BUILD_TESTS "Build NUMOS tests" ON)
option(NUMOS_BUILD_VULKAN "Build the reference Vulkan backend and the viewer" ON)

# Core library: CPU side only, no third-party dependencies.
add_library(numos STATIC
  src/layout.cpp
  src/log.cpp
  src/settings.cpp
  src/shaders.cpp
  src/celestial.cpp
  src/renderer.cpp
  src/renderer_api.cpp
  src/weather.cpp
  src/seed.cpp
  src/ground_field.cpp
)
target_include_directories(numos
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Shared C++/Slang headers (numos/lib/*.h, numos/util/*.h).
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
  target_compile_options(numos PRIVATE /W4 /permissive-)
  target_compile_definitions(numos PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(numos PRIVATE -Wall -Wextra)
endif()

if(NUMOS_BUILD_TESTS)
  enable_testing()
  add_executable(numos_tests tests/main.cpp)
  target_link_libraries(numos_tests PRIVATE numos)
  target_compile_definitions(numos_tests PRIVATE NUMOS_TEST_SPIRV_DIR="${CMAKE_CURRENT_SOURCE_DIR}/build/spirv")
  add_test(NAME numos_tests COMMAND numos_tests)
endif()

if(NUMOS_BUILD_VULKAN)
  find_package(Vulkan REQUIRED)
  add_library(numos_vulkan STATIC backends/vulkan/numos_vulkan.cpp)
  target_include_directories(numos_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/backends/vulkan)
  target_link_libraries(numos_vulkan PUBLIC numos Vulkan::Vulkan)

  add_executable(numos_viewer viewer/main.cpp viewer/viewer_shared.cpp)
  target_include_directories(numos_viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/viewer)
  target_link_libraries(numos_viewer PRIVATE numos_vulkan)

  # The interactive viewer (numos_viewer --interactive): a window through GLFW and a settings UI
  # through Dear ImGui, both git submodules under external/. Only the viewer links them.
  set(NUMOS_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
  set(NUMOS_GLFW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw)
  if(EXISTS ${NUMOS_IMGUI_DIR}/imgui.cpp AND EXISTS ${NUMOS_GLFW_DIR}/CMakeLists.txt)
    set(NUMOS_VIEWER_UI_DEFAULT ON)
  else()
    set(NUMOS_VIEWER_UI_DEFAULT OFF)
  endif()
  option(NUMOS_BUILD_VIEWER_UI "Build the viewer's interactive window (needs the external/imgui and external/glfw submodules)"
         ${NUMOS_VIEWER_UI_DEFAULT})

  if(NUMOS_BUILD_VIEWER_UI)
    if(NOT EXISTS ${NUMOS_IMGUI_DIR}/imgui.cpp OR NOT EXISTS ${NUMOS_GLFW_DIR}/CMakeLists.txt)
      message(FATAL_ERROR "NUMOS_BUILD_VIEWER_UI needs the submodules: git submodule update --init external/imgui external/glfw")
    endif()
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${NUMOS_GLFW_DIR} ${CMAKE_CURRENT_BINARY_DIR}/external/glfw EXCLUDE_FROM_ALL)

    add_library(numos_imgui STATIC
      ${NUMOS_IMGUI_DIR}/imgui.cpp
      ${NUMOS_IMGUI_DIR}/imgui_draw.cpp
      ${NUMOS_IMGUI_DIR}/imgui_tables.cpp
      ${NUMOS_IMGUI_DIR}/imgui_widgets.cpp
      ${NUMOS_IMGUI_DIR}/backends/imgui_impl_glfw.cpp
      ${NUMOS_IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(numos_imgui PUBLIC ${NUMOS_IMGUI_DIR} ${NUMOS_IMGUI_DIR}/backends)
    target_link_libraries(numos_imgui PUBLIC glfw Vulkan::Vulkan)

    target_sources(numos_viewer PRIVATE
      viewer/app_main.cpp
      viewer/app_window.cpp
      viewer/app_camera.cpp
      viewer/app_ui.cpp
      viewer/app_settings_ui.cpp
    )
    target_compile_definitions(numos_viewer PRIVATE NUMOS_VIEWER_UI=1)
    target_link_libraries(numos_viewer PRIVATE numos_imgui)
  endif()
endif()
