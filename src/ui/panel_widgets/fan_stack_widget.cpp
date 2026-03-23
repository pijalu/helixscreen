// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fan_stack_widget.h"

#include "ui_carousel.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_fan_arc_resize.h"
#include "ui_fan_control_overlay.h"
#include "ui_fonts.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_fan_state.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "ui/fan_spin_animation.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_fan_stack_widget() {
    register_widget_factory("fan_stack", [](const std::string&) {
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
    save_widget_config(new_config);
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

    // Pressed feedback: dim widget on touch
    lv_obj_set_style_opa(widget_obj_, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);

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
    for (auto* icon : {part_icon_, hotend_icon_, aux_icon_})
        set_icon_pivot(icon);

    setup_common_observers([this]() { refresh_all_animations(); }, [this]() { bind_fans(); });
    bind_fans();

    spdlog::debug("[FanStackWidget] Attached stack (animations={})", animations_enabled_);
}

void FanStackWidget::attach_carousel(lv_obj_t* widget_obj) {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj, "fan_carousel");
    if (!carousel) {
        spdlog::error("[FanStackWidget] Could not find fan_carousel in XML");
        return;
    }

    setup_common_observers(
        [this]() {
            for (auto& page : carousel_pages_)
                update_fan_animation(page.fan_icon, page.arc ? lv_arc_get_value(page.arc) : 0);
        },
        [this]() { bind_carousel_fans(); });
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
    for (auto* icon : {part_icon_, hotend_icon_, aux_icon_})
        if (icon)
            helix::ui::fan_spin_stop(icon);
    for (auto& page : carousel_pages_)
        if (page.fan_icon)
            helix::ui::fan_spin_stop(page.fan_icon);
    carousel_pages_.clear();

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

    // Bind part fan
    part_observer_ = bind_fan_observer(part_fan_name_, [this](int speed) {
        part_speed_ = speed;
        update_label(part_label_, speed);
        update_fan_animation(part_icon_, speed);
    });

    // Bind hotend fan
    hotend_observer_ = bind_fan_observer(hotend_fan_name_, [this](int speed) {
        hotend_speed_ = speed;
        update_label(hotend_label_, speed);
        update_fan_animation(hotend_icon_, speed);
    });

    // Bind aux fan (hide row if none)
    if (!aux_fan_name_.empty() && aux_row_)
        lv_obj_remove_flag(aux_row_, LV_OBJ_FLAG_HIDDEN);
    else if (aux_row_)
        lv_obj_add_flag(aux_row_, LV_OBJ_FLAG_HIDDEN);

    aux_observer_ = bind_fan_observer(aux_fan_name_, [this](int speed) {
        aux_speed_ = speed;
        update_label(aux_label_, speed);
        update_fan_animation(aux_icon_, speed);
    });

    spdlog::debug("[FanStackWidget] Bound fans: part='{}' hotend='{}' aux='{}'", part_fan_name_,
                  hotend_fan_name_, aux_fan_name_);
}

void FanStackWidget::bind_carousel_fans() {
    if (!widget_obj_)
        return;

    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "fan_carousel");
    if (!carousel)
        return;

    // Freeze the update queue while tearing down observers and widgets to
    // prevent the WebSocket thread from enqueuing callbacks for destroyed objects.
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        part_observer_.reset();
        hotend_observer_.reset();
        aux_observer_.reset();
        carousel_observers_.clear();
        for (auto& page : carousel_pages_) {
            if (page.fan_icon)
                helix::ui::fan_spin_stop(page.fan_icon);
        }
        carousel_pages_.clear();

        // Clear existing carousel pages (the carousel may have pages from a previous bind)
        auto* state_ptr = ui_carousel_get_state(carousel);
        if (state_ptr && state_ptr->scroll_container) {
            helix::ui::UpdateQueue::instance().drain();
            lv_obj_clean(state_ptr->scroll_container);
            state_ptr->real_tiles.clear();
            ui_carousel_rebuild_indicators(carousel);
        }
    }

    const auto& fans = printer_state_.get_fans();

    // When no fans are discovered yet (e.g. disconnected), use placeholder
    // entries so the carousel still shows arc widgets at 0%.
    struct FanEntry {
        std::string display_name;
        std::string object_name;
        int speed_percent;
        bool is_controllable;
    };
    std::vector<FanEntry> entries;
    if (fans.empty()) {
        entries.push_back({lv_tr("Part"), "", 0, false});
        entries.push_back({"Hotend", "", 0, false});
        spdlog::debug("[FanStackWidget] Carousel: no fans discovered, using placeholders");
    } else {
        for (const auto& fan : fans) {
            std::string short_name = fan.display_name;
            auto pos = short_name.find(" Fan");
            if (pos != std::string::npos && short_name.size() > 4)
                short_name.erase(pos, 4);
            entries.push_back(
                {short_name, fan.object_name, fan.speed_percent, fan.is_controllable});
        }
    }

    const lv_font_t* xs_font = theme_manager_get_font("font_xs");
    lv_color_t text_muted = theme_manager_get_color("text_muted");

    for (const auto& entry : entries) {
        // Thin wrapper page: column layout with arc core + tiny name label
        lv_obj_t* page = lv_obj_create(lv_scr_act());
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_pad_gap(page, 0, 0);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_flex_cross_place(page, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        // Create the core arc widget (no card chrome, no buttons)
        char val_str[16];
        snprintf(val_str, sizeof(val_str), "%d", entry.speed_percent);
        const char* attrs[] = {"initial_value", val_str, nullptr};
        lv_obj_t* arc_core = static_cast<lv_obj_t*>(lv_xml_create(page, "fan_arc_core", attrs));
        if (!arc_core) {
            spdlog::error("[FanStackWidget] lv_xml_create('fan_arc_core') returned NULL for '{}'",
                          entry.display_name);
            lv_obj_delete(page);
            continue;
        }

        // XML component root views don't propagate their name attribute to
        // lv_obj_set_name(), but fan_arc_resize_to_fit() needs to find
        // "dial_container" by name. Set it explicitly.
        lv_obj_set_name_static(arc_core, "dial_container");

        // fan_arc_core uses token-based sizing for card contexts; carousel
        // needs it to fill the tile instead.
        lv_obj_set_size(arc_core, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_grow(arc_core, 1);

        lv_obj_t* name_lbl = lv_label_create(page);
        lv_label_set_text(name_lbl, entry.display_name.c_str());
        lv_obj_set_style_text_color(name_lbl, text_muted, 0);
        if (xs_font)
            lv_obj_set_style_text_font(name_lbl, xs_font, 0);

        // Cache arc, label, and icon pointers for observer updates
        CarouselPage cp;
        cp.arc = lv_obj_find_by_name(arc_core, "dial_arc");
        cp.speed_label = lv_obj_find_by_name(arc_core, "speed_label");
        cp.fan_icon = lv_obj_find_by_name(arc_core, "fan_icon");

        // Shrink speed label font for compact display
        if (xs_font && cp.speed_label)
            lv_obj_set_style_text_font(cp.speed_label, xs_font, 0);

        set_icon_pivot(cp.fan_icon);

        // Shrink knob for compact carousel display
        if (cp.arc) {
            lv_obj_set_style_pad_all(cp.arc, 2, LV_PART_KNOB);
        }

        // Auto-controlled fans: hide knob, disable arc interaction
        if (!entry.is_controllable && cp.arc) {
            lv_obj_remove_flag(cp.arc, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(cp.arc, LV_OPA_TRANSP, LV_PART_KNOB);
            lv_obj_set_style_shadow_width(cp.arc, 0, LV_PART_KNOB);
            lv_obj_set_style_outline_width(cp.arc, 0, LV_PART_KNOB);
        }

        // Make whole page clickable → open fan control overlay
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(page, this);
        lv_obj_add_event_cb(
            page,
            [](lv_event_t* e) {
                auto* self = static_cast<FanStackWidget*>(lv_event_get_user_data(e));
                if (self)
                    self->handle_clicked();
            },
            LV_EVENT_CLICKED, this);

        ui_carousel_add_item(carousel, page);

        size_t page_idx = carousel_pages_.size();
        carousel_pages_.push_back(cp);

        // Observe fan speed → update arc value + label text + spin animation
        // (skip for placeholders with no object_name)
        if (entry.object_name.empty())
            continue;

        auto obs = bind_fan_observer(entry.object_name, [this, page_idx](int speed) {
            if (page_idx >= carousel_pages_.size())
                return;
            auto& cp = carousel_pages_[page_idx];
            if (cp.arc)
                lv_arc_set_value(cp.arc, speed);
            if (cp.speed_label) {
                char buf[8];
                lv_label_set_text(cp.speed_label,
                                  lv_tr(helix::format::format_fan_speed(speed, buf, sizeof(buf))));
            }
            update_fan_animation(cp.fan_icon, speed);
        });
        if (obs)
            carousel_observers_.push_back(std::move(obs));
    }

    // Attach auto-resize AFTER all pages are reparented into the carousel.
    // Doing it inside the loop above would trigger an initial resize before the
    // carousel layout is finalized, resulting in 0x0 container dimensions and
    // the arc collapsing to MIN_ARC_SIZE.
    lv_obj_update_layout(carousel);
    auto* state = ui_carousel_get_state(carousel);
    if (state && state->scroll_container) {
        uint32_t child_count = lv_obj_get_child_count(state->scroll_container);
        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* child = lv_obj_get_child(state->scroll_container, static_cast<int32_t>(i));
            if (child) {
                helix::ui::fan_arc_attach_auto_resize(child);
            }
        }
    }

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[FanStackWidget] Carousel bound {} fan pages", page_count);
}

void FanStackWidget::set_icon_pivot(lv_obj_t* icon) {
    if (icon) {
        lv_obj_set_style_transform_pivot_x(icon, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(icon, LV_PCT(50), 0);
    }
}

ObserverGuard FanStackWidget::bind_fan_observer(const std::string& fan_name,
                                                std::function<void(int speed)> on_update) {
    if (fan_name.empty())
        return {};

    SubjectLifetime lifetime;
    lv_subject_t* subject = printer_state_.get_fan_speed_subject(fan_name, lifetime);
    if (!subject)
        return {};

    std::weak_ptr<bool> weak_alive = alive_;
    auto guard = helix::ui::observe_int_sync<FanStackWidget>(
        subject, this,
        [weak_alive, on_update](FanStackWidget* /*self*/, int speed) {
            if (weak_alive.expired())
                return;
            on_update(speed);
        },
        lifetime);

    // Read current value immediately — the deferred observer initial fire
    // is dropped when populate_widgets() freezes the update queue.
    on_update(lv_subject_get_int(subject));
    return guard;
}

void FanStackWidget::setup_common_observers(std::function<void()> on_anim_changed,
                                            std::function<void()> on_fans_version) {
    animations_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();

    std::weak_ptr<bool> weak_alive = alive_;
    anim_settings_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        DisplaySettingsManager::instance().subject_animations_enabled(), this,
        [weak_alive, on_anim_changed](FanStackWidget* self, int enabled) {
            if (weak_alive.expired())
                return;
            self->animations_enabled_ = (enabled != 0);
            on_anim_changed();
        });

    version_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        printer_state_.get_fans_version_subject(), this,
        [weak_alive, on_fans_version](FanStackWidget* /*self*/, int /*version*/) {
            if (weak_alive.expired())
                return;
            on_fans_version();
        });
}

void FanStackWidget::update_label(lv_obj_t* label, int speed_pct) {
    if (!label)
        return;

    char buf[8];
    helix::format::format_percent(speed_pct, buf, sizeof(buf));
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
