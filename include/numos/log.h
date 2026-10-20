// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace numos {

enum class LogLevel { Info, Warn, Error };

// The library writes all diagnostics through this callback. Default: nothing is logged.
// `message` is UTF-8/ASCII without a trailing newline, valid only during the call.
// The callback is process-wide (one for every Renderer). Set it once, before other NUMOS calls: the
// setter is not synchronised with log() calls running on other threads. The callback runs on
// whichever thread called into NUMOS, so with Renderers or settings calls on several threads it
// must be thread-safe itself.
using LogCallback = void (*)(LogLevel level, const char* message, void* user);
void setLogCallback(LogCallback callback, void* user);

// Forwards to the callback (internal use, but harmless for hosts).
void log(LogLevel level, const char* message);

} // namespace numos
