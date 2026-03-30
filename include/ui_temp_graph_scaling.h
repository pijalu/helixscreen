// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_temp_graph_scaling.h
 * @brief Dynamic Y-axis scaling for temperature graphs
 *
 * Provides hysteresis-based scaling to prevent oscillation when temps hover
 * near thresholds. Expands eagerly (at 90% of max) and shrinks conservatively
 * (at 60% of previous step).
 */

/**
 * @brief Calculate the optimal Y-axis maximum for a temperature graph
 *
 * Implements dynamic scaling with hysteresis:
 * - Expands when nozzle_temp > 80% of current_max (in 50°C steps up to 300°C)
 * - Shrinks when max(nozzle, bed) < 60% of (current_max - 50) (down to 150°C minimum)
 *
 * @param current_max Current Y-axis maximum (typically 150-300°C)
 * @param nozzle_temp Current nozzle temperature in °C
 * @param bed_temp Current bed temperature in °C
 * @return New Y-axis maximum (unchanged if no scaling needed)
 *
 * @note The asymmetric thresholds (80% expand, 60% shrink) create a dead zone
 *       that prevents rapid oscillation when temps hover near a boundary.
 */
inline float calculate_mini_graph_y_max(float current_max, float nozzle_temp, float bed_temp) {
    constexpr float Y_MAX_MIN = 150.0f;      // Minimum Y-axis max (good for room temp visibility)
    constexpr float Y_MAX_MAX = 300.0f;      // Maximum Y-axis max (covers highest nozzle temps)
    constexpr float Y_STEP = 50.0f;          // Step size for scaling
    constexpr float EXPAND_THRESHOLD = 0.8f; // Expand at 80% of current max (leaves ~20% headroom)
    constexpr float SHRINK_THRESHOLD = 0.6f; // Shrink at 60% of previous step

    float max_temp = (nozzle_temp > bed_temp) ? nozzle_temp : bed_temp;

    // Expand: if nozzle approaching current max
    if (nozzle_temp > current_max * EXPAND_THRESHOLD && current_max < Y_MAX_MAX) {
        float new_max = current_max + Y_STEP;
        return (new_max < Y_MAX_MAX) ? new_max : Y_MAX_MAX;
    }

    // Shrink: if both temps are well below previous step
    float shrink_threshold_temp = (current_max - Y_STEP) * SHRINK_THRESHOLD;
    if (max_temp < shrink_threshold_temp && current_max > Y_MAX_MIN) {
        float new_max = current_max - Y_STEP;
        return (new_max > Y_MAX_MIN) ? new_max : Y_MAX_MIN;
    }

    return current_max;
}
