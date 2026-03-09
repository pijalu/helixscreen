// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_split_button.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "sound_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>

using namespace helix;

namespace {

struct SplitButtonData {
    static constexpr uint32_t MAGIC = 0x53504C54; // "SPLT"
    uint32_t magic{MAGIC};
    lv_obj_t* main_btn{nullptr};
    lv_obj_t* arrow_btn{nullptr};
    lv_obj_t* dropdown{nullptr};
    lv_obj_t* icon{nullptr};
    lv_obj_t* label{nullptr};
    char* text_format{nullptr}; // owned, nullable
    bool show_selection{true};
};

SplitButtonData* get_data(lv_obj_t* sb) {
    if (!sb) return nullptr;
    auto* data = static_cast<SplitButtonData*>(lv_obj_get_user_data(sb));
    if (!data || data->magic != SplitButtonData::MAGIC) return nullptr;
    return data;
}

/**
 * @brief Get icon font for button icons (same approach as ui_button)
 */
const lv_font_t* get_button_icon_font() {
    static const lv_font_t* cached = nullptr;
    static bool resolved = false;
    if (resolved) return cached;

    const char* font_name = lv_xml_get_const_silent(nullptr, "icon_font_sm");
    const lv_font_t* font = nullptr;
    if (font_name) {
        font = lv_xml_get_font(nullptr, font_name);
    }
    if (!font) {
        font = &mdi_icons_24;
    }

    cached = font;
    resolved = true;
    return cached;
}

/**
 * @brief Update text contrast for inner widgets based on container background
 */
void update_split_button_contrast(lv_obj_t* sb) {
    auto* data = get_data(sb);
    if (!data) return;

    lv_opa_t bg_opa = lv_obj_get_style_bg_opa(sb, LV_PART_MAIN);
    bool is_ghost = bg_opa < LV_OPA_50;

    lv_color_t text_color;
    if (is_ghost) {
        text_color = theme_manager_get_color("text");
    } else {
        lv_color_t bg = lv_obj_get_style_bg_color(sb, LV_PART_MAIN);
        text_color = theme_manager_get_contrast_color(bg);
    }

    auto set_contrast = [&](lv_obj_t* obj) {
        if (!obj) return;
        lv_obj_set_style_text_color(obj, text_color, LV_PART_MAIN);
    };

    set_contrast(data->label);
    set_contrast(data->icon);

    // Update the arrow button's chevron icon
    if (data->arrow_btn) {
        lv_obj_t* arrow_icon = lv_obj_get_child(data->arrow_btn, 0);
        set_contrast(arrow_icon);
    }
}

void split_button_style_changed_cb(lv_event_t* e) {
    lv_obj_t* sb = lv_event_get_target_obj(e);
    update_split_button_contrast(sb);
}

/**
 * @brief Update the button label from current dropdown selection
 */
void update_label_from_selection(SplitButtonData* data) {
    if (!data || !data->label || !data->dropdown || !data->show_selection) return;

    char selected_text[128];
    lv_dropdown_get_selected_str(data->dropdown, selected_text, sizeof(selected_text));

    if (data->text_format) {
        char formatted[256];
        snprintf(formatted, sizeof(formatted), data->text_format, selected_text);
        lv_label_set_text(data->label, formatted);
    } else {
        lv_label_set_text(data->label, selected_text);
    }
}

/**
 * @brief Main button clicked — forward to container + play sound
 */
void main_btn_clicked_cb(lv_event_t* e) {
    lv_obj_t* main_btn = lv_event_get_target_obj(e);
    lv_obj_t* sb = lv_obj_get_parent(main_btn);
    SoundManager::instance().play("button_tap");
    lv_obj_send_event(sb, LV_EVENT_CLICKED, nullptr);
}

/**
 * @brief Arrow button clicked — open dropdown + play sound
 */
void arrow_btn_clicked_cb(lv_event_t* e) {
    lv_obj_t* arrow_btn = lv_event_get_target_obj(e);
    lv_obj_t* sb = lv_obj_get_parent(arrow_btn);
    auto* data = get_data(sb);
    if (!data || !data->dropdown) return;

    SoundManager::instance().play("button_tap");
    lv_dropdown_open(data->dropdown);
}

/**
 * @brief Dropdown value changed — update label + forward event to container
 */
void dropdown_value_changed_cb(lv_event_t* e) {
    lv_obj_t* dropdown = lv_event_get_target_obj(e);
    lv_obj_t* sb = lv_obj_get_parent(dropdown);
    auto* data = get_data(sb);
    if (!data) return;

    update_label_from_selection(data);
    lv_obj_send_event(sb, LV_EVENT_VALUE_CHANGED, nullptr);
}

/**
 * @brief Delete callback — clean up user data
 */
void split_button_delete_cb(lv_event_t* e) {
    lv_obj_t* sb = lv_event_get_target_obj(e);
    auto* data = get_data(sb);
    if (!data) return;

    lv_free(data->text_format);
    delete data;
    lv_obj_set_user_data(sb, nullptr);
}

/**
 * @brief Create an icon label inside a parent
 */
lv_obj_t* create_icon(lv_obj_t* parent, const char* icon_name) {
    if (!icon_name || strlen(icon_name) == 0) return nullptr;

    const char* codepoint = ui_icon::lookup_codepoint(icon_name);
    if (!codepoint) {
        const char* stripped = ui_icon::strip_legacy_prefix(icon_name);
        if (stripped != icon_name) {
            codepoint = ui_icon::lookup_codepoint(stripped);
        }
    }
    if (!codepoint) {
        spdlog::warn("[ui_split_button] Icon '{}' not found", icon_name);
        return nullptr;
    }

    lv_obj_t* icon = lv_label_create(parent);
    lv_label_set_text(icon, codepoint);
    lv_obj_set_style_text_font(icon, get_button_icon_font(), LV_PART_MAIN);
    return icon;
}

/**
 * @brief Apply variant style to the container (same logic as ui_button)
 */
lv_style_t* get_variant_style(const char* variant_str) {
    auto& tm = ThemeManager::instance();
    if (strcmp(variant_str, "secondary") == 0) return tm.get_style(StyleRole::ButtonSecondary);
    if (strcmp(variant_str, "danger") == 0) return tm.get_style(StyleRole::ButtonDanger);
    if (strcmp(variant_str, "success") == 0) return tm.get_style(StyleRole::ButtonSuccess);
    if (strcmp(variant_str, "tertiary") == 0) return tm.get_style(StyleRole::ButtonTertiary);
    if (strcmp(variant_str, "warning") == 0) return tm.get_style(StyleRole::ButtonWarning);
    if (strcmp(variant_str, "ghost") == 0) return tm.get_style(StyleRole::ButtonGhost);
    if (strcmp(variant_str, "outline") == 0) return tm.get_style(StyleRole::ButtonOutline);
    return tm.get_style(StyleRole::ButtonPrimary);
}

/**
 * @brief XML create callback for <ui_split_button>
 */
void* ui_split_button_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));

    // Create outer container (lv_obj, not lv_button — so we get raw obj styling)
    lv_obj_t* sb = lv_obj_create(parent);
    lv_obj_set_height(sb, theme_manager_get_spacing("button_height"));
    lv_obj_set_style_pad_all(sb, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

    // Apply variant style to container
    const char* variant_str = lv_xml_get_value_of(attrs, "variant");
    if (!variant_str) variant_str = "primary";

    lv_style_t* style = get_variant_style(variant_str);
    if (style) {
        lv_obj_add_style(sb, style, LV_PART_MAIN);
    }

    // Parse attributes
    const char* text = lv_xml_get_value_of(attrs, "text");
    if (!text) text = "";
    const char* icon_name = lv_xml_get_value_of(attrs, "icon");
    const char* options = lv_xml_get_value_of(attrs, "options");

    // Allocate user data
    auto* data = new SplitButtonData{};
    lv_obj_set_user_data(sb, data);

    // --- Main button (ghost, flex_grow=1) ---
    data->main_btn = lv_button_create(sb);
    lv_obj_set_style_bg_opa(data->main_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(data->main_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(data->main_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data->main_btn, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(data->main_btn, 1);
    lv_obj_set_height(data->main_btn, lv_pct(100));
    lv_obj_set_flex_flow(data->main_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(data->main_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(data->main_btn, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);
    lv_obj_set_style_pad_left(data->main_btn, theme_manager_get_spacing("space_sm"), LV_PART_MAIN);

    // Icon (optional)
    if (icon_name && strlen(icon_name) > 0) {
        data->icon = create_icon(data->main_btn, icon_name);
    }

    // Label
    data->label = lv_label_create(data->main_btn);
    lv_label_set_text(data->label, text);

    // Main button click handler
    lv_obj_add_event_cb(data->main_btn, main_btn_clicked_cb, LV_EVENT_CLICKED, nullptr);

    // --- Divider ---
    lv_obj_t* divider = lv_obj_create(sb);
    lv_obj_set_size(divider, 1, lv_pct(60));
    lv_obj_set_style_bg_opa(divider, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(divider, theme_manager_get_contrast_color(
        lv_obj_get_style_bg_color(sb, LV_PART_MAIN)), LV_PART_MAIN);
    lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    // --- Arrow button (ghost, fixed width ~40px) ---
    data->arrow_btn = lv_button_create(sb);
    lv_obj_set_style_bg_opa(data->arrow_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(data->arrow_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(data->arrow_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data->arrow_btn, 0, LV_PART_MAIN);
    lv_obj_set_size(data->arrow_btn, 40, lv_pct(100));
    lv_obj_set_flex_flow(data->arrow_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(data->arrow_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Chevron down icon
    lv_obj_t* chevron = lv_label_create(data->arrow_btn);
    const char* chevron_cp = ui_icon::lookup_codepoint("chevron_down");
    if (chevron_cp) {
        lv_label_set_text(chevron, chevron_cp);
    }
    lv_obj_set_style_text_font(chevron, get_button_icon_font(), LV_PART_MAIN);

    // Arrow button click handler
    lv_obj_add_event_cb(data->arrow_btn, arrow_btn_clicked_cb, LV_EVENT_CLICKED, nullptr);

    // --- Hidden dropdown (zero size, for popup list) ---
    data->dropdown = lv_dropdown_create(sb);
    lv_obj_set_size(data->dropdown, 0, 0);
    lv_obj_set_style_opa(data->dropdown, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(data->dropdown, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data->dropdown, 0, LV_PART_MAIN);
    lv_obj_remove_flag(data->dropdown, LV_OBJ_FLAG_CLICKABLE);

    if (options && strlen(options) > 0) {
        lv_dropdown_set_options(data->dropdown, options);
    }

    // Dropdown value changed handler
    lv_obj_add_event_cb(data->dropdown, dropdown_value_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Register event handlers on container
    lv_obj_add_event_cb(sb, split_button_style_changed_cb, LV_EVENT_STYLE_CHANGED, nullptr);
    lv_obj_add_event_cb(sb, split_button_style_changed_cb, LV_EVENT_STATE_CHANGED, nullptr);
    lv_obj_add_event_cb(sb, split_button_delete_cb, LV_EVENT_DELETE, nullptr);

    // Apply initial contrast
    update_split_button_contrast(sb);

    spdlog::trace("[ui_split_button] Created variant='{}' text='{}' icon='{}' options='{}'",
                  variant_str, text, icon_name ? icon_name : "", options ? options : "");

    return sb;
}

/**
 * @brief XML apply callback for <ui_split_button>
 */
void ui_split_button_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);

    void* item = lv_xml_state_get_item(state);
    lv_obj_t* sb = static_cast<lv_obj_t*>(item);
    auto* data = get_data(sb);
    if (!data) return;

    // Handle text_format attribute
    const char* text_format = lv_xml_get_value_of(attrs, "text_format");
    if (text_format && strlen(text_format) > 0) {
        lv_free(data->text_format);
        data->text_format = lv_strdup(text_format);
    }

    // Handle show_selection attribute
    const char* show_sel = lv_xml_get_value_of(attrs, "show_selection");
    if (show_sel) {
        data->show_selection = (strcmp(show_sel, "false") != 0);
    }

    // Handle selected attribute
    const char* selected = lv_xml_get_value_of(attrs, "selected");
    if (selected && data->dropdown) {
        uint32_t idx = static_cast<uint32_t>(atoi(selected));
        lv_dropdown_set_selected(data->dropdown, idx);
    }

    // Update label from selection if show_selection is active
    if (data->show_selection && data->text_format) {
        update_label_from_selection(data);
    }
}

} // namespace

void ui_split_button_init() {
    lv_xml_register_widget("ui_split_button", ui_split_button_create, ui_split_button_apply);
    spdlog::trace("[ui_split_button] Registered split button widget");
}

void ui_split_button_set_options(lv_obj_t* sb, const char* options) {
    auto* data = get_data(sb);
    if (!data || !data->dropdown || !options) return;
    lv_dropdown_set_options(data->dropdown, options);
    update_label_from_selection(data);
}

void ui_split_button_set_selected(lv_obj_t* sb, uint32_t index) {
    auto* data = get_data(sb);
    if (!data || !data->dropdown) return;
    lv_dropdown_set_selected(data->dropdown, index);
    update_label_from_selection(data);
}

uint32_t ui_split_button_get_selected(lv_obj_t* sb) {
    auto* data = get_data(sb);
    if (!data || !data->dropdown) return 0;
    return lv_dropdown_get_selected(data->dropdown);
}

void ui_split_button_set_text(lv_obj_t* sb, const char* text) {
    auto* data = get_data(sb);
    if (!data || !data->label || !text) return;
    lv_label_set_text(data->label, text);
    lv_obj_invalidate(sb);
}
