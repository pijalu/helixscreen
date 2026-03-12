// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/// Unique identifier for an AMS slot across backends
using SlotKey = std::pair<int, int>; ///< (slot_index, backend_index)

/// Information about a G-code tool's expected filament
struct GcodeToolInfo {
    int tool_index;       ///< G-code tool number (0-based)
    uint32_t color_rgb;   ///< Expected color (0xRRGGBB)
    std::string material; ///< Expected material type ("PLA", "PETG", etc.)
};

/// Information about an available AMS slot.
/// This is an intentional abstraction boundary — callers convert from
/// ams_types.h SlotInfo to keep FilamentMapper free of LVGL dependency.
struct AvailableSlot {
    int slot_index;           ///< Slot index within its backend
    int backend_index;        ///< Which AMS backend (0 = primary)
    uint32_t color_rgb;       ///< Loaded filament color (0xRRGGBB)
    std::string material;     ///< Loaded material type
    bool is_empty;            ///< True if slot has no filament
    int current_tool_mapping; ///< What tool this slot is currently mapped to (-1 = none)

    /// Unique key for this slot across all backends
    SlotKey key() const { return {slot_index, backend_index}; }
};

/// Result of mapping a single tool
struct ToolMapping {
    int tool_index = -1;       ///< G-code tool number
    int mapped_slot = -1;      ///< AMS slot index (-1 = auto/unmapped)
    int mapped_backend = -1;   ///< Backend index (-1 = auto/primary)
    bool material_mismatch = false; ///< True if slot material != expected material
    bool is_auto = false;      ///< True if using "auto" (no explicit mapping)

    enum class MatchReason {
        FIRMWARE_MAPPING, ///< Matched via current firmware tool-to-slot mapping
        COLOR_MATCH,      ///< Matched by closest color
        AUTO,             ///< No explicit mapping, let firmware decide
    };
    MatchReason reason = MatchReason::AUTO;
};

/// Pure logic class for computing filament-to-tool mappings.
/// No LVGL dependency — takes data in, returns mappings out.
/// All methods are static and thread-safe.
class FilamentMapper {
public:
    /// Compute default mappings for all tools.
    /// Priority: firmware mapping -> color match -> auto
    static std::vector<ToolMapping> compute_defaults(
        const std::vector<GcodeToolInfo>& tools,
        const std::vector<AvailableSlot>& slots);

    /// Check if two colors are within matching tolerance.
    /// Uses weighted RGB distance (luminance-weighted) with tolerance of 40 units.
    static bool colors_match(uint32_t color_a, uint32_t color_b);

    /// Find the best matching slot for a given color.
    /// Returns the matching slot's key, or {-1, -1} if no match within tolerance.
    static SlotKey find_closest_color_slot(uint32_t target_color,
                                           const std::vector<AvailableSlot>& slots,
                                           const std::vector<SlotKey>& already_used);

    /// Find tool indices that have no resolved mapping (auto with no match).
    /// These are the tools that would trigger a color mismatch warning.
    static std::vector<int> find_unresolved_tools(const std::vector<ToolMapping>& mappings);

    /// Weighted RGB distance between two colors (luminance-weighted).
    /// Uses standard luminance coefficients: R=0.30, G=0.59, B=0.11.
    static int color_distance(uint32_t a, uint32_t b);

    /// Case-insensitive material comparison
    static bool materials_match(const std::string& a, const std::string& b);

    static constexpr int COLOR_MATCH_TOLERANCE = 40;
};

} // namespace helix
