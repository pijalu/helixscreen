// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_utils.h"

#include "theme_manager.h"

#include <cmath>

// ============================================================================
// Responsive Layout
// ============================================================================

lv_coord_t ui_get_header_content_padding(lv_coord_t screen_height) {
    (void)screen_height; // Parameter kept for API stability

    // Use unified space_* system - values are already responsive based on breakpoint
    // set during theme initialization (space_lg = 12/16/20px at small/medium/large)
    int32_t spacing = theme_manager_get_spacing("space_lg");

    // Fallback if theme not initialized (e.g., in unit tests)
    constexpr int32_t DEFAULT_SPACE_LG = 16; // Medium breakpoint value
    if (spacing == 0) {
        spacing = DEFAULT_SPACE_LG;
    }

    return spacing;
}

lv_coord_t ui_get_responsive_header_height(lv_coord_t screen_height) {
    // Responsive header heights for space efficiency:
    // Large/Medium (≥600px): 60px (comfortable)
    // Small (480-599px): 48px (compact)
    // Tiny (≤479px): 40px (minimal)

    if (screen_height >= UI_SCREEN_MEDIUM_H) {
        return 60;
    } else if (screen_height >= UI_SCREEN_SMALL_H) {
        return 48;
    } else {
        return 40;
    }
}

// ============================================================================
// LED Icon Utilities
// ============================================================================

const char* ui_brightness_to_lightbulb_icon(int brightness) {
    // Clamp to valid range
    if (brightness <= 0) {
        return "lightbulb_outline"; // OFF state
    }
    if (brightness < 15) {
        return "lightbulb_on_10";
    }
    if (brightness < 25) {
        return "lightbulb_on_20";
    }
    if (brightness < 35) {
        return "lightbulb_on_30";
    }
    if (brightness < 45) {
        return "lightbulb_on_40";
    }
    if (brightness < 55) {
        return "lightbulb_on_50";
    }
    if (brightness < 65) {
        return "lightbulb_on_60";
    }
    if (brightness < 75) {
        return "lightbulb_on_70";
    }
    if (brightness < 85) {
        return "lightbulb_on_80";
    }
    if (brightness < 95) {
        return "lightbulb_on_90";
    }
    return "lightbulb_on"; // 100%
}

// ============================================================================
// Color Utilities
// ============================================================================

int ui_color_distance(uint32_t color1, uint32_t color2) {
    int r1 = (color1 >> 16) & 0xFF;
    int g1 = (color1 >> 8) & 0xFF;
    int b1 = color1 & 0xFF;

    int r2 = (color2 >> 16) & 0xFF;
    int g2 = (color2 >> 8) & 0xFF;
    int b2 = color2 & 0xFF;

    // Weighted distance - green is most perceptible to human eye
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;

    // Weights: R=0.30, G=0.59, B=0.11 (standard luminance)
    // Squared for distance calculation, then sqrt
    int dist_sq = (dr * dr * 30 + dg * dg * 59 + db * db * 11) / 100;
    return static_cast<int>(std::sqrt(static_cast<double>(dist_sq)));
}
