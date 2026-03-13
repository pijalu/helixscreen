// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_mapper.h"
#include "ui_filament_mapping_modal.h"

#include <lvgl.h>

#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Compact filament mapping card for the print detail view
 *
 * Shows a compact row of color swatch pairs (gcode_color -> slot_color)
 * for each tool mapping. Tapping the card opens the FilamentMappingModal
 * for full interaction.
 *
 * Visibility: shown when AMS/toolchanger is detected AND the file
 * uses at least one tool. Hidden otherwise (falls back to nothing).
 */
class FilamentMappingCard {
  public:
    FilamentMappingCard() = default;
    ~FilamentMappingCard() = default;

    // Non-copyable (holds LVGL widget pointers)
    FilamentMappingCard(const FilamentMappingCard&) = delete;
    FilamentMappingCard& operator=(const FilamentMappingCard&) = delete;

    /**
     * @brief Attach to XML widgets after instantiation
     *
     * @param card_widget The filament_mapping_card ui_card
     * @param rows_container The filament_mapping_rows container (used for compact swatch row)
     * @param warning_container The filament_mapping_warning container
     */
    void create(lv_obj_t* card_widget, lv_obj_t* rows_container, lv_obj_t* warning_container);

    /**
     * @brief Update with new file data + current AMS state
     *
     * Shows the card if AMS is available and file has tools.
     * Computes default mappings via FilamentMapper::compute_defaults().
     *
     * @param gcode_colors Per-tool hex color strings (e.g., "#FF0000")
     * @param gcode_materials Per-tool material strings (e.g., "PLA")
     */
    void update(const std::vector<std::string>& gcode_colors,
                const std::vector<std::string>& gcode_materials);

    /**
     * @brief Hide the card
     */
    void hide();

    /**
     * @brief Get current tool-to-slot mappings
     */
    [[nodiscard]] std::vector<helix::ToolMapping> get_mappings() const { return mappings_; }

    /**
     * @brief Get per-tool gcode info (colors, materials)
     */
    [[nodiscard]] std::vector<helix::GcodeToolInfo> get_tool_info() const { return tool_info_; }

    /**
     * @brief Get per-tool mapped colors (RGB values from chosen slots)
     *
     * Returns a vector of uint32_t colors, one per tool. For auto/unmapped
     * tools, returns the gcode tool's original color.
     */
    [[nodiscard]] std::vector<uint32_t> get_mapped_colors() const;

    using MappingsChangedCallback = std::function<void()>;

    /**
     * @brief Register callback for when user changes mappings via the modal
     */
    void set_on_mappings_changed(MappingsChangedCallback cb) { on_mappings_changed_ = std::move(cb); }

    /**
     * @brief Check if any mappings have material mismatches
     */
    [[nodiscard]] bool has_mismatch() const;

    /**
     * @brief Check if card is currently visible
     */
    [[nodiscard]] bool is_visible() const;

    /**
     * @brief Null widget pointers (called during destroy-on-close)
     */
    void on_ui_destroyed();

  private:
    /// Build compact swatch pair row in rows_container_
    void rebuild_compact_view();

    /// Check if any mappings have material mismatches
    bool has_any_mismatch() const;

    /// Open the filament mapping modal
    void open_mapping_modal();

    /// Build AvailableSlot list from AmsState singleton
    std::vector<helix::AvailableSlot> collect_available_slots();

    /// Build GcodeToolInfo list from color/material strings
    std::vector<helix::GcodeToolInfo> build_tool_info(
        const std::vector<std::string>& colors,
        const std::vector<std::string>& materials);

    lv_obj_t* card_ = nullptr;
    lv_obj_t* rows_container_ = nullptr;
    lv_obj_t* warning_container_ = nullptr;

    std::vector<helix::ToolMapping> mappings_;
    std::vector<helix::GcodeToolInfo> tool_info_;
    std::vector<helix::AvailableSlot> available_slots_;

    FilamentMappingModal mapping_modal_;
    MappingsChangedCallback on_mappings_changed_;
};

} // namespace helix::ui
