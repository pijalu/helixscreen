// SPDX-License-Identifier: GPL-3.0-or-later

#include "power_device_widget.h"

#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_fonts.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "power_device_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {
void register_power_device_widget() {
    register_widget_factory("power_device", [](const std::string& id) {
        return std::make_unique<PowerDeviceWidget>(id);
    });
    lv_xml_register_event_cb(nullptr, "power_device_clicked_cb",
                             PowerDeviceWidget::power_device_clicked_cb);
}
} // namespace helix

namespace {

/// Resolve a responsive spacing token to pixels, with a fallback.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

// Power-related icons for the picker grid
static const char* const kPowerIcons[] = {
    // clang-format off
    // Power symbols
    "power_cycle",       "power",              "power_on",            "power_off",
    "power_standby",
    // Plugs
    "power_plug",        "power_plug_off",     "power_plug_outline",  "power_plug_battery",
    // Sockets
    "power_socket",      "power_socket_au",    "power_socket_ch",     "power_socket_de",
    "power_socket_eu",   "power_socket_fr",    "power_socket_it",     "power_socket_jp",
    "power_socket_uk",   "power_socket_us",
    // Device types (already in font)
    "lightbulb_outline", "lightbulb_on",       "led_strip",           "fan",
    "radiator",          "flash",              "electric_switch",
    // clang-format on
};
static constexpr size_t kPowerIconCount = std::size(kPowerIcons);
static constexpr int kIconCellSize = 36;
static constexpr const char* kDefaultIcon = "power_cycle";

// Icons with distinct on/off glyphs. Config always stores the ON variant;
// resolve_icon_for_state() derives the OFF variant from this table.
struct IconPair {
    const char* on_icon;
    const char* off_icon;
};
static const IconPair kIconPairs[] = {
    {"power_on", "power_off"},
    {"power_plug", "power_plug_off"},
    {"lightbulb_on", "lightbulb_outline"},
    {"fan", "fan_off"},
};

/// Map an off-variant icon name to its on-variant (e.g., "fan_off" → "fan").
/// Returns the input unchanged if it's not an off-variant.
static const char* to_on_variant(const char* icon) {
    for (const auto& pair : kIconPairs) {
        if (std::strcmp(icon, pair.off_icon) == 0)
            return pair.on_icon;
    }
    return icon;
}

/// Return the icon to display for a given power status.
/// For paired icons, returns the off-variant when the device is off/locked.
static const char* resolve_icon_for_state(const char* base_icon, int status) {
    if (status == 1)
        return base_icon;
    for (const auto& pair : kIconPairs) {
        if (std::strcmp(base_icon, pair.on_icon) == 0)
            return pair.off_icon;
    }
    return base_icon;
}

/// Apply highlight styling to an icon grid cell.
void apply_icon_cell_highlight(lv_obj_t* cell, bool selected) {
    if (selected) {
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, theme_manager_get_color("primary"), 0);
        lv_obj_set_style_bg_opa(cell, 20, 0);
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("primary"), 0);
    } else {
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, 0, 0);
    }
}

} // namespace

using namespace helix;

PowerDeviceWidget* PowerDeviceWidget::s_active_picker_ = nullptr;

PowerDeviceWidget::PowerDeviceWidget(const std::string& instance_id) : instance_id_(instance_id) {}

PowerDeviceWidget::~PowerDeviceWidget() {
    detach();
}

void PowerDeviceWidget::set_config(const nlohmann::json& config) {
    if (config.contains("device") && config["device"].is_string()) {
        device_name_ = config["device"].get<std::string>();
    }
    if (config.contains("icon") && config["icon"].is_string()) {
        icon_name_ = config["icon"].get<std::string>();
    }
    spdlog::debug("[PowerDeviceWidget] Config: {}={} icon={}", instance_id_,
                  device_name_.empty() ? "(unconfigured)" : device_name_,
                  icon_name_.empty() ? kDefaultIcon : icon_name_);
}

void PowerDeviceWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);

        // Pressed feedback
        lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Cache LVGL object pointers from XML
    badge_obj_ = lv_obj_find_by_name(widget_obj_, "power_badge");
    icon_obj_ = lv_obj_find_by_name(widget_obj_, "power_icon");
    name_label_ = lv_obj_find_by_name(widget_obj_, "power_device_name");
    status_label_ = lv_obj_find_by_name(widget_obj_, "power_device_status");
    lock_icon_ = lv_obj_find_by_name(widget_obj_, "power_lock_icon");

    if (!device_name_.empty()) {
        // Observe the device status subject. Use a LOCAL lifetime variable
        // so the ObserverGuard's weak_ptr expires when deinit_subjects()
        // destroys the PowerDeviceState's copy (shutdown safety).
        SubjectLifetime lifetime;
        lv_subject_t* subj =
            PowerDeviceState::instance().get_status_subject(device_name_, lifetime);
        if (subj) {
            auto token = lifetime_.token();
            status_observer_ = helix::ui::observe_int_sync<PowerDeviceWidget>(
                subj, this,
                [token](PowerDeviceWidget* self, int status) {
                    if (token.expired())
                        return;
                    self->update_display(status);
                },
                lifetime);

            // Set initial display name
            if (name_label_) {
                std::string display =
                    helix::get_display_name(device_name_, helix::DeviceType::POWER_DEVICE);
                lv_label_set_text(name_label_, display.c_str());
            }
        } else {
            spdlog::warn("[PowerDeviceWidget] No status subject for device '{}'", device_name_);
            update_display(-1);
        }
    } else {
        // Unconfigured state
        update_display(-1);
    }

    spdlog::debug("[PowerDeviceWidget] Attached {} (device: {})", instance_id_,
                  device_name_.empty() ? "none" : device_name_);
}

void PowerDeviceWidget::detach() {
    lifetime_.invalidate();
    dismiss_device_picker();

    status_observer_.reset();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    badge_obj_ = nullptr;
    icon_obj_ = nullptr;
    name_label_ = nullptr;
    status_label_ = nullptr;
    lock_icon_ = nullptr;

    spdlog::debug("[PowerDeviceWidget] Detached");
}

void PowerDeviceWidget::update_display(int status) {
    // Status values: 0=off, 1=on, 2=locked, -1=unconfigured

    if (badge_obj_) {
        switch (status) {
        case 1: // ON
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("danger"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 40, 0);
            break;
        case 0: // OFF
        case 2: // LOCKED
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("text_muted"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 20, 0);
            break;
        default: // Unconfigured
            lv_obj_set_style_bg_color(badge_obj_, theme_manager_get_color("secondary"), 0);
            lv_obj_set_style_bg_opa(badge_obj_, 20, 0);
            break;
        }
    }

    if (icon_obj_) {
        // Apply icon — for paired icons, toggle between on/off variants
        const char* base_icon = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        const char* effective_icon = resolve_icon_for_state(base_icon, status);
        ui_icon_set_source(icon_obj_, effective_icon);

        switch (status) {
        case 1:
            ui_icon_set_variant(icon_obj_, "danger");
            break;
        case 0:
        case 2:
            ui_icon_set_variant(icon_obj_, "muted");
            break;
        default:
            ui_icon_set_variant(icon_obj_, "secondary");
            break;
        }
    }

    if (lock_icon_) {
        if (status == 2) {
            lv_obj_remove_flag(lock_icon_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lock_icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (status_label_) {
        switch (status) {
        case 1:
            lv_label_set_text(status_label_, lv_tr("ON"));
            lv_obj_set_style_text_color(status_label_, theme_manager_get_color("danger"), 0);
            break;
        case 0:
            lv_label_set_text(status_label_, lv_tr("OFF"));
            lv_obj_set_style_text_color(status_label_, theme_manager_get_color("text_muted"), 0);
            break;
        case 2:
            lv_label_set_text(status_label_, lv_tr("LOCKED"));
            lv_obj_set_style_text_color(status_label_, theme_manager_get_color("text_muted"), 0);
            break;
        default:
            lv_label_set_text(status_label_, "");
            break;
        }
    }

    if (name_label_ && status == -1) {
        lv_label_set_text(name_label_, lv_tr("Configure"));
    }
}

void PowerDeviceWidget::handle_clicked() {
    if (device_name_.empty()) {
        spdlog::info("[PowerDeviceWidget] {} clicked (unconfigured) - showing picker",
                     instance_id_);
        show_device_picker();
        return;
    }

    // Check current status
    SubjectLifetime lt;
    lv_subject_t* subj = PowerDeviceState::instance().get_status_subject(device_name_, lt);
    if (subj) {
        int status = lv_subject_get_int(subj);
        if (status == 2) {
            spdlog::debug("[PowerDeviceWidget] {} - device '{}' is locked, ignoring click",
                          instance_id_, device_name_);
            return;
        }
    }

    MoonrakerAPI* api = get_api();
    if (!api) {
        spdlog::warn("[PowerDeviceWidget] No API available");
        return;
    }

    // Suppress "Printer Firmware Disconnected" dialog when turning off a power device.
    // The device may have bound_services: klipper, causing an expected Klipper disconnect.
    // Check current state: if on (1), toggle means turning off.
    {
        SubjectLifetime lt;
        lv_subject_t* status_subj =
            PowerDeviceState::instance().get_status_subject(device_name_, lt);
        if (status_subj && lv_subject_get_int(status_subj) == 1) {
            EmergencyStopOverlay::instance().suppress_recovery_dialog(
                RecoverySuppression::NORMAL);
        }
    }

    spdlog::info("[PowerDeviceWidget] {} toggling device '{}'", instance_id_, device_name_);
    api->set_device_power(
        device_name_, "toggle",
        [name = device_name_]() {
            spdlog::debug("[PowerDeviceWidget] Device '{}' toggled successfully", name);
        },
        [name = device_name_](const MoonrakerError& err) {
            spdlog::error("[PowerDeviceWidget] Failed to toggle device '{}': {}", name,
                          err.message);
        });
}

void PowerDeviceWidget::power_device_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] power_device_clicked_cb");
    auto* widget = panel_widget_from_event<PowerDeviceWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

bool PowerDeviceWidget::on_edit_configure() {
    spdlog::info("[PowerDeviceWidget] {} configure requested - showing picker", instance_id_);
    show_device_picker();
    return false;
}

MoonrakerAPI* PowerDeviceWidget::get_api() const {
    return get_moonraker_api();
}

void PowerDeviceWidget::show_device_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    // Dismiss any other widget's picker
    if (s_active_picker_ && s_active_picker_ != this) {
        s_active_picker_->dismiss_device_picker();
    }

    auto device_names = PowerDeviceState::instance().device_names();
    if (device_names.empty()) {
        spdlog::warn("[PowerDeviceWidget] No power devices available");
        return;
    }
    std::sort(device_names.begin(), device_names.end());

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
            LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] backdrop_cb");
            if (s_active_picker_) {
                s_active_picker_->dismiss_device_picker();
            }
            LVGL_SAFE_EVENT_CB_END();
        },
        LV_EVENT_CLICKED, nullptr);

    // Card container
    lv_obj_t* card = lv_obj_create(picker_backdrop_);
    int card_w = std::clamp(screen_w * 50 / 100, 200, 360);
    lv_obj_set_width(card, card_w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, screen_h * 70 / 100, 0);
    lv_obj_set_style_bg_color(card, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, theme_manager_get_color("border"), 0);
    lv_obj_set_style_pad_all(card, space_md, 0);
    lv_obj_set_style_pad_gap(card, space_xs, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); // Prevent clicks passing through

    // Title
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, lv_tr("Select Power Device"));
    lv_obj_set_style_text_font(title, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(title, theme_manager_get_color("text"), 0);
    lv_obj_set_width(title, LV_PCT(100));

    // Divider below title
    lv_obj_t* divider = lv_obj_create(card);
    lv_obj_set_width(divider, LV_PCT(100));
    lv_obj_set_height(divider, 1);
    lv_obj_set_style_bg_color(divider, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_bg_opa(divider, 38, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    // Scrollable list
    lv_obj_t* list = lv_obj_create(card);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(list, screen_h * 35 / 100, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_gap(list, 2, 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (const auto& name : device_names) {
        bool is_selected = (name == device_name_);
        std::string display = helix::get_display_name(name, helix::DeviceType::POWER_DEVICE);

        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(row, 6, 0);

        // Highlight selected row
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);
        if (is_selected) {
            lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), 0);
        }

        // Pressed feedback
        lv_obj_set_style_bg_color(row, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        // Device display name
        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, display.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store device name for click handler
        auto* name_copy = new std::string(name);
        lv_obj_set_user_data(row, name_copy);

        // Free heap string when row is deleted
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) { delete static_cast<std::string*>(lv_event_get_user_data(ev)); },
            LV_EVENT_DELETE, name_copy);

        // Click selects device and dismisses picker
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] device_row_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(ev));
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
                if (!name_ptr)
                    return;

                if (PowerDeviceWidget::s_active_picker_) {
                    std::string selected = *name_ptr;
                    PowerDeviceWidget::s_active_picker_->select_device(selected);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    // Icon section divider
    lv_obj_t* icon_divider = lv_obj_create(card);
    lv_obj_set_width(icon_divider, LV_PCT(100));
    lv_obj_set_height(icon_divider, 1);
    lv_obj_set_style_bg_color(icon_divider, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_style_bg_opa(icon_divider, 38, 0);
    lv_obj_set_style_pad_all(icon_divider, 0, 0);
    lv_obj_set_style_border_width(icon_divider, 0, 0);
    lv_obj_remove_flag(icon_divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon_divider, LV_OBJ_FLAG_CLICKABLE);

    // Icon section title
    lv_obj_t* icon_title = lv_label_create(card);
    lv_label_set_text(icon_title, lv_tr("Icon"));
    lv_obj_set_style_text_font(icon_title, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(icon_title, theme_manager_get_color("text"), 0);
    lv_obj_set_width(icon_title, LV_PCT(100));

    // Icon grid (wrap flow)
    lv_obj_t* icon_grid = lv_obj_create(card);
    lv_obj_set_width(icon_grid, LV_PCT(100));
    lv_obj_set_height(icon_grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icon_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(icon_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(icon_grid, 0, 0);
    lv_obj_set_style_pad_gap(icon_grid, 4, 0);
    lv_obj_set_style_bg_opa(icon_grid, 0, 0);
    lv_obj_set_style_border_width(icon_grid, 0, 0);
    lv_obj_remove_flag(icon_grid, LV_OBJ_FLAG_SCROLLABLE);

    std::string effective_icon = icon_name_.empty() ? kDefaultIcon : icon_name_;

    for (size_t i = 0; i < kPowerIconCount; ++i) {
        lv_obj_t* cell = lv_obj_create(icon_grid);
        lv_obj_set_size(cell, kIconCellSize, kIconCellSize);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(cell, 0, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);

        // Pressed feedback
        lv_obj_set_style_bg_color(cell, theme_manager_get_color("text_muted"),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);

        apply_icon_cell_highlight(cell, kPowerIcons[i] == effective_icon);

        // Icon glyph
        const char* cp = ui_icon::lookup_codepoint(kPowerIcons[i]);
        if (cp) {
            lv_obj_t* icon = lv_label_create(cell);
            lv_label_set_text(icon, cp);
            lv_obj_set_style_text_font(icon, &mdi_icons_24, 0);
            lv_obj_set_style_text_color(icon, theme_manager_get_color("text"), 0);
            lv_obj_center(icon);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        // Store index as user_data
        lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_obj_add_event_cb(
            cell,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] icon_cell_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto idx =
                    static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                if (idx < kPowerIconCount && PowerDeviceWidget::s_active_picker_) {
                    PowerDeviceWidget::s_active_picker_->select_icon(kPowerIcons[idx]);
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    s_active_picker_ = this;

    // Self-clearing delete callback for parent deletion safety
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* ev) {
            auto* self = static_cast<PowerDeviceWidget*>(lv_event_get_user_data(ev));
            if (!self)
                return;
            self->picker_backdrop_ = nullptr;
            if (s_active_picker_ == self) {
                s_active_picker_ = nullptr;
            }
        },
        LV_EVENT_DELETE, this);

    // Position card near the widget
    if (card && widget_obj_) {
        lv_area_t widget_area;
        lv_obj_get_coords(widget_obj_, &widget_area);

        int card_x = (widget_area.x1 + widget_area.x2) / 2 - card_w / 2;
        int card_y = widget_area.y2 + space_xs;

        // Clamp to screen bounds
        if (card_x < space_md)
            card_x = space_md;
        if (card_x + card_w > screen_w - space_md)
            card_x = screen_w - card_w - space_md;

        int card_max_h = screen_h * 70 / 100;
        if (card_y + card_max_h > screen_h - space_md) {
            card_y = widget_area.y1 - card_max_h - space_xs;
            if (card_y < space_md)
                card_y = space_md;
        }

        lv_obj_set_pos(card, card_x, card_y);
    }

    spdlog::debug("[PowerDeviceWidget] Picker shown with {} devices", device_names.size());
}

void PowerDeviceWidget::dismiss_device_picker() {
    if (!picker_backdrop_) {
        return;
    }

    lv_obj_t* backdrop = picker_backdrop_;
    picker_backdrop_ = nullptr;
    s_active_picker_ = nullptr;

    if (lv_obj_is_valid(backdrop)) {
        helix::ui::safe_delete_deferred(backdrop);
    }

    spdlog::debug("[PowerDeviceWidget] Picker dismissed");
}

void PowerDeviceWidget::select_device(const std::string& name) {
    device_name_ = name;
    save_config();
    dismiss_device_picker();

    // Re-attach to start observing the new device
    if (widget_obj_ && parent_screen_) {
        // Reset observer before re-attaching
        status_observer_.reset();

        // Observe the new device status (local lifetime — see attach() comment)
        SubjectLifetime lifetime;
        lv_subject_t* subj =
            PowerDeviceState::instance().get_status_subject(device_name_, lifetime);
        if (subj) {
            auto token = lifetime_.token();
            status_observer_ = helix::ui::observe_int_sync<PowerDeviceWidget>(
                subj, this,
                [token](PowerDeviceWidget* self, int status) {
                    if (token.expired())
                        return;
                    self->update_display(status);
                },
                lifetime);
        }

        // Update display name
        if (name_label_) {
            std::string display =
                helix::get_display_name(device_name_, helix::DeviceType::POWER_DEVICE);
            lv_label_set_text(name_label_, display.c_str());
        }
    }

    spdlog::info("[PowerDeviceWidget] {} selected device: {}", instance_id_, name);
}

void PowerDeviceWidget::select_icon(const std::string& name) {
    // Store the ON variant so update_display can derive the OFF icon from the pair table
    std::string canonical(to_on_variant(name.c_str()));
    icon_name_ = (canonical == kDefaultIcon) ? "" : canonical;
    save_config();

    // Update the widget icon immediately
    if (icon_obj_) {
        const char* effective = icon_name_.empty() ? kDefaultIcon : icon_name_.c_str();
        ui_icon_set_source(icon_obj_, effective);
    }

    // Update icon grid highlights if picker is still open
    if (picker_backdrop_) {
        // The icon grid is inside: backdrop -> card -> icon_grid
        // Walk card children to find grid and update highlights
        lv_obj_t* card = lv_obj_get_child(picker_backdrop_, 0);
        if (card) {
            // Icon grid is the last child of the card
            uint32_t child_count = lv_obj_get_child_count(card);
            if (child_count > 0) {
                lv_obj_t* icon_grid = lv_obj_get_child(card, child_count - 1);
                std::string effective_icon = icon_name_.empty() ? kDefaultIcon : icon_name_;
                uint32_t grid_count = lv_obj_get_child_count(icon_grid);
                for (uint32_t i = 0; i < grid_count; ++i) {
                    lv_obj_t* cell = lv_obj_get_child(icon_grid, i);
                    auto idx =
                        static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell)));
                    if (idx < kPowerIconCount) {
                        apply_icon_cell_highlight(cell, kPowerIcons[idx] == effective_icon);
                    }
                }
            }
        }
    }

    spdlog::info("[PowerDeviceWidget] {} selected icon: {}", instance_id_,
                 icon_name_.empty() ? "power_cycle (default)" : icon_name_);
}

void PowerDeviceWidget::save_config() {
    nlohmann::json config;
    config["device"] = device_name_;
    if (!icon_name_.empty())
        config["icon"] = icon_name_;
    save_widget_config(config);
    spdlog::debug("[PowerDeviceWidget] Saved config: {}={} icon={}", instance_id_, device_name_,
                  icon_name_.empty() ? kDefaultIcon : icon_name_);
}
