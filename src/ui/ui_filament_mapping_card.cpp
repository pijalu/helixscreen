// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_card.h"

#include "ams_state.h"
#include "color_utils.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "ui_fonts.h"
#include "ui_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Layout constants for the compact swatch pair display.
// Swatches are built in C++ (not XML components) because the number varies
// per file. lv_obj_add_event_cb and lv_label_set_text are used here
// as allowed exceptions for dynamic content.
namespace {
constexpr lv_opa_t SWATCH_BORDER_OPA = 30;
} // namespace

// ============================================================================
// Setup
// ============================================================================

void FilamentMappingCard::create(lv_obj_t* card_widget, lv_obj_t* rows_container,
                                  lv_obj_t* warning_container) {
    card_ = card_widget;
    rows_container_ = rows_container;
    warning_container_ = warning_container;

    // Make the entire card tappable to open the mapping modal
    if (card_) {
        lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            card_,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentMappingCard*>(lv_event_get_user_data(e));
                self->open_mapping_modal();
            },
            LV_EVENT_CLICKED, this);
    }

    spdlog::debug("[FilamentMapping] Card created");
}

// ============================================================================
// Update / visibility
// ============================================================================

void FilamentMappingCard::update(const std::vector<std::string>& gcode_colors,
                                  const std::vector<std::string>& gcode_materials) {
    if (!card_ || !rows_container_) {
        return;
    }

    // Check if AMS is available
    auto& ams = AmsState::instance();
    if (!ams.is_available()) {
        hide();
        return;
    }

    // Build tool info from file metadata
    tool_info_ = build_tool_info(gcode_colors, gcode_materials);

    if (tool_info_.empty()) {
        hide();
        return;
    }

    // Collect available slots from AMS backends
    available_slots_ = collect_available_slots();

    // Compute mappings based on user preference
    if (SettingsManager::instance().get_auto_color_map()) {
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

    // Build the compact UI
    rebuild_compact_view();

    // Show the card
    lv_obj_remove_flag(card_, LV_OBJ_FLAG_HIDDEN);

    spdlog::debug("[FilamentMapping] Updated: {} tools, {} slots, {} mappings",
                  tool_info_.size(), available_slots_.size(), mappings_.size());
}

void FilamentMappingCard::hide() {
    if (card_) {
        lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool FilamentMappingCard::has_mismatch() const {
    return has_any_mismatch();
}

bool FilamentMappingCard::is_visible() const {
    if (!card_) {
        return false;
    }
    return !lv_obj_has_flag(card_, LV_OBJ_FLAG_HIDDEN);
}

void FilamentMappingCard::on_ui_destroyed() {
    card_ = nullptr;
    rows_container_ = nullptr;
    warning_container_ = nullptr;
}

// ============================================================================
// Compact swatch pair view
// ============================================================================

void FilamentMappingCard::rebuild_compact_view() {
    if (!rows_container_) {
        return;
    }

    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_clean(rows_container_);

    // Responsive spacing from design tokens
    int32_t swatch_sz = theme_manager_get_spacing("space_md");
    int32_t inner_gap = theme_manager_get_spacing("space_xxs");
    int32_t pair_gap = theme_manager_get_spacing("space_xs");
    int32_t pill_pad_h = theme_manager_get_spacing("space_xs");
    int32_t pill_pad_v = theme_manager_get_spacing("space_xxs");
    int32_t pill_radius = theme_manager_get_spacing("space_lg");

    // Configure as a horizontal flex row of swatch pairs (wrapping)
    lv_obj_set_flex_flow(rows_container_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_flex_cross_place(rows_container_, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_gap(rows_container_, pair_gap, 0);

    size_t count = std::min(mappings_.size(), tool_info_.size());
    bool multi_tool = count > 1;
    for (size_t i = 0; i < count; ++i) {
        const auto& mapping = mappings_[i];
        const auto& tool = tool_info_[i];

        // Create a pill container: [Tx] [gcode_color] → [slot_color]
        lv_obj_t* pair = lv_obj_create(rows_container_);
        lv_obj_remove_style_all(pair);
        lv_obj_set_size(pair, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(pair, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_gap(pair, inner_gap, 0);
        lv_obj_set_style_pad_left(pair, pill_pad_h, 0);
        lv_obj_set_style_pad_right(pair, pill_pad_h, 0);
        lv_obj_set_style_pad_top(pair, pill_pad_v, 0);
        lv_obj_set_style_pad_bottom(pair, pill_pad_v, 0);
        lv_obj_set_style_radius(pair, pill_radius, 0);
        lv_obj_set_style_bg_color(pair, theme_manager_get_color("elevated_bg"), 0);
        lv_obj_set_style_bg_opa(pair, LV_OPA_COVER, 0);
        lv_obj_remove_flag(pair, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(pair, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(pair, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Tool label (e.g. "T0", "T1") — only for multi-tool files
        if (multi_tool) {
            lv_obj_t* tool_lbl = lv_label_create(pair);
            lv_label_set_text_fmt(tool_lbl, "T%d", tool.tool_index);
            lv_obj_set_style_text_font(tool_lbl, theme_manager_get_font("font_xs"), 0);
            lv_obj_set_style_text_color(tool_lbl, theme_manager_get_color("text_muted"), 0);
            lv_obj_remove_flag(tool_lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(tool_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        // G-code color swatch
        lv_obj_t* gcode_sw = lv_obj_create(pair);
        lv_obj_remove_style_all(gcode_sw);
        lv_obj_set_size(gcode_sw, swatch_sz, swatch_sz);
        lv_obj_set_style_radius(gcode_sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(gcode_sw, lv_color_hex(tool.color_rgb), 0);
        lv_obj_set_style_bg_opa(gcode_sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(gcode_sw, 1, 0);
        lv_obj_set_style_border_color(gcode_sw, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_border_opa(gcode_sw, SWATCH_BORDER_OPA, 0);
        lv_obj_remove_flag(gcode_sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(gcode_sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(gcode_sw, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Arrow
        lv_obj_t* arrow = lv_label_create(pair);
        lv_label_set_text(arrow, ICON_ARROW_RIGHT);
        lv_obj_set_style_text_font(arrow, theme_manager_get_font("icon_font_xs"), 0);
        lv_obj_set_style_text_color(arrow, theme_manager_get_color("text_muted"), 0);
        lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(arrow, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Slot color swatch
        uint32_t slot_color = 0x808080;
        bool slot_empty = false;
        if (!mapping.is_auto && mapping.mapped_slot >= 0) {
            for (const auto& s : available_slots_) {
                if (s.slot_index == mapping.mapped_slot &&
                    s.backend_index == mapping.mapped_backend) {
                    slot_color = s.color_rgb;
                    slot_empty = s.is_empty;
                    break;
                }
            }
        }
        lv_obj_t* slot_sw = lv_obj_create(pair);
        lv_obj_remove_style_all(slot_sw);
        lv_obj_set_size(slot_sw, swatch_sz, swatch_sz);
        lv_obj_set_style_radius(slot_sw, LV_RADIUS_CIRCLE, 0);
        if (slot_empty) {
            // Empty slot: transparent fill with warning border
            lv_obj_set_style_bg_opa(slot_sw, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(slot_sw, 2, 0);
            lv_obj_set_style_border_color(slot_sw, theme_manager_get_color("warning"), 0);
            lv_obj_set_style_border_opa(slot_sw, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(slot_sw, lv_color_hex(slot_color), 0);
            lv_obj_set_style_bg_opa(slot_sw, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(slot_sw, 1, 0);
            lv_obj_set_style_border_color(slot_sw, theme_manager_get_color("text_muted"), 0);
            lv_obj_set_style_border_opa(slot_sw, SWATCH_BORDER_OPA, 0);
        }
        lv_obj_remove_flag(slot_sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(slot_sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(slot_sw, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Warning icon visibility is handled by XML bind_flag_if_eq on "filament_mismatch" subject
}

bool FilamentMappingCard::has_any_mismatch() const {
    for (const auto& m : mappings_) {
        if (m.material_mismatch) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Color queries
// ============================================================================

std::vector<uint32_t> FilamentMappingCard::get_mapped_colors() const {
    std::vector<uint32_t> colors;
    colors.reserve(mappings_.size());

    for (size_t i = 0; i < mappings_.size(); ++i) {
        const auto& mapping = mappings_[i];

        if (!mapping.is_auto && mapping.mapped_slot >= 0) {
            // Look up chosen slot color
            uint32_t slot_color = (i < tool_info_.size()) ? tool_info_[i].color_rgb : 0x808080;
            for (const auto& s : available_slots_) {
                if (s.slot_index == mapping.mapped_slot &&
                    s.backend_index == mapping.mapped_backend) {
                    slot_color = s.color_rgb;
                    break;
                }
            }
            colors.push_back(slot_color);
        } else {
            // Auto or unmapped: use gcode tool's original color
            colors.push_back((i < tool_info_.size()) ? tool_info_[i].color_rgb : 0x808080);
        }
    }

    return colors;
}

// ============================================================================
// Modal interaction
// ============================================================================

void FilamentMappingCard::open_mapping_modal() {
    spdlog::debug("[FilamentMapping] Opening mapping modal");

    mapping_modal_.set_tool_info(tool_info_);
    mapping_modal_.set_available_slots(available_slots_);
    mapping_modal_.set_mappings(mappings_);
    mapping_modal_.set_on_mappings_updated([this](auto mappings) {
        mappings_ = std::move(mappings);
        rebuild_compact_view();
        if (on_mappings_changed_) {
            on_mappings_changed_();
        }
    });
    mapping_modal_.show(lv_screen_active());
}

// ============================================================================
// Data collection
// ============================================================================

std::vector<helix::AvailableSlot> FilamentMappingCard::collect_available_slots() {
    std::vector<helix::AvailableSlot> slots;
    auto& ams = AmsState::instance();

    for (int bi = 0; bi < ams.backend_count(); ++bi) {
        auto* backend = ams.get_backend(bi);
        if (!backend) {
            continue;
        }

        auto info = backend->get_system_info();
        bool multi_unit = info.units.size() > 1;

        for (const auto& unit : info.units) {
            for (const auto& slot_info : unit.slots) {
                helix::AvailableSlot as;
                as.slot_index = slot_info.global_index;
                as.local_slot_index = slot_info.slot_index;
                as.backend_index = bi;
                as.color_rgb = slot_info.color_rgb;
                as.material = slot_info.material;
                as.is_empty = (slot_info.status == SlotStatus::EMPTY ||
                               slot_info.status == SlotStatus::UNKNOWN);
                as.current_tool_mapping = slot_info.mapped_tool;
                as.unit_index = unit.unit_index;
                if (multi_unit) {
                    as.unit_display_name = unit.display_name.empty()
                                               ? unit.name
                                               : unit.display_name;
                }
                slots.push_back(std::move(as));
            }
        }
    }

    spdlog::debug("[FilamentMapping] Collected {} available slots from {} backends",
                  slots.size(), ams.backend_count());
    return slots;
}

std::vector<helix::GcodeToolInfo> FilamentMappingCard::build_tool_info(
    const std::vector<std::string>& colors,
    const std::vector<std::string>& materials) {
    std::vector<helix::GcodeToolInfo> tools;

    // Use the larger of colors or materials to determine tool count.
    // If both are empty, return empty — the card will be hidden.
    size_t count = std::max(colors.size(), materials.size());
    if (count == 0) {
        return tools;
    }

    for (size_t i = 0; i < count; ++i) {
        helix::GcodeToolInfo tool;
        tool.tool_index = static_cast<int>(i);

        // Parse color
        if (i < colors.size() && !colors[i].empty()) {
            auto parsed = helix::parse_hex_color(colors[i]);
            tool.color_rgb = parsed.value_or(0x808080);
        } else {
            tool.color_rgb = 0x808080;
        }

        // Material
        if (i < materials.size()) {
            tool.material = materials[i];
        }

        tools.push_back(std::move(tool));
    }

    return tools;
}

} // namespace helix::ui
