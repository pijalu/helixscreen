// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_mapper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "lvgl/src/others/translation/lv_translation.h"

namespace helix {

std::string FilamentMapper::format_slot_label(const AvailableSlot& slot) {
    char buf[192];

    // Build material suffix
    const char* material_str = nullptr;
    if (slot.is_empty) {
        material_str = lv_tr("Empty");
    } else if (!slot.material.empty()) {
        material_str = slot.material.c_str();
    }

    if (slot.unit_display_name.empty()) {
        // Single-unit: "Slot 2" or "Slot 2: PLA"
        if (material_str) {
            snprintf(buf, sizeof(buf), "%s %d: %s",
                     lv_tr("Slot"), slot.slot_index + 1, material_str);
        } else {
            snprintf(buf, sizeof(buf), "%s %d",
                     lv_tr("Slot"), slot.slot_index + 1);
        }
    } else {
        // Multi-unit: "Turtle 1 · Slot 2" or "Turtle 1 · Slot 2: PLA"
        // unit_display_name is not translated — it's a user-configured AFC name
        if (material_str) {
            snprintf(buf, sizeof(buf), "%s \xc2\xb7 %s %d: %s",
                     slot.unit_display_name.c_str(),
                     lv_tr("Slot"), slot.slot_index + 1, material_str);
        } else {
            snprintf(buf, sizeof(buf), "%s \xc2\xb7 %s %d",
                     slot.unit_display_name.c_str(),
                     lv_tr("Slot"), slot.slot_index + 1);
        }
    }
    return buf;
}

int FilamentMapper::color_distance(uint32_t a, uint32_t b) {
    int r1 = (a >> 16) & 0xFF;
    int g1 = (a >> 8) & 0xFF;
    int b1 = a & 0xFF;

    int r2 = (b >> 16) & 0xFF;
    int g2 = (b >> 8) & 0xFF;
    int b2 = b & 0xFF;

    // Weighted RGB distance using standard luminance coefficients.
    // Weights: R=0.30, G=0.59, B=0.11 (standard luminance)
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;

    int dist_sq = (dr * dr * 30 + dg * dg * 59 + db * db * 11) / 100;
    return static_cast<int>(std::sqrt(static_cast<double>(dist_sq)));
}

bool FilamentMapper::colors_match(uint32_t color_a, uint32_t color_b) {
    return color_distance(color_a, color_b) <= COLOR_MATCH_TOLERANCE;
}

bool FilamentMapper::materials_match(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
        return std::tolower(static_cast<unsigned char>(ca)) ==
               std::tolower(static_cast<unsigned char>(cb));
    });
}

SlotKey FilamentMapper::find_closest_color_slot(uint32_t target_color,
                                                 const std::vector<AvailableSlot>& slots,
                                                 const std::vector<SlotKey>& already_used) {
    SlotKey best_key{-1, -1};
    int best_distance = COLOR_MATCH_TOLERANCE + 1; // Must be within tolerance

    for (const auto& slot : slots) {
        if (slot.is_empty) {
            continue;
        }

        // Skip slots already claimed by a higher-priority tool
        auto key = slot.key();
        if (std::find(already_used.begin(), already_used.end(), key) != already_used.end()) {
            continue;
        }

        int dist = color_distance(target_color, slot.color_rgb);
        if (dist < best_distance) {
            best_distance = dist;
            best_key = key;
        }
    }

    return best_key;
}

std::vector<ToolMapping> FilamentMapper::compute_defaults(
    const std::vector<GcodeToolInfo>& tools,
    const std::vector<AvailableSlot>& slots) {

    std::vector<ToolMapping> mappings;
    mappings.reserve(tools.size());

    // Track which slots have been claimed so that later tools don't steal
    // slots from earlier ones. Uses (slot_index, backend_index) pairs since
    // slot indices are only unique within a backend.
    std::vector<SlotKey> used_slots;

    for (const auto& tool : tools) {
        ToolMapping mapping;
        mapping.tool_index = tool.tool_index;

        // Priority 1: Check firmware mapping (slot already assigned to this tool)
        bool firmware_matched = false;
        for (const auto& slot : slots) {
            if (slot.is_empty) {
                continue;
            }
            if (slot.current_tool_mapping == tool.tool_index) {
                mapping.mapped_slot = slot.slot_index;
                mapping.mapped_backend = slot.backend_index;
                mapping.reason = ToolMapping::MatchReason::FIRMWARE_MAPPING;

                if (!tool.material.empty() && !slot.material.empty() &&
                    !materials_match(tool.material, slot.material)) {
                    mapping.material_mismatch = true;
                }

                used_slots.push_back(slot.key());
                firmware_matched = true;
                break;
            }
        }

        if (firmware_matched) {
            mappings.push_back(mapping);
            continue;
        }

        // Priority 2: Color match
        auto [slot_idx, backend_idx] = find_closest_color_slot(tool.color_rgb, slots, used_slots);
        if (slot_idx >= 0) {
            mapping.mapped_slot = slot_idx;
            mapping.mapped_backend = backend_idx;
            mapping.reason = ToolMapping::MatchReason::COLOR_MATCH;

            // Find the slot to check material compatibility
            for (const auto& slot : slots) {
                if (slot.slot_index == slot_idx && slot.backend_index == backend_idx) {
                    if (!tool.material.empty() && !slot.material.empty() &&
                        !materials_match(tool.material, slot.material)) {
                        mapping.material_mismatch = true;
                    }
                    break;
                }
            }

            used_slots.push_back({slot_idx, backend_idx});
            mappings.push_back(mapping);
            continue;
        }

        // Priority 3: Auto (no match found)
        mapping.is_auto = true;
        mappings.push_back(mapping);
    }

    return mappings;
}

std::vector<int> FilamentMapper::find_unresolved_tools(const std::vector<ToolMapping>& mappings) {
    std::vector<int> unresolved;
    for (const auto& m : mappings) {
        if (m.is_auto && m.reason == ToolMapping::MatchReason::AUTO) {
            unresolved.push_back(m.tool_index);
        }
    }
    return unresolved;
}

} // namespace helix
