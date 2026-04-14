// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_lvgl_anomaly.h"

#include "system/telemetry_manager.h"

extern "C" void helix_lvgl_anomaly(const char* code, const char* context) {
    TelemetryManager::instance().record_error("lvgl_anomaly", code ? code : "unknown",
                                              context ? context : "");
}
