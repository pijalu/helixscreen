// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_modal.h"

#include "theme_manager.h"
#include "ui_fonts.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Layout constants for dynamically created rows.
// Rows are built in C++ (not XML components) because the number of rows
// varies per file. lv_obj_add_event_cb and lv_label_set_text are used here
// as allowed exceptions for dynamic content.
namespace {
constexpr int TOOL_LABEL_MIN_W = 32;
constexpr lv_opa_t SWATCH_BORDER_OPA = 30;
} // namespace

// ============================================================================
// Configuration
// ============================================================================

void FilamentMappingModal::set_tool_info(const std::vector<helix::GcodeToolInfo>& tools) {
    tool_info_ = tools;
}

void FilamentMappingModal::set_available_slots(const std::vector<helix::AvailableSlot>& slots) {
    available_slots_ = slots;
}

void FilamentMappingModal::set_mappings(std::vector<helix::ToolMapping> mappings) {
    mappings_ = std::move(mappings);
}

void FilamentMappingModal::set_on_mappings_updated(MappingsUpdatedCallback cb) {
    on_updated_cb_ = std::move(cb);
}

// ============================================================================
// Modal lifecycle
// ============================================================================

void FilamentMappingModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    // Snapshot so Cancel can revert
    original_mappings_ = mappings_;

    tool_list_ = find_widget("mapping_tool_list");
    if (!tool_list_) {
        spdlog::warn("[FilamentMappingModal] Could not find mapping_tool_list widget");
        return;
    }

    rebuild_rows();

    spdlog::debug("[FilamentMappingModal] Shown with {} tools, {} slots",
                  tool_info_.size(), available_slots_.size());
}

void FilamentMappingModal::on_ok() {
    if (on_updated_cb_) {
        on_updated_cb_(mappings_);
    }
    hide();
}

void FilamentMappingModal::on_cancel() {
    mappings_ = original_mappings_;
    hide();
}

// ============================================================================
// Row building
// ============================================================================

void FilamentMappingModal::rebuild_rows() {
    if (!tool_list_) {
        return;
    }

    lv_obj_clean(tool_list_);

    for (size_t i = 0; i < mappings_.size(); ++i) {
        create_tool_row(static_cast<int>(i));
    }
}

lv_obj_t* FilamentMappingModal::create_tool_row(int tool_index) {
    if (!tool_list_ || tool_index < 0 ||
        tool_index >= static_cast<int>(mappings_.size()) ||
        tool_index >= static_cast<int>(tool_info_.size())) {
        return nullptr;
    }

    const auto& mapping = mappings_[static_cast<size_t>(tool_index)];
    const auto& tool = tool_info_[static_cast<size_t>(tool_index)];

    // Responsive spacing from design tokens
    int32_t row_pad = theme_manager_get_spacing("space_md");
    int32_t row_gap = theme_manager_get_spacing("space_md");
    int32_t swatch_sz = theme_manager_get_spacing("space_xl");
    int32_t row_radius = theme_manager_get_spacing("space_sm");

    // Entire row is tappable — layout: [T0] [gcode_swatch] → [chosen_swatch] [slot_text] [warn?] [>]
    lv_obj_t* row = lv_obj_create(tool_list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, row_pad, 0);
    lv_obj_set_style_pad_gap(row, row_gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(row, row_radius, 0);
    lv_obj_set_style_bg_color(row, theme_manager_get_color("elevated_bg"), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Tool label (multi-tool only)
    if (tool_info_.size() > 1) {
        lv_obj_t* tool_label = lv_label_create(row);
        char tool_buf[8];
        snprintf(tool_buf, sizeof(tool_buf), "T%d", tool_index);
        lv_label_set_text(tool_label, tool_buf);
        lv_obj_set_style_text_font(tool_label, theme_manager_get_font("font_body"), 0);
        lv_obj_set_style_text_color(tool_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_min_width(tool_label, TOOL_LABEL_MIN_W, 0);
        lv_obj_remove_flag(tool_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(tool_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Left swatch: G-code expected color
    lv_obj_t* expected_swatch = lv_obj_create(row);
    lv_obj_remove_style_all(expected_swatch);
    lv_obj_set_size(expected_swatch, swatch_sz, swatch_sz);
    lv_obj_set_style_radius(expected_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(expected_swatch, lv_color_hex(tool.color_rgb), 0);
    lv_obj_set_style_bg_opa(expected_swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(expected_swatch, 1, 0);
    lv_obj_set_style_border_color(expected_swatch, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_border_opa(expected_swatch, SWATCH_BORDER_OPA, 0);
    lv_obj_remove_flag(expected_swatch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(expected_swatch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(expected_swatch, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Arrow: gcode → chosen
    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, ICON_ARROW_RIGHT);
    lv_obj_set_style_text_font(arrow, &mdi_icons_24, 0);
    lv_obj_set_style_text_color(arrow, theme_manager_get_color("text_muted"), 0);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Right swatch: chosen slot color
    uint32_t slot_color = 0x808080;
    if (!mapping.is_auto && mapping.mapped_slot >= 0) {
        for (const auto& s : available_slots_) {
            if (s.slot_index == mapping.mapped_slot &&
                s.backend_index == mapping.mapped_backend) {
                slot_color = s.color_rgb;
                break;
            }
        }
    }
    lv_obj_t* chosen_swatch = lv_obj_create(row);
    lv_obj_remove_style_all(chosen_swatch);
    lv_obj_set_size(chosen_swatch, swatch_sz, swatch_sz);
    lv_obj_set_style_radius(chosen_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chosen_swatch, lv_color_hex(slot_color), 0);
    lv_obj_set_style_bg_opa(chosen_swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chosen_swatch, 1, 0);
    lv_obj_set_style_border_color(chosen_swatch, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_border_opa(chosen_swatch, SWATCH_BORDER_OPA, 0);
    lv_obj_remove_flag(chosen_swatch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(chosen_swatch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chosen_swatch, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Slot text (fills remaining space)
    lv_obj_t* slot_text = lv_label_create(row);
    std::string display = get_slot_display_text(mapping);
    lv_label_set_text(slot_text, display.c_str());
    lv_obj_set_style_text_font(slot_text, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(slot_text, theme_manager_get_color("text"), 0);
    lv_obj_set_flex_grow(slot_text, 1);
    lv_obj_remove_flag(slot_text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(slot_text, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Material mismatch indicator
    if (mapping.material_mismatch) {
        lv_obj_t* warn = lv_label_create(row);
        lv_label_set_text(warn, ICON_TRIANGLE_EXCLAMATION);
        lv_obj_set_style_text_font(warn, &mdi_icons_24, 0);
        lv_obj_set_style_text_color(warn, theme_manager_get_color("warning"), 0);
        lv_obj_remove_flag(warn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(warn, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Chevron
    lv_obj_t* chevron = lv_label_create(row);
    lv_label_set_text(chevron, ICON_CHEVRON_RIGHT);
    lv_obj_set_style_text_font(chevron, &mdi_icons_24, 0);
    lv_obj_set_style_text_color(chevron, theme_manager_get_color("text_muted"), 0);
    lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chevron, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Click handler (dynamic content exception — rows are rebuilt on each update)
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* self = static_cast<FilamentMappingModal*>(lv_event_get_user_data(e));
            lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
            self->on_row_tapped(idx);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(tool_index)));

    return row;
}

// ============================================================================
// Display text helpers
// ============================================================================

std::string FilamentMappingModal::get_slot_display_text(const helix::ToolMapping& mapping) const {
    if (mapping.is_auto) {
        return lv_tr("Auto");
    }

    if (mapping.mapped_slot < 0) {
        return lv_tr("Unmapped");
    }

    for (const auto& slot : available_slots_) {
        if (slot.slot_index == mapping.mapped_slot &&
            slot.backend_index == mapping.mapped_backend) {
            return helix::FilamentMapper::format_slot_label(slot);
        }
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d", lv_tr("Slot"), mapping.mapped_slot + 1);
    return buf;
}

// ============================================================================
// Row tap -> picker modal
// ============================================================================

void FilamentMappingModal::on_row_tapped(int tool_index) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tool_info_.size())) {
        return;
    }

    const auto& tool = tool_info_[static_cast<size_t>(tool_index)];
    const auto& mapping = mappings_[static_cast<size_t>(tool_index)];

    spdlog::debug("[FilamentMappingModal] Row tapped: T{}", tool_index);

    picker_modal_.set_tool_info(tool_index, tool.color_rgb, tool.material);
    picker_modal_.set_available_slots(available_slots_);
    picker_modal_.set_current_selection(mapping.mapped_slot, mapping.mapped_backend);
    picker_modal_.set_on_select([this, tool_index](const FilamentPickerModal::Selection& sel) {
        on_slot_selected(tool_index, sel);
    });
    picker_modal_.show(lv_screen_active());
}

void FilamentMappingModal::on_slot_selected(int tool_index,
                                             const FilamentPickerModal::Selection& selection) {
    if (tool_index < 0 || tool_index >= static_cast<int>(mappings_.size())) {
        return;
    }

    auto& mapping = mappings_[static_cast<size_t>(tool_index)];
    mapping.mapped_slot = selection.slot_index;
    mapping.mapped_backend = selection.backend_index;
    mapping.is_auto = selection.is_auto;

    if (selection.is_auto) {
        mapping.reason = helix::ToolMapping::MatchReason::AUTO;
        mapping.material_mismatch = false;
    } else {
        // Check material mismatch
        mapping.material_mismatch = false;
        const auto& tool = tool_info_[static_cast<size_t>(tool_index)];
        if (!tool.material.empty()) {
            for (const auto& slot : available_slots_) {
                if (slot.slot_index == selection.slot_index &&
                    slot.backend_index == selection.backend_index) {
                    if (!slot.material.empty() &&
                        !helix::FilamentMapper::materials_match(tool.material, slot.material)) {
                        mapping.material_mismatch = true;
                    }
                    break;
                }
            }
        }
    }

    spdlog::info("[FilamentMappingModal] T{} mapped to: auto={}, slot={}, backend={}",
                 tool_index, selection.is_auto, selection.slot_index, selection.backend_index);

    // Rebuild UI to reflect changes
    rebuild_rows();
}

} // namespace helix::ui
