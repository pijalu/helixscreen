// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_stack_widget.h"

#include "ui_carousel.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fan_control_overlay.h"
#include "ui_fan_dial.h"
#include "ui_fonts.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_fan_state.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "ui/fan_spin_animation.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix {
void register_fan_stack_widget() {
    register_widget_factory("fan_stack", []() {
        auto& ps = get_printer_state();
        return std::make_unique<FanStackWidget>(ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "on_fan_stack_clicked", FanStackWidget::on_fan_stack_clicked);
}
} // namespace helix

using namespace helix;

FanStackWidget::FanStackWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

FanStackWidget::~FanStackWidget() {
    detach();
}

void FanStackWidget::set_config(const nlohmann::json& config) {
    config_ = config;
}

std::string FanStackWidget::get_component_name() const {
    if (is_carousel_mode()) {
        return "panel_widget_fan_carousel";
    }
    return "panel_widget_fan_stack";
}

bool FanStackWidget::on_edit_configure() {
    bool was_carousel = is_carousel_mode();
    nlohmann::json new_config = config_;
    if (was_carousel) {
        new_config.erase("display_mode");
    } else {
        new_config["display_mode"] = "carousel";
    }
    spdlog::info("[FanStackWidget] Toggling display_mode: {} → {}",
                 was_carousel ? "carousel" : "stack", was_carousel ? "stack" : "carousel");
    PanelWidgetManager::instance().get_widget_config("home").set_widget_config("fan_stack",
                                                                               new_config);
    return true;
}

bool FanStackWidget::is_carousel_mode() const {
    if (config_.contains("display_mode") && config_["display_mode"].is_string()) {
        return config_["display_mode"].get<std::string>() == "carousel";
    }
    return false;
}

void FanStackWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;
    lv_obj_set_user_data(widget_obj_, this);

    if (is_carousel_mode()) {
        attach_carousel(widget_obj);
    } else {
        attach_stack(widget_obj);
    }
}

void FanStackWidget::attach_stack(lv_obj_t* /*widget_obj*/) {
    // Cache label, name, and icon pointers
    part_label_ = lv_obj_find_by_name(widget_obj_, "fan_stack_part_speed");
    hotend_label_ = lv_obj_find_by_name(widget_obj_, "fan_stack_hotend_speed");
    aux_label_ = lv_obj_find_by_name(widget_obj_, "fan_stack_aux_speed");
    aux_row_ = lv_obj_find_by_name(widget_obj_, "fan_stack_aux_row");
    part_icon_ = lv_obj_find_by_name(widget_obj_, "fan_stack_part_icon");
    hotend_icon_ = lv_obj_find_by_name(widget_obj_, "fan_stack_hotend_icon");
    aux_icon_ = lv_obj_find_by_name(widget_obj_, "fan_stack_aux_icon");

    // Set initial text — text_small is a registered widget so XML inner content
    // isn't reliably applied. Observers update with real values on next tick.
    for (auto* label : {part_label_, hotend_label_, aux_label_}) {
        if (label)
            lv_label_set_text(label, "0%");
    }

    // Set rotation pivots on icons (center of 16px icon)
    for (auto* icon : {part_icon_, hotend_icon_, aux_icon_}) {
        if (icon) {
            lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
            lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);
        }
    }

    // Read initial animation setting
    auto& dsm = DisplaySettingsManager::instance();
    animations_enabled_ = dsm.get_animations_enabled();

    // Observe animation setting changes
    std::weak_ptr<bool> weak_alive = alive_;
    anim_settings_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        DisplaySettingsManager::instance().subject_animations_enabled(), this,
        [weak_alive](FanStackWidget* self, int enabled) {
            if (weak_alive.expired())
                return;
            self->animations_enabled_ = (enabled != 0);
            self->refresh_all_animations();
        });

    // Observe fans_version to re-bind when fans are discovered
    version_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        printer_state_.get_fans_version_subject(), this,
        [weak_alive](FanStackWidget* self, int /*version*/) {
            if (weak_alive.expired())
                return;
            self->bind_fans();
        });

    // Bind immediately with current fan data (the deferred observer callback
    // may be dropped if the update queue is frozen during populate_widgets).
    bind_fans();

    spdlog::debug("[FanStackWidget] Attached stack (animations={})", animations_enabled_);
}

void FanStackWidget::attach_carousel(lv_obj_t* widget_obj) {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj, "fan_carousel");
    if (!carousel) {
        spdlog::error("[FanStackWidget] Could not find fan_carousel in XML");
        return;
    }

    // Observe fans_version to rebuild carousel pages when fans are discovered
    std::weak_ptr<bool> weak_alive = alive_;
    version_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        printer_state_.get_fans_version_subject(), this,
        [weak_alive](FanStackWidget* self, int /*version*/) {
            if (weak_alive.expired())
                return;
            self->bind_carousel_fans();
        });

    // Bind immediately with current fan data (the deferred observer callback
    // may be dropped if the update queue is frozen during populate_widgets).
    bind_carousel_fans();

    spdlog::debug("[FanStackWidget] Attached carousel");
}

void FanStackWidget::detach() {
    *alive_ = false;
    part_observer_.reset();
    hotend_observer_.reset();
    aux_observer_.reset();
    version_observer_.reset();
    anim_settings_observer_.reset();
    carousel_observers_.clear();

    // Stop any running animations before clearing pointers
    if (part_icon_)
        stop_spin(part_icon_);
    if (hotend_icon_)
        stop_spin(hotend_icon_);
    if (aux_icon_)
        stop_spin(aux_icon_);

    // Destroy carousel FanDial instances
    fan_dials_.clear();

    if (widget_obj_)
        lv_obj_set_user_data(widget_obj_, nullptr);
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    fan_control_panel_ = nullptr;
    part_label_ = nullptr;
    hotend_label_ = nullptr;
    aux_label_ = nullptr;
    aux_row_ = nullptr;
    part_icon_ = nullptr;
    hotend_icon_ = nullptr;
    aux_icon_ = nullptr;

    spdlog::debug("[FanStackWidget] Detached");
}

void FanStackWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/,
                                     int /*height_px*/) {
    // Size adaptation only applies to stack mode
    if (!widget_obj_ || is_carousel_mode())
        return;

    // Size tiers:
    //   1x1 (compact):  xs fonts, single-letter labels (P, H, C)
    //   wider or taller: sm fonts, short labels (Part, HE, Chm)
    bool bigger = (colspan >= 2 || rowspan >= 2);

    const char* font_token = bigger ? "font_small" : "font_xs";
    const lv_font_t* text_font = theme_manager_get_font(font_token);
    if (!text_font)
        return;

    // Icon font: xs=16px, sm=24px
    const lv_font_t* icon_font = bigger ? &mdi_icons_24 : &mdi_icons_16;

    // Apply text font to all speed labels
    for (auto* label : {part_label_, hotend_label_, aux_label_}) {
        if (label)
            lv_obj_set_style_text_font(label, text_font, 0);
    }

    // Apply icon font to fan icons
    for (auto* icon : {part_icon_, hotend_icon_, aux_icon_}) {
        if (icon) {
            lv_obj_t* glyph = lv_obj_get_child(icon, 0);
            if (glyph)
                lv_obj_set_style_text_font(glyph, icon_font, 0);
        }
    }

    // Name labels — three tiers of text:
    //   1x1 or 1x2: single letter (P, H, C)
    //   2x1 (wide but short): abbreviations (Part, HE, Chm)
    //   2x2+ (wide AND tall): full words (Part, Hotend, Chamber)
    bool wide = (colspan >= 2);
    bool roomy = (colspan >= 2 && rowspan >= 2);
    struct NameMapping {
        const char* obj_name;
        const char* compact; // narrow: single letter
        const char* abbrev;  // wide: short abbreviation
        const char* full;    // wide+tall: full word
    };
    static constexpr NameMapping name_map[] = {
        {"fan_stack_part_name", "P", "Part", "Part"},
        {"fan_stack_hotend_name", "H", "HE", "Hotend"},
        {"fan_stack_aux_name", "C", "Chm", "Chamber"},
    };
    for (const auto& m : name_map) {
        lv_obj_t* lbl = lv_obj_find_by_name(widget_obj_, m.obj_name);
        if (lbl) {
            lv_obj_set_style_text_font(lbl, text_font, 0);
            const char* text = roomy ? m.full : (wide ? m.abbrev : m.compact);
            lv_label_set_text(lbl, lv_tr(text));
        }
    }

    // Center the content block when the widget is wider than 1x.
    // Each row is LV_SIZE_CONTENT so it shrink-wraps its text.
    // Setting cross_place to CENTER on the flex-column parent centers
    // the rows horizontally, but that causes ragged left edges.
    // Instead: keep rows at SIZE_CONTENT and set the parent's
    // cross_place to CENTER — but use a uniform min_width on all rows
    // so they share the same left edge.
    const char* row_names[] = {"fan_stack_part_row", "fan_stack_hotend_row", "fan_stack_aux_row"};
    if (bigger) {
        // First pass: set rows to content width and measure the widest
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row)
                lv_obj_set_width(row, LV_SIZE_CONTENT);
        }
        lv_obj_update_layout(widget_obj_);

        int max_w = 0;
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
                int w = lv_obj_get_width(row);
                if (w > max_w)
                    max_w = w;
            }
        }

        // Second pass: set all rows to the same width (widest row)
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row)
                lv_obj_set_width(row, max_w);
        }
    } else {
        for (const char* rn : row_names) {
            lv_obj_t* row = lv_obj_find_by_name(widget_obj_, rn);
            if (row)
                lv_obj_set_width(row, LV_PCT(100));
        }
    }

    spdlog::debug("[FanStackWidget] on_size_changed {}x{} -> font {}", colspan, rowspan,
                  font_token);
}

void FanStackWidget::bind_fans() {
    // Reset existing per-fan observers
    part_observer_.reset();
    hotend_observer_.reset();
    aux_observer_.reset();

    part_fan_name_.clear();
    hotend_fan_name_.clear();
    aux_fan_name_.clear();

    part_speed_ = 0;
    hotend_speed_ = 0;
    aux_speed_ = 0;

    const auto& fans = printer_state_.get_fans();
    if (fans.empty()) {
        spdlog::debug("[FanStackWidget] No fans discovered yet");
        return;
    }

    // Classify fans into our three rows and set name labels
    std::string part_display, hotend_display, aux_display;
    for (const auto& fan : fans) {
        switch (fan.type) {
        case FanType::PART_COOLING:
            if (part_fan_name_.empty()) {
                part_fan_name_ = fan.object_name;
                part_display = fan.display_name;
            }
            break;
        case FanType::HEATER_FAN:
            if (hotend_fan_name_.empty()) {
                hotend_fan_name_ = fan.object_name;
                hotend_display = fan.display_name;
            }
            break;
        case FanType::CONTROLLER_FAN:
        case FanType::TEMPERATURE_FAN:
        case FanType::GENERIC_FAN:
            if (aux_fan_name_.empty()) {
                aux_fan_name_ = fan.object_name;
                aux_display = fan.display_name;
            }
            break;
        }
    }

    std::weak_ptr<bool> weak_alive = alive_;

    // Bind part fan
    if (!part_fan_name_.empty()) {
        SubjectLifetime lifetime;
        lv_subject_t* subject = printer_state_.get_fan_speed_subject(part_fan_name_, lifetime);
        if (subject) {
            part_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
                subject, this,
                [weak_alive](FanStackWidget* self, int speed) {
                    if (weak_alive.expired())
                        return;
                    self->part_speed_ = speed;
                    self->update_label(self->part_label_, speed);
                    self->update_fan_animation(self->part_icon_, speed);
                },
                lifetime);

            // Read current value directly — the deferred observer initial fire
            // is dropped when populate_widgets() freezes the update queue.
            int current = lv_subject_get_int(subject);
            part_speed_ = current;
            update_label(part_label_, current);
            update_fan_animation(part_icon_, current);
        }
    }

    // Bind hotend fan
    if (!hotend_fan_name_.empty()) {
        SubjectLifetime lifetime;
        lv_subject_t* subject = printer_state_.get_fan_speed_subject(hotend_fan_name_, lifetime);
        if (subject) {
            hotend_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
                subject, this,
                [weak_alive](FanStackWidget* self, int speed) {
                    if (weak_alive.expired())
                        return;
                    self->hotend_speed_ = speed;
                    self->update_label(self->hotend_label_, speed);
                    self->update_fan_animation(self->hotend_icon_, speed);
                },
                lifetime);

            int current = lv_subject_get_int(subject);
            hotend_speed_ = current;
            update_label(hotend_label_, current);
            update_fan_animation(hotend_icon_, current);
        }
    }

    // Bind aux fan (hide row if none)
    if (!aux_fan_name_.empty()) {
        if (aux_row_) {
            lv_obj_remove_flag(aux_row_, LV_OBJ_FLAG_HIDDEN);
        }
        SubjectLifetime lifetime;
        lv_subject_t* subject = printer_state_.get_fan_speed_subject(aux_fan_name_, lifetime);
        if (subject) {
            aux_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
                subject, this,
                [weak_alive](FanStackWidget* self, int speed) {
                    if (weak_alive.expired())
                        return;
                    self->aux_speed_ = speed;
                    self->update_label(self->aux_label_, speed);
                    self->update_fan_animation(self->aux_icon_, speed);
                },
                lifetime);

            int current = lv_subject_get_int(subject);
            aux_speed_ = current;
            update_label(aux_label_, current);
            update_fan_animation(aux_icon_, current);
        }
    } else {
        if (aux_row_) {
            lv_obj_add_flag(aux_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    spdlog::debug("[FanStackWidget] Bound fans: part='{}' hotend='{}' aux='{}'", part_fan_name_,
                  hotend_fan_name_, aux_fan_name_);
}

void FanStackWidget::bind_carousel_fans() {
    if (!widget_obj_)
        return;

    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "fan_carousel");
    if (!carousel)
        return;

    // Reset existing per-fan observers and dials
    part_observer_.reset();
    hotend_observer_.reset();
    aux_observer_.reset();
    carousel_observers_.clear();
    fan_dials_.clear();

    const auto& fans = printer_state_.get_fans();
    if (fans.empty()) {
        spdlog::debug("[FanStackWidget] Carousel: no fans discovered yet");
        return;
    }

    // Clear existing carousel pages (the carousel may have pages from a previous bind)
    auto* state = ui_carousel_get_state(carousel);
    if (state && state->scroll_container) {
        lv_obj_clean(state->scroll_container);
        state->real_tiles.clear();
        ui_carousel_rebuild_indicators(carousel);
    }

    std::weak_ptr<bool> weak_alive = alive_;

    for (const auto& fan : fans) {
        // Create FanDial as a carousel page
        auto dial = std::make_unique<FanDial>(lv_scr_act(), fan.display_name, fan.object_name,
                                              fan.speed_percent);

        // Auto-controlled fans get read-only arc (no knob, muted indicator)
        if (!fan.is_controllable) {
            dial->set_read_only(true);
        }

        // Wire icon click to open fan control overlay
        dial->set_on_icon_clicked([weak_alive, this](const std::string& /*fan_id*/) {
            if (weak_alive.expired())
                return;
            handle_clicked();
        });

        // Wire speed change callback only for controllable fans
        if (fan.is_controllable) {
            std::string object_name = fan.object_name;
            auto& ps = printer_state_;
            dial->set_on_speed_changed(
                [weak_alive, &ps, object_name](const std::string& /*fan_id*/, int speed_percent) {
                    if (weak_alive.expired())
                        return;
                    auto* api = get_moonraker_api();
                    if (!api) {
                        spdlog::warn("[FanStackWidget] Cannot send fan speed - no API connection");
                        NOTIFY_WARNING("No printer connection");
                        return;
                    }

                    ps.update_fan_speed(object_name, static_cast<double>(speed_percent) / 100.0);
                    api->set_fan_speed(
                        object_name, static_cast<double>(speed_percent), []() {},
                        [object_name](const MoonrakerError& err) {
                            NOTIFY_ERROR("Fan control failed: {}", err.user_message());
                        });
                });
        }

        // Add to carousel with size/style overrides for compact widget slot
        lv_obj_t* root = dial->get_root();
        if (root) {
            // Fill carousel page instead of using overlay-sized tokens
            lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
            lv_obj_set_style_min_width(root, 0, 0);
            lv_obj_set_style_max_width(root, LV_PCT(100), 0);
            lv_obj_set_style_min_height(root, 0, 0);
            lv_obj_set_style_max_height(root, LV_PCT(100), 0);

            // Strip card border/background — carousel pages don't need card chrome
            lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_gap(root, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);

            // Hide Off/On button row — too small for carousel widget slot
            lv_obj_t* btn_row = lv_obj_find_by_name(root, "button_row");
            if (btn_row) {
                lv_obj_add_flag(btn_row, LV_OBJ_FLAG_HIDDEN);
            }

            // Inset the dial container so the arc doesn't clip the name label
            lv_obj_t* dial_container = lv_obj_find_by_name(root, "dial_container");
            if (dial_container) {
                int32_t inset = theme_manager_get_spacing("space_sm");
                lv_obj_set_style_pad_all(dial_container, inset, LV_PART_MAIN);
            }

            // Shrink text for compact display
            const lv_font_t* xs_font = theme_manager_get_font("font_xs");
            if (xs_font) {
                lv_obj_t* name_label = lv_obj_find_by_name(root, "name_label");
                if (name_label) {
                    lv_obj_set_style_text_font(name_label, xs_font, 0);
                }
                lv_obj_t* speed_label = lv_obj_find_by_name(root, "speed_label");
                if (speed_label) {
                    lv_obj_set_style_text_font(speed_label, xs_font, 0);
                }
            }

            ui_carousel_add_item(carousel, root);
        }

        // Observe fan speed to update dial
        SubjectLifetime lifetime;
        lv_subject_t* subject = printer_state_.get_fan_speed_subject(fan.object_name, lifetime);
        if (subject) {
            FanDial* dial_ptr = dial.get();
            auto obs = helix::ui::observe_int_sync<FanStackWidget>(
                subject, this,
                [weak_alive, dial_ptr](FanStackWidget* /*self*/, int speed) {
                    if (weak_alive.expired())
                        return;
                    dial_ptr->set_speed(speed);
                },
                lifetime);

            // Read current value immediately — deferred initial fire is
            // dropped when populate_widgets() freezes the update queue.
            dial_ptr->set_speed(lv_subject_get_int(subject));

            carousel_observers_.push_back(std::move(obs));
        }

        fan_dials_.push_back(std::move(dial));
    }

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[FanStackWidget] Carousel bound {} fan dials", page_count);
}

void FanStackWidget::update_label(lv_obj_t* label, int speed_pct) {
    if (!label)
        return;

    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d%%", speed_pct);
    lv_label_set_text(label, buf);
}

void FanStackWidget::update_fan_animation(lv_obj_t* icon, int speed_pct) {
    if (!icon)
        return;

    if (!animations_enabled_ || speed_pct <= 0) {
        helix::ui::fan_spin_stop(icon);
    } else {
        helix::ui::fan_spin_start(icon, speed_pct);
    }
}

void FanStackWidget::refresh_all_animations() {
    update_fan_animation(part_icon_, part_speed_);
    update_fan_animation(hotend_icon_, hotend_speed_);
    update_fan_animation(aux_icon_, aux_speed_);
}

void FanStackWidget::spin_anim_cb(void* var, int32_t value) {
    helix::ui::fan_spin_anim_cb(var, value);
}

void FanStackWidget::stop_spin(lv_obj_t* icon) {
    helix::ui::fan_spin_stop(icon);
}

void FanStackWidget::start_spin(lv_obj_t* icon, int speed_pct) {
    helix::ui::fan_spin_start(icon, speed_pct);
}

void FanStackWidget::handle_clicked() {
    spdlog::debug("[FanStackWidget] Clicked - opening fan control overlay");

    if (!fan_control_panel_ && parent_screen_) {
        auto& overlay = get_fan_control_overlay();

        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();
        overlay.set_api(get_moonraker_api());

        fan_control_panel_ = overlay.create(parent_screen_);
        if (!fan_control_panel_) {
            spdlog::error("[FanStackWidget] Failed to create fan control overlay");
            return;
        }
        NavigationManager::instance().register_overlay_instance(fan_control_panel_, &overlay);
    }

    if (fan_control_panel_) {
        get_fan_control_overlay().set_api(get_moonraker_api());
        NavigationManager::instance().push_overlay(fan_control_panel_);
    }
}

void FanStackWidget::on_fan_stack_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[FanStackWidget] on_fan_stack_clicked");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<FanStackWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_clicked();
    } else {
        spdlog::warn("[FanStackWidget] on_fan_stack_clicked: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}
