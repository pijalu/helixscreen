// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_lvgl_anomaly.h"

#include <cstdio>
#include <string>

// Stack trace support (glibc Linux / macOS only).
// Android NDK and musl libc (Creality K1) don't ship execinfo.h.
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__))
#include <execinfo.h>
#define HELIX_ANOMALY_HAS_BACKTRACE 1
#endif

#include "system/telemetry_manager.h"

namespace {
// Capture the caller's backtrace and format as a comma-separated list of hex PCs.
// The telemetry resolver pipeline (scripts/telemetry-crashes.py) can symbolize
// hex PCs against the binary's symbol cache the same way it resolves crash frames.
// Skip 2 frames: this helper + helix_lvgl_anomaly itself.
std::string capture_backtrace_hex() {
#ifdef HELIX_ANOMALY_HAS_BACKTRACE
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
#else
    return std::string{};
#endif
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
