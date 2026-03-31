// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_notification_stub.cpp
 * @brief Stub implementations of UI notification functions for CLI tools
 *
 * Tools like moonraker-inspector need moonraker_client which references
 * UI notification functions. These stubs prevent linking errors while
 * allowing the CLI tool to run without LVGL/UI dependencies.
 */

#include <cstdio>

// Stub implementations - just ignore notifications for CLI tools
// Note: These must be C++ functions (not extern "C") to match the C++ declarations
void ui_notification_error(const char*, const char*, bool) {
    // Silently ignore - this is a CLI tool
}

void ui_notification_warning(const char*) {
    // Silently ignore - this is a CLI tool
}
