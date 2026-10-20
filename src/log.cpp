// Copyright 2026 Kim2091 (Sparkles)
// SPDX-License-Identifier: Apache-2.0

#include "numos/log.h"

namespace numos {

namespace {
LogCallback g_callback = nullptr;
void* g_user = nullptr;
} // namespace

void setLogCallback(LogCallback callback, void* user) {
  g_callback = callback;
  g_user = user;
}

void log(LogLevel level, const char* message) {
  if (g_callback) {
    g_callback(level, message, g_user);
  }
}

} // namespace numos
