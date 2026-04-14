// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_lvgl_anomaly.h"

#include <cstdio>
#include <execinfo.h>
#include <string>

#include "system/telemetry_manager.h"

namespace {
// Capture the caller's backtrace and format as a comma-separated list of hex PCs.
// The telemetry resolver pipeline (scripts/telemetry-crashes.py) can symbolize
// hex PCs against the binary's symbol cache the same way it resolves crash frames.
// Skip 2 frames: this helper + helix_lvgl_anomaly itself.
std::string capture_backtrace_hex() {
    constexpr int kMaxFrames = 24;
    constexpr int kSkipFrames = 2;
    void* frames[kMaxFrames];
    int n = ::backtrace(frames, kMaxFrames);
    std::string out;
    out.reserve(kMaxFrames * 16);
    for (int i = kSkipFrames; i < n; ++i) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s0x%lx", i == kSkipFrames ? "" : ",",
                      reinterpret_cast<unsigned long>(frames[i]));
        out += buf;
    }
    return out;
}
} // namespace

extern "C" void helix_lvgl_anomaly(const char* code, const char* context) {
    // Use "display" category — it's on the TelemetryManager allow-list and
    // semantically correct for LVGL/render-layer anomalies. Per-category rate
    // limit (1/5min) applies, so we share the budget with other display errors.
    std::string ctx;
    ctx.reserve(256);
    if (context && *context) {
        ctx = context;
        ctx += " | ";
    }
    ctx += "bt=";
    ctx += capture_backtrace_hex();
    TelemetryManager::instance().record_error("display", code ? code : "unknown_anomaly", ctx);
}
