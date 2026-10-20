@echo off
rem Copyright 2026 Kim2091 (Sparkles)
rem SPDX-License-Identifier: Apache-2.0
rem
rem Configure and build NUMOS on Windows with the newest Visual Studio C++ toolchain (CMake and Ninja
rem as shipped with Visual Studio), then run the tests. Vulkan is found through the VULKAN_SDK
rem environment variable that the LunarG Vulkan SDK installer sets.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio or the Build Tools & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (echo No Visual Studio installation with the C++ tools found & exit /b 1)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
cd /d "%~dp0.."
if not exist build\cmake\build.ninja (
  cmake -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build build\cmake -j 4 || exit /b 1
ctest --test-dir build\cmake --output-on-failure
