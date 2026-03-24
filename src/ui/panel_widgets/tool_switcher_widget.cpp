// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_switcher_widget.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_utils.h"

#include <spdlog/spdlog.h>

namespace helix {

// Static instance for event callback routing
ToolSwitcherWidget* ToolSwitcherWidget::s_active_instance = nullptr;

/// Resolve a responsive spacing token to pixels, with a fallback.
static int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

void register_tool_switcher_widget() {
    register_widget_factory("tool_switcher", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<ToolSwitcherWidget>(ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "tool_pill_cb", ToolSwitcherWidget::tool_pill_cb);
    lv_xml_register_event_cb(nullptr, "tool_compact_cb", ToolSwitcherWidget::tool_compact_cb);
}

ToolSwitcherWidget::ToolSwitcherWidget(PrinterState& printer_state)
    : printer_state_(printer_state) {}

ToolSwitcherWidget::~ToolSwitcherWidget() {
    *alive_ = false;
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
}

void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;
    s_active_instance = this;

    auto& tool_state = ToolState::instance();
    std::weak_ptr<bool> weak_alive = alive_;

    // Observe active tool changes
    active_tool_observer_ = helix::ui::observe_int_sync<ToolSwitcherWidget>(
        tool_state.get_active_tool_subject(), this,
        [weak_alive](ToolSwitcherWidget* self, int tool) {
            if (weak_alive.expired()) return;
            self->on_active_tool_changed(tool);
        });

    // Observe tool count changes to trigger rebuild
    tool_count_observer_ = helix::ui::observe_int_sync<ToolSwitcherWidget>(
        tool_state.get_tool_count_subject(), this,
        [weak_alive](ToolSwitcherWidget* self, int /*count*/) {
            if (weak_alive.expired()) return;
            if (self->current_colspan_ == 1 && self->current_rowspan_ == 1) {
                self->rebuild_compact();
            } else {
                self->rebuild_pills();
            }
        });

    // Initial build based on current size
    if (current_colspan_ == 1 && current_rowspan_ == 1) {
        rebuild_compact();
    } else {
        rebuild_pills();
    }
}

void ToolSwitcherWidget::detach() {
    *alive_ = false;
    dismiss_tool_picker();
    active_tool_observer_.reset();
    tool_count_observer_.reset();
    pill_buttons_.clear();
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ToolSwitcherWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                         int /*height_px*/) {
    current_colspan_ = colspan;
    current_rowspan_ = rowspan;

    if (!widget_obj_) return;

    if (colspan == 1 && rowspan == 1) {
        rebuild_compact();
    } else {
        rebuild_pills();
    }
}

// ============================================================================
// Pill buttons (inline mode for 1x2, 2x1, 2x2, etc.)
// ============================================================================

void ToolSwitcherWidget::rebuild_pills() {
    if (!widget_obj_) return;

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for pill rebuild");
        return;
    }

    pill_buttons_.clear();
    lv_obj_clean(container);

    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    int active = tool_state.active_tool_index();

    if (tools.empty()) {
        spdlog::debug("[ToolSwitcher] No tools available for pill rebuild");
        return;
    }

    int space_xs = resolve_space_token("space_xs", 4);

    // Choose flex direction based on widget shape
    // 1x2 (tall) = column, otherwise row
    if (current_colspan_ == 1 && current_rowspan_ >= 2) {
        lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    } else {
        lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    }
    lv_obj_set_style_pad_gap(container, space_xs, 0);

    lv_color_t accent_color = theme_manager_get_color("accent");
    lv_color_t card_bg = theme_manager_get_color("card_bg");
    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t text_inv = theme_manager_get_color("text_inverse");

    for (size_t i = 0; i < tools.size(); ++i) {
        lv_obj_t* btn = lv_button_create(container);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(btn, 32, 0);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_pad_ver(btn, 4, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);

        bool is_active = (static_cast<int>(i) == active);

        if (is_active) {
            lv_obj_set_style_bg_color(btn, accent_color, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(btn, card_bg, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        }

        // Pressed state feedback
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_PART_MAIN | LV_STATE_PRESSED);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, tools[i].name.c_str());
        lv_obj_set_style_text_color(label, is_active ? text_inv : text_color, 0);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        lv_obj_center(label);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store tool index in user_data for click handler
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] pill_click");
                if (!s_active_instance) return;
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                s_active_instance->handle_tool_selected(idx);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);

        pill_buttons_.push_back(btn);
    }

    spdlog::debug("[ToolSwitcher] Built {} pill buttons, active={}", tools.size(), active);
}

void ToolSwitcherWidget::on_active_tool_changed(int tool_index) {
    if (pill_buttons_.empty()) {
        // In compact mode, update the label
        if (widget_obj_ && current_colspan_ == 1 && current_rowspan_ == 1) {
            rebuild_compact();
        }
        return;
    }

    lv_color_t accent_color = theme_manager_get_color("accent");
    lv_color_t card_bg = theme_manager_get_color("card_bg");
    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t text_inv = theme_manager_get_color("text_inverse");

    for (size_t i = 0; i < pill_buttons_.size(); ++i) {
        lv_obj_t* btn = pill_buttons_[i];
        if (!lv_obj_is_valid(btn)) continue;

        bool is_active = (static_cast<int>(i) == tool_index);

        lv_obj_set_style_bg_color(btn, is_active ? accent_color : card_bg, 0);

        // Update label color
        if (lv_obj_get_child_count(btn) > 0) {
            lv_obj_t* label = lv_obj_get_child(btn, 0);
            if (label) {
                lv_obj_set_style_text_color(label, is_active ? text_inv : text_color, 0);
            }
        }
    }

    spdlog::debug("[ToolSwitcher] Active tool changed to T{}", tool_index);
}

// ============================================================================
// Compact mode (1x1 — single label + picker popup)
// ============================================================================

void ToolSwitcherWidget::rebuild_compact() {
    if (!widget_obj_) return;

    lv_obj_t* container = lv_obj_find_by_name(widget_obj_, "tool_switcher_container");
    if (!container) {
        spdlog::warn("[ToolSwitcher] Container not found for compact rebuild");
        return;
    }

    pill_buttons_.clear();
    lv_obj_clean(container);

    auto& tool_state = ToolState::instance();
    int active = tool_state.active_tool_index();
    const auto& tools = tool_state.tools();

    // Set container clickable for compact mode
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);

    // Current tool label centered
    lv_obj_t* label = lv_label_create(container);
    std::string tool_name = (active >= 0 && active < static_cast<int>(tools.size()))
                                ? tools[active].name
                                : "T?";
    lv_label_set_text(label, tool_name.c_str());
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
    lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Click opens picker
    lv_obj_add_event_cb(
        container,
        [](lv_event_t* /*e*/) {
            LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] compact_click");
            if (s_active_instance) {
                s_active_instance->show_tool_picker();
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    spdlog::debug("[ToolSwitcher] Built compact mode, active=T{}", active);
}

// ============================================================================
// Tool picker popup (for compact mode)
// ============================================================================

void ToolSwitcherWidget::show_tool_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    auto& tool_state = ToolState::instance();
    const auto& tools = tool_state.tools();
    int active = tool_state.active_tool_index();

    if (tools.empty()) return;

    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);
    int space_md = resolve_space_token("space_md", 10);

    int screen_w = lv_obj_get_width(parent_screen_);
    int screen_h = lv_obj_get_height(parent_screen_);

    // Backdrop (full screen, transparent, catches clicks to dismiss)
    picker_backdrop_ = lv_obj_create(parent_screen_);
    lv_obj_set_size(picker_backdrop_, screen_w, screen_h);
    lv_obj_set_pos(picker_backdrop_, 0, 0);
    lv_obj_set_style_bg_color(picker_backdrop_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(picker_backdrop_, LV_OPA_50, 0);
    lv_obj_set_style_border_width(picker_backdrop_, 0, 0);
    lv_obj_set_style_radius(picker_backdrop_, 0, 0);
    lv_obj_remove_flag(picker_backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(picker_backdrop_, LV_OBJ_FLAG_CLICKABLE);

    // Backdrop click dismisses picker
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* /*e*/) {
            LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] backdrop_cb");
            if (s_active_instance) {
                s_active_instance->dismiss_tool_picker();
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    // Card container
    lv_obj_t* card = lv_obj_create(picker_backdrop_);
    int card_w = std::clamp(screen_w * 50 / 100, 180, 320);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, screen_h * 70 / 100, 0);
    lv_obj_set_style_bg_color(card, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, theme_manager_get_color("border"), 0);
    lv_obj_set_style_pad_all(card, space_md, 0);
    lv_obj_set_style_pad_gap(card, space_sm, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); // Prevent clicks passing through
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Select Tool");
    lv_obj_set_style_text_font(title, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(title, theme_manager_get_color("text"), 0);
    lv_obj_set_width(title, LV_PCT(100));

    // Grid of tool buttons
    // Use grid layout: 3 columns for 6+ tools, 2 for 3-5, 1 for 1-2
    int cols = (tools.size() >= 6) ? 3 : (tools.size() >= 3) ? 2 : 1;

    lv_obj_t* grid = lv_obj_create(card);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grid, space_xs, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_bg_opa(grid, 0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    // Calculate button width based on grid columns
    // Account for gaps: (cols-1) * space_xs
    int available_w = card_w - 2 * space_md;
    int btn_w = (available_w - (cols - 1) * space_xs) / cols;

    lv_color_t accent_color = theme_manager_get_color("accent");
    lv_color_t surface_color = theme_manager_get_color("card_bg");
    lv_color_t text_color = theme_manager_get_color("text");
    lv_color_t text_inv = theme_manager_get_color("text_inverse");

    for (size_t i = 0; i < tools.size(); ++i) {
        lv_obj_t* btn = lv_button_create(grid);
        lv_obj_set_width(btn, btn_w);
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(btn, 36, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_pad_ver(btn, space_sm, 0);
        lv_obj_set_style_pad_hor(btn, space_xs, 0);

        bool is_active = (static_cast<int>(i) == active);

        if (is_active) {
            lv_obj_set_style_bg_color(btn, accent_color, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(btn, surface_color, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        }

        lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_PART_MAIN | LV_STATE_PRESSED);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, tools[i].name.c_str());
        lv_obj_set_style_text_color(label, is_active ? text_inv : text_color, 0);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        lv_obj_center(label);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store tool index in user_data
        lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_obj_add_event_cb(
            btn,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] picker_tool_click");
                if (!s_active_instance) return;
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int idx =
                    static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                s_active_instance->dismiss_tool_picker();
                s_active_instance->handle_tool_selected(idx);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    spdlog::debug("[ToolSwitcher] Picker shown with {} tools", tools.size());
}

void ToolSwitcherWidget::dismiss_tool_picker() {
    if (!picker_backdrop_) return;

    lv_obj_t* backdrop = picker_backdrop_;
    picker_backdrop_ = nullptr;

    if (lv_obj_is_valid(backdrop)) {
        helix::ui::safe_delete(backdrop);
    }

    spdlog::debug("[ToolSwitcher] Picker dismissed");
}

// ============================================================================
// Tool selection with safety gate
// ============================================================================

void ToolSwitcherWidget::handle_tool_selected(int tool_index) {
    auto& tool_state = ToolState::instance();

    // Already on this tool
    if (tool_index == tool_state.active_tool_index()) {
        spdlog::debug("[ToolSwitcher] Tool T{} already active, ignoring", tool_index);
        return;
    }

    // Check if printing — warn before tool change
    auto job_state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));

    if (job_state == PrintJobState::PRINTING || job_state == PrintJobState::PAUSED) {
        spdlog::info("[ToolSwitcher] Print active, showing confirmation for T{}", tool_index);

        helix::ui::modal_show_confirmation(
            "Tool Change During Print",
            "Changing tools while printing may cause issues. Continue?",
            ::ModalSeverity::Warning, "Change Tool",
            // on_confirm
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] confirm_tool_change");
                int idx = static_cast<int>(
                    reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                auto* api = get_moonraker_api();
                if (api) {
                    ToolState::instance().request_tool_change(idx, api);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            // on_cancel (nullptr = just dismiss)
            nullptr,
            // user_data = tool_index
            reinterpret_cast<void*>(static_cast<intptr_t>(tool_index)));
        return;
    }

    // Not printing — change directly
    auto* api = get_moonraker_api();
    if (api) {
        tool_state.request_tool_change(tool_index, api);
        spdlog::info("[ToolSwitcher] Requesting tool change to T{}", tool_index);
    }
}

// ============================================================================
// Static XML event callbacks (registered at startup, used in XML if needed)
// ============================================================================

void ToolSwitcherWidget::tool_pill_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_pill_cb");
    if (!s_active_instance) return;
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    s_active_instance->handle_tool_selected(idx);
    LVGL_SAFE_EVENT_CB_END();
}

void ToolSwitcherWidget::tool_compact_cb(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolSwitcher] tool_compact_cb");
    if (s_active_instance) {
        s_active_instance->show_tool_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
