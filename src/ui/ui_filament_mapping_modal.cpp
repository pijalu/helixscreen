// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_modal.h"

#include "ui_fonts.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "theme_manager.h"

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

    auto_color_map_ = SettingsManager::instance().get_auto_color_map();

    // Snapshot so Cancel can revert
    original_mappings_ = mappings_;

    tool_list_ = find_widget("mapping_tool_list");
    if (!tool_list_) {
        spdlog::warn("[FilamentMappingModal] Could not find mapping_tool_list widget");
        return;
    }

    rebuild_rows();

    spdlog::debug("[FilamentMappingModal] Shown with {} tools, {} slots, auto_color_map={}",
                  tool_info_.size(), available_slots_.size(), auto_color_map_);
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
    toggle_switch_ = nullptr;

    trigger_widgets_.clear();
    trigger_widgets_.resize(mappings_.size(), nullptr);

    // Toggle row first, then tool rows
    create_toggle_row();

    for (size_t i = 0; i < mappings_.size(); ++i) {
        create_tool_row(static_cast<int>(i));
    }
}

lv_obj_t* FilamentMappingModal::create_tool_row(int tool_index) {
    if (!tool_list_ || tool_index < 0 || tool_index >= static_cast<int>(mappings_.size()) ||
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

    // Entire row is tappable — layout: [T0] [gcode_swatch] → [chosen_swatch] [slot_text] [warn?]
    // [>]
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

    // Gcode material label (after swatch, before arrow)
    if (!tool.material.empty()) {
        lv_obj_t* mat_label = lv_label_create(row);
        lv_label_set_text(mat_label, tool.material.c_str());
        lv_obj_set_style_text_font(mat_label, theme_manager_get_font("font_body"), 0);
        lv_obj_set_style_text_color(mat_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_remove_flag(mat_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(mat_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Arrow: gcode → chosen
    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, ICON_ARROW_RIGHT);
    lv_obj_set_style_text_font(arrow, &mdi_icons_24, 0);
    lv_obj_set_style_text_color(arrow, theme_manager_get_color("text_muted"), 0);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Look up the mapped slot once for color and warning checks
    const auto* mapped = find_mapped_slot(mapping);
    uint32_t slot_color = mapped ? mapped->color_rgb : 0x808080;

    // Dropdown trigger container
    int32_t trigger_radius = theme_manager_get_spacing("space_sm");
    int32_t trigger_pad_h = theme_manager_get_spacing("space_sm");
    int32_t trigger_pad_v = theme_manager_get_spacing("space_xs");
    int32_t trigger_gap = theme_manager_get_spacing("space_sm");

    lv_obj_t* trigger = lv_obj_create(row);
    lv_obj_remove_style_all(trigger);
    lv_obj_set_height(trigger, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(trigger, 1);
    lv_obj_set_flex_flow(trigger, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(trigger, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_hor(trigger, trigger_pad_h, 0);
    lv_obj_set_style_pad_ver(trigger, trigger_pad_v, 0);
    lv_obj_set_style_pad_gap(trigger, trigger_gap, 0);
    lv_obj_set_style_radius(trigger, trigger_radius, 0);
    lv_obj_set_style_bg_color(trigger, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(trigger, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(trigger, 1, 0);
    lv_obj_set_style_border_color(trigger, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_border_opa(trigger, SWATCH_BORDER_OPA, 0);
    lv_obj_remove_flag(trigger, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(trigger, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(trigger, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Store trigger for picker anchoring
    trigger_widgets_[static_cast<size_t>(tool_index)] = trigger;

    // Slot color swatch (inside trigger)
    if (mapped && !mapped->is_empty) {
        lv_obj_t* chosen_swatch = lv_obj_create(trigger);
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
    } else if (mapped && mapped->is_empty) {
        // Empty slot: hollow swatch with warning border
        lv_obj_t* chosen_swatch = lv_obj_create(trigger);
        lv_obj_remove_style_all(chosen_swatch);
        lv_obj_set_size(chosen_swatch, swatch_sz, swatch_sz);
        lv_obj_set_style_radius(chosen_swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chosen_swatch, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chosen_swatch, 2, 0);
        lv_obj_set_style_border_color(chosen_swatch, theme_manager_get_color("warning"), 0);
        lv_obj_set_style_border_opa(chosen_swatch, LV_OPA_COVER, 0);
        lv_obj_remove_flag(chosen_swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(chosen_swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(chosen_swatch, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    // No swatch for Auto/Unmapped (mapped == nullptr)

    // Slot text label (inside trigger, fills remaining space)
    lv_obj_t* slot_text = lv_label_create(trigger);
    std::string display = get_slot_display_text(mapping);
    lv_label_set_text(slot_text, display.c_str());
    lv_obj_set_style_text_font(slot_text, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(slot_text, theme_manager_get_color("text"), 0);
    lv_obj_set_flex_grow(slot_text, 1);
    lv_label_set_long_mode(slot_text, LV_LABEL_LONG_DOT);
    lv_obj_remove_flag(slot_text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(slot_text, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Warning indicator (inside trigger, before chevron)
    bool mapped_to_empty = mapped && mapped->is_empty;
    if (mapping.material_mismatch || mapped_to_empty) {
        lv_obj_t* warn = lv_label_create(trigger);
        lv_label_set_text(warn, ICON_TRIANGLE_EXCLAMATION);
        lv_obj_set_style_text_font(warn, &mdi_icons_16, 0);
        lv_obj_set_style_text_color(warn, theme_manager_get_color("warning"), 0);
        lv_obj_remove_flag(warn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(warn, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Chevron (inside trigger, rightmost — hidden in auto-color mode)
    if (!auto_color_map_) {
        lv_obj_t* chevron = lv_label_create(trigger);
        lv_label_set_text(chevron, ICON_CHEVRON_DOWN);
        lv_obj_set_style_text_font(chevron, &mdi_icons_16, 0);
        lv_obj_set_style_text_color(chevron, theme_manager_get_color("text_muted"), 0);
        lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(chevron, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    if (auto_color_map_) {
        // Auto color mode: dim rows, mapper handles assignments
        lv_obj_set_style_opa(row, LV_OPA_50, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    } else {
        // Manual mode: rows are interactive for reassignment
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentMappingModal*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int idx =
                    static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                self->on_row_tapped(idx);
            },
            LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(tool_index)));
    }

    return row;
}

// ============================================================================
// Display text helpers
// ============================================================================

const helix::AvailableSlot*
FilamentMappingModal::find_mapped_slot(const helix::ToolMapping& mapping) const {
    if (mapping.is_auto || mapping.mapped_slot < 0) {
        return nullptr;
    }
    for (const auto& slot : available_slots_) {
        if (slot.slot_index == mapping.mapped_slot &&
            slot.backend_index == mapping.mapped_backend) {
            return &slot;
        }
    }
    return nullptr;
}

std::string FilamentMappingModal::get_slot_display_text(const helix::ToolMapping& mapping) const {
    if (mapping.is_auto) {
        return lv_tr("Auto");
    }

    if (mapping.mapped_slot < 0) {
        return lv_tr("Unmapped");
    }

    const auto* slot = find_mapped_slot(mapping);
    if (slot) {
        return helix::FilamentMapper::format_slot_label(*slot);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d", lv_tr("Slot"), mapping.mapped_slot + 1);
    return buf;
}

// ============================================================================
// Row tap -> slot picker
// ============================================================================

void FilamentMappingModal::on_row_tapped(int tool_index) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tool_info_.size())) {
        return;
    }

    const auto& tool = tool_info_[static_cast<size_t>(tool_index)];
    const auto& mapping = mappings_[static_cast<size_t>(tool_index)];

    spdlog::debug("[FilamentMappingModal] Row tapped: T{}", tool_index);

    FilamentSlotPicker::Selection current{mapping.mapped_slot, mapping.mapped_backend,
                                          mapping.is_auto};

    lv_obj_t* trigger = trigger_widgets_[static_cast<size_t>(tool_index)];

    slot_picker_.show(lv_screen_active(), trigger, tool_index, tool.material, available_slots_,
                      current, [this, tool_index](const FilamentSlotPicker::Selection& sel) {
                          on_slot_selected(tool_index, sel);
                      });
}

void FilamentMappingModal::on_slot_selected(int tool_index,
                                            const FilamentSlotPicker::Selection& selection) {
    if (tool_index < 0 || tool_index >= static_cast<int>(mappings_.size())) {
        return;
    }

    auto& mapping = mappings_[static_cast<size_t>(tool_index)];
    mapping.mapped_slot = selection.slot_index;
    mapping.mapped_backend = selection.backend_index;
    mapping.is_auto = selection.is_auto;

    mapping.material_mismatch = false;
    if (selection.is_auto) {
        mapping.reason = helix::ToolMapping::MatchReason::AUTO;
    } else {
        const auto& tool = tool_info_[static_cast<size_t>(tool_index)];
        const auto* slot = find_mapped_slot(mapping);
        if (slot && !tool.material.empty() && !slot->material.empty() &&
            !helix::FilamentMapper::materials_match(tool.material, slot->material)) {
            mapping.material_mismatch = true;
        }
    }

    spdlog::info("[FilamentMappingModal] T{} mapped to: auto={}, slot={}, backend={}", tool_index,
                 selection.is_auto, selection.slot_index, selection.backend_index);

    // Rebuild UI to reflect changes
    rebuild_rows();
}

// ============================================================================
// Toggle: keep current assignments
// ============================================================================

void FilamentMappingModal::create_toggle_row() {
    if (!tool_list_) {
        return;
    }

    int32_t row_pad_v = theme_manager_get_spacing("space_sm");

    // Row container
    lv_obj_t* row = lv_obj_create(tool_list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(row, row_pad_v, 0);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Label
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, lv_tr("Map to closest colors with matching material"));
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);

    // Switch
    toggle_switch_ = lv_switch_create(row);
    if (auto_color_map_) {
        lv_obj_add_state(toggle_switch_, LV_STATE_CHECKED);
    }

    lv_obj_add_event_cb(
        toggle_switch_,
        [](lv_event_t* e) {
            auto* self = static_cast<FilamentMappingModal*>(lv_event_get_user_data(e));
            bool checked = lv_obj_has_state(self->toggle_switch_, LV_STATE_CHECKED);
            self->on_toggle_changed(checked);
        },
        LV_EVENT_VALUE_CHANGED, this);
}

void FilamentMappingModal::on_toggle_changed(bool auto_color) {
    auto_color_map_ = auto_color;
    SettingsManager::instance().set_auto_color_map(auto_color);
    recalculate_mappings();
    rebuild_rows();
}

void FilamentMappingModal::recalculate_mappings() {
    if (auto_color_map_) {
        // Color matching: clear firmware mappings so they don't override color matches
        auto slots_for_matching = available_slots_;
        for (auto& s : slots_for_matching) {
            s.current_tool_mapping = -1;
        }
        mappings_ = helix::FilamentMapper::compute_defaults(tool_info_, slots_for_matching);
    } else {
        // Positional assignment (T0→slot 0, T1→slot 1, etc.)
        mappings_ = helix::FilamentMapper::use_current_assignments(tool_info_, available_slots_);
    }
}

} // namespace helix::ui
