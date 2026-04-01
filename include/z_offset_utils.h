// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_state.h" // ZOffsetCalibrationStrategy

#include <cstddef>
#include <functional>
#include <string>

class MoonrakerAPI;

namespace helix::zoffset {

/// Returns true and shows toast if strategy auto-persists (FIRMWARE_MANAGED).
/// Callers should return early when this returns true.
bool is_auto_saved(ZOffsetCalibrationStrategy strategy);

/// Format microns as "+0.050mm" or "-0.025mm". Empty string if microns == 0.
void format_delta(int microns, char* buf, size_t buf_size);

/// Format microns as "+0.050mm" (always shows value, even for 0).
void format_offset(int microns, char* buf, size_t buf_size);

/// Execute strategy-aware save sequence:
///   PROBE_CALIBRATE -> Z_OFFSET_APPLY_PROBE -> SAVE_CONFIG
///   ENDSTOP -> Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG
///   FIRMWARE_MANAGED -> no-op (firmware/macros auto-persist)
///
/// @param api           Moonraker API for gcode execution (must not be null)
/// @param strategy      Calibration strategy determining command sequence
/// @param on_success    Called after SAVE_CONFIG succeeds (Klipper will restart)
/// @param on_error      Called with user-facing message on any failure
void apply_and_save(MoonrakerAPI* api, ZOffsetCalibrationStrategy strategy,
                    std::function<void()> on_success,
                    std::function<void(const std::string& error)> on_error);

} // namespace helix::zoffset
