// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_picker_modal.h"

#include "theme_manager.h"
#include "ui_fonts.h"
#include "ui_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Layout constants for picker rows (opacities are fixed visual properties)
constexpr lv_opa_t PICKER_SELECTED_BG_OPA = 40;
constexpr lv_opa_t PICKER_SWATCH_BORDER_OPA = 30;

// ============================================================================
// Configuration
// ============================================================================

void FilamentPickerModal::set_tool_info(int tool_index, uint32_t expected_color,
                                        const std::string& expected_material) {
    tool_index_ = tool_index;
    expected_color_ = expected_color;
    expected_material_ = expected_material;
}

void FilamentPickerModal::set_available_slots(const std::vector<helix::AvailableSlot>& slots) {
    slots_ = slots;
}

void FilamentPickerModal::set_current_selection(int slot_index, int backend_index) {
    current_selection_.slot_index = slot_index;
    current_selection_.backend_index = backend_index;
    current_selection_.is_auto = (slot_index < 0);
}

void FilamentPickerModal::set_on_select(SelectCallback cb) {
    on_select_cb_ = std::move(cb);
}

// ============================================================================
// Modal lifecycle
// ============================================================================

void FilamentPickerModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    // Update title to indicate which tool is being remapped
    lv_obj_t* title_widget = find_widget("header_title");
    if (title_widget) {
        char title_buf[64];
        if (expected_material_.empty()) {
            snprintf(title_buf, sizeof(title_buf), "T%d — %s", tool_index_,
                     lv_tr("Select Filament"));
        } else {
            snprintf(title_buf, sizeof(title_buf), "T%d %s — %s", tool_index_,
                     expected_material_.c_str(), lv_tr("Select Filament"));
        }
        lv_label_set_text(title_widget, title_buf);
    }

    // Add a color swatch next to the header icon to show the expected gcode color
    lv_obj_t* header_icon = find_widget("header_icon");
    if (header_icon) {
        lv_obj_t* parent = lv_obj_get_parent(header_icon);
        if (parent) {
            // Insert swatch after the icon
            lv_obj_t* swatch = lv_obj_create(parent);
            lv_obj_remove_style_all(swatch);
            lv_obj_set_size(swatch, theme_manager_get_spacing("space_xl"),
                           theme_manager_get_spacing("space_xl"));
            lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(swatch, lv_color_hex(expected_color_), 0);
            lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(swatch, 1, 0);
            lv_obj_set_style_border_color(swatch, theme_manager_get_color("text_muted"), 0);
            lv_obj_set_style_border_opa(swatch, PICKER_SWATCH_BORDER_OPA, 0);
            lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
            // Move after icon (index 1, icon is 0)
            lv_obj_move_to_index(swatch, 1);
        }
    }

    slot_list_ = find_widget("picker_slot_list");
    if (!slot_list_) {
        spdlog::error("[FilamentPicker] picker_slot_list not found in XML");
        return;
    }

    populate_slot_list();

    spdlog::debug("[FilamentPicker] Shown for tool T{} with {} available slots",
                  tool_index_, slots_.size());
}

void FilamentPickerModal::on_ok() {
    if (on_select_cb_) {
        on_select_cb_(current_selection_);
    }
    hide();
}

void FilamentPickerModal::on_cancel() {
    hide();
}

// ============================================================================
// Slot list population
// ============================================================================

void FilamentPickerModal::populate_slot_list() {
    if (!slot_list_) {
        return;
    }

    lv_obj_clean(slot_list_);

    // "Auto (best match)" row at top
    create_auto_row(slot_list_);

    // One row per available slot
    for (size_t i = 0; i < slots_.size(); ++i) {
        create_slot_row(slot_list_, static_cast<int>(i), slots_[i]);
    }

    update_row_highlights();
}

lv_obj_t* FilamentPickerModal::create_auto_row(lv_obj_t* parent) {
    int32_t row_pad = theme_manager_get_spacing("space_sm");
    int32_t row_gap = theme_manager_get_spacing("space_sm");
    int32_t swatch_sz = theme_manager_get_spacing("space_xl");
    int32_t row_radius = theme_manager_get_spacing("space_sm");

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, row_pad, 0);
    lv_obj_set_style_pad_gap(row, row_gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(row, row_radius, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Auto icon placeholder (a small "A" circle)
    lv_obj_t* swatch = lv_obj_create(row);
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, swatch_sz, swatch_sz);
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(swatch, theme_manager_get_color("primary"), 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* auto_label = lv_label_create(swatch);
    lv_label_set_text(auto_label, "A");
    lv_obj_center(auto_label);
    lv_obj_set_style_text_font(auto_label, theme_manager_get_font("font_small"), 0);
    lv_obj_set_style_text_color(auto_label, lv_color_white(), 0);

    // "Auto (best match)" text
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, lv_tr("Auto (best match)"));
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
    lv_obj_set_flex_grow(label, 1);

    // Click handler — use lv_obj_add_event_cb (dynamic content exception)
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* self = static_cast<FilamentPickerModal*>(lv_event_get_user_data(e));
            self->select_row(-1, -1, true);
        },
        LV_EVENT_CLICKED, this);

    // Store identification: use user data flag via obj name
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));

    return row;
}

lv_obj_t* FilamentPickerModal::create_slot_row(lv_obj_t* parent, int index,
                                                const helix::AvailableSlot& slot) {
    int32_t row_pad = theme_manager_get_spacing("space_sm");
    int32_t row_gap = theme_manager_get_spacing("space_sm");
    int32_t swatch_sz = theme_manager_get_spacing("space_xl");
    int32_t row_radius = theme_manager_get_spacing("space_sm");

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, row_pad, 0);
    lv_obj_set_style_pad_gap(row, row_gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(row, row_radius, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (slot.is_empty) {
        // Grayed out - not clickable
        lv_obj_set_style_opa(row, LV_OPA_40, 0);
    } else {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }

    // Color swatch
    create_color_swatch(row, slot.color_rgb, swatch_sz);

    // Slot label: "Slot N: Material" or "Turtle 1 · Slot N: Material"
    lv_obj_t* label = lv_label_create(row);
    std::string label_text = helix::FilamentMapper::format_slot_label(slot);
    lv_label_set_text(label, label_text.c_str());
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
    lv_obj_set_flex_grow(label, 1);

    // Material mismatch warning
    if (!slot.is_empty && !expected_material_.empty() && !slot.material.empty() &&
        !helix::FilamentMapper::materials_match(expected_material_, slot.material)) {
        lv_obj_t* warn_icon = lv_label_create(row);
        lv_label_set_text(warn_icon, ICON_TRIANGLE_EXCLAMATION);
        lv_obj_set_style_text_font(warn_icon, &mdi_icons_16, 0);
        lv_obj_set_style_text_color(warn_icon, theme_manager_get_color("warning"), 0);

        lv_obj_t* warn_text = lv_label_create(row);
        lv_label_set_text(warn_text, lv_tr("Incompatible"));
        lv_obj_set_style_text_font(warn_text, theme_manager_get_font("font_small"), 0);
        lv_obj_set_style_text_color(warn_text, theme_manager_get_color("warning"), 0);
    }

    // Click handler (dynamic content exception)
    if (!slot.is_empty) {
        // Store the index into the slots_ vector, we'll look up the actual slot from there
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentPickerModal*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int row_index =
                    static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                if (row_index >= 0 && row_index < static_cast<int>(self->slots_.size())) {
                    const auto& s = self->slots_[static_cast<size_t>(row_index)];
                    self->select_row(s.slot_index, s.backend_index, false);
                }
            },
            LV_EVENT_CLICKED, this);
    }

    // Store index for lookup
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    return row;
}

// ============================================================================
// Selection management
// ============================================================================

void FilamentPickerModal::select_row(int slot_index, int backend_index, bool is_auto) {
    current_selection_.slot_index = slot_index;
    current_selection_.backend_index = backend_index;
    current_selection_.is_auto = is_auto;
    update_row_highlights();

    spdlog::debug("[FilamentPicker] Selected: auto={}, slot={}, backend={}",
                  is_auto, slot_index, backend_index);
}

void FilamentPickerModal::update_row_highlights() {
    if (!slot_list_) {
        return;
    }

    uint32_t child_count = lv_obj_get_child_count(slot_list_);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* row = lv_obj_get_child(slot_list_, static_cast<int32_t>(i));
        if (!row) {
            continue;
        }

        bool is_selected = false;
        int row_data = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));

        if (current_selection_.is_auto && row_data == -1) {
            // Auto row
            is_selected = true;
        } else if (!current_selection_.is_auto && row_data >= 0 &&
                   row_data < static_cast<int>(slots_.size())) {
            const auto& slot = slots_[static_cast<size_t>(row_data)];
            is_selected = (slot.slot_index == current_selection_.slot_index &&
                           slot.backend_index == current_selection_.backend_index);
        }

        if (is_selected) {
            lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), 0);
            lv_obj_set_style_bg_opa(row, PICKER_SELECTED_BG_OPA, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, theme_manager_get_color("primary"), 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row, 0, 0);
        }
    }
}

// ============================================================================
// Helper: color swatch
// ============================================================================

lv_obj_t* FilamentPickerModal::create_color_swatch(lv_obj_t* parent, uint32_t color_rgb,
                                                    lv_coord_t size) {
    lv_obj_t* swatch = lv_obj_create(parent);
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, size, size);
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(swatch, lv_color_hex(color_rgb), 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(swatch, 1, 0);
    lv_obj_set_style_border_color(swatch, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_border_opa(swatch, PICKER_SWATCH_BORDER_OPA, 0);
    lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
    return swatch;
}

} // namespace helix::ui
