// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_breakpoint.h
 * @brief Responsive breakpoint definitions used across the UI
 *
 * Single source of truth for breakpoint enums and screen-height thresholds.
 * Included by grid_layout.h, theme_manager.h, and any code that needs to
 * reason about responsive breakpoints without pulling in the full theme system.
 */

#pragma once

#include <cstdint>

/// Breakpoint tiers — used everywhere instead of magic integers.
/// Selected based on screen height (vertical resolution).
/// Values match the legacy UiBreakpointIndex enum for LVGL subject compatibility.
enum class UiBreakpoint : int32_t {
    Micro = 0,  // height ≤ 272  — 480x272
    Tiny = 1,   // height ≤ 390  — 480x320
    Small = 2,  // height ≤ 460  — 480x400, 1920x440
    Medium = 3, // height ≤ 550  — 800x480
    Large = 4,  // height ≤ 700  — 1024x600
    XLarge = 5, // height > 700  — 1280x720+
};

/// Screen height thresholds (max height inclusive for each breakpoint)
#define UI_BREAKPOINT_MICRO_MAX 272  // height ≤272 → MICRO (480x272)
#define UI_BREAKPOINT_TINY_MAX 390   // height 273-390 → TINY (480x320)
#define UI_BREAKPOINT_SMALL_MAX 460  // height 391-460 → SMALL (480x400, 1920x440)
#define UI_BREAKPOINT_MEDIUM_MAX 550 // height 461-550 → MEDIUM (800x480)
#define UI_BREAKPOINT_LARGE_MAX                                                                    \
    700 // height 551-700 → LARGE (1024x600)
        // height >700 → XLARGE (1280x720+)

/// Convert a UiBreakpoint enum to its underlying integer index (for array access).
constexpr inline int32_t to_int(UiBreakpoint bp) {
    return static_cast<int32_t>(bp);
}

/// Convert a raw integer to a UiBreakpoint (clamped to valid range [Micro, XLarge]).
/// Safe for use with lv_subject_get_int() results from the ui_breakpoint subject.
inline UiBreakpoint as_breakpoint(int32_t raw) {
    constexpr int32_t min = to_int(UiBreakpoint::Micro);
    constexpr int32_t max = to_int(UiBreakpoint::XLarge);

    raw = std::clamp(raw, min, max);
    return static_cast<UiBreakpoint>(raw);
}
