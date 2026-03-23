// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermistor_widget.h"

#include "ui_carousel.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <set>

namespace helix {
void register_thermistor_widget() {
    register_widget_factory("thermistor", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<ThermistorWidget>(ps);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "thermistor_clicked_cb",
                             ThermistorWidget::thermistor_clicked_cb);
    lv_xml_register_event_cb(nullptr, "thermistor_picker_backdrop_cb",
                             ThermistorWidget::thermistor_picker_backdrop_cb);
    lv_xml_register_event_cb(nullptr, "thermistor_picker_done_cb",
                             ThermistorWidget::thermistor_picker_done_cb);
}
} // namespace helix

using namespace helix;
using helix::ui::temperature::centi_to_degrees_f;
using helix::ui::temperature::format_temperature_f;

/// Strip redundant " Temperature" suffix — the widget context already implies it
static void strip_temperature_suffix(std::string& name) {
    const std::string suffix = " Temperature";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name.erase(name.size() - suffix.size());
    }
}

namespace {

/// Resolve a responsive spacing token to pixels, with a fallback.
int resolve_space_token(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// Free heap-allocated klipper_name strings stored as user_data on picker rows.
void cleanup_picker_row_strings(lv_obj_t* backdrop) {
    lv_obj_t* sensor_list = lv_obj_find_by_name(backdrop, "sensor_list");
    if (!sensor_list)
        return;
    uint32_t count = lv_obj_get_child_count(sensor_list);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* row = lv_obj_get_child(sensor_list, i);
        auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
        delete name_ptr;
        lv_obj_set_user_data(row, nullptr);
    }
}

/// Create a sensor row in a picker list. Returns the row object.
/// @param is_multi  If true, adds a checkbox; if false, adds highlight styling.
lv_obj_t* create_sensor_row(lv_obj_t* list, const std::string& display_name,
                            const std::string& klipper_name, bool is_selected, bool is_multi,
                            int space_sm, int space_xs) {
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();

    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, space_sm, 0);
    lv_obj_set_style_pad_gap(row, space_xs, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    if (is_multi) {
        // Checkbox for multi-select
        lv_obj_t* cb = lv_checkbox_create(row);
        lv_checkbox_set_text(cb, "");
        lv_obj_set_style_pad_all(cb, 0, 0);
        if (is_selected) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
        lv_obj_remove_flag(cb, LV_OBJ_FLAG_CLICKABLE);
    } else {
        // Highlight for single-select
        lv_obj_set_style_bg_opa(row, is_selected ? 30 : 0, 0);
    }

    // Sensor display name
    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, display_name.c_str());
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name, lv_font_get_default(), 0);

    // Current temperature
    auto state = tsm.get_sensor_state(klipper_name);
    char temp_buf[16];
    if (state && state->available) {
        helix::ui::temperature::format_temperature(static_cast<int>(state->temperature), temp_buf,
                                                   sizeof(temp_buf));
    } else {
        std::strcpy(temp_buf, "--\xC2\xB0"
                              "C");
    }
    lv_obj_t* temp = lv_label_create(row);
    lv_label_set_text(temp, temp_buf);
    lv_obj_set_style_text_font(temp, lv_font_get_default(), 0);
    lv_obj_set_style_text_opa(temp, 180, 0);

    return row;
}

/// Position a context_menu card near a widget, clamped to screen.
void position_picker_card(lv_obj_t* backdrop, lv_obj_t* widget_obj, lv_obj_t* parent_screen,
                          int card_w) {
    lv_obj_t* card = lv_obj_find_by_name(backdrop, "context_menu");
    if (!card || !widget_obj)
        return;

    int space_xs = resolve_space_token("space_xs", 4);
    int space_md = resolve_space_token("space_md", 10);
    int screen_w = lv_obj_get_width(parent_screen);
    int screen_h = lv_obj_get_height(parent_screen);

    lv_obj_set_width(card, card_w);
    lv_obj_set_style_max_height(card, screen_h * 80 / 100, 0);
    lv_obj_update_layout(card);
    int card_h = lv_obj_get_height(card);

    lv_area_t widget_area;
    lv_obj_get_coords(widget_obj, &widget_area);

    int card_x = (widget_area.x1 + widget_area.x2) / 2 - card_w / 2;
    int card_y = widget_area.y2 + space_xs;

    if (card_x < space_md)
        card_x = space_md;
    if (card_x + card_w > screen_w - space_md)
        card_x = screen_w - card_w - space_md;
    if (card_y + card_h > screen_h - space_md) {
        card_y = widget_area.y1 - card_h - space_xs;
        if (card_y < space_md)
            card_y = space_md;
    }

    lv_obj_set_pos(card, card_x, card_y);
}

} // anonymous namespace

ThermistorWidget::ThermistorWidget(PrinterState& /*printer_state*/) {
    std::strcpy(temp_buffer_, "--\xC2\xB0"
                              "C"); // "--°C"
}

ThermistorWidget::~ThermistorWidget() {
    detach();
}

void ThermistorWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, this);
    }

    if (is_carousel_mode()) {
        attach_carousel();
    } else {
        attach_single();
    }
}

void ThermistorWidget::attach_single() {
    // Cache label pointers
    temp_label_ = lv_obj_find_by_name(widget_obj_, "thermistor_temp");
    name_label_ = lv_obj_find_by_name(widget_obj_, "thermistor_name");

    // If no sensor saved (set_config provided none), auto-select first available
    if (selected_sensor_.empty()) {
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        auto sensors = tsm.get_sensors_sorted();
        if (!sensors.empty()) {
            select_sensor(sensors.front().klipper_name);
        }
    } else {
        // Re-bind observer to saved sensor
        auto& tsm = helix::sensors::TemperatureSensorManager::instance();
        SubjectLifetime lifetime;
        lv_subject_t* subject = tsm.get_temp_subject(selected_sensor_, lifetime);
        if (subject) {
            std::weak_ptr<bool> weak_alive = alive_;
            temp_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
                subject, this,
                [weak_alive](ThermistorWidget* self, int temp) {
                    if (weak_alive.expired())
                        return;
                    self->on_temp_changed(temp);
                },
                lifetime);
        }
        update_display();
    }

    spdlog::debug("[ThermistorWidget] Attached single (sensor: {})",
                  selected_sensor_.empty() ? "none" : selected_sensor_);
}

void ThermistorWidget::attach_carousel() {
    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "thermistor_carousel");
    if (!carousel) {
        spdlog::error("[ThermistorWidget] Could not find thermistor_carousel in XML");
        return;
    }

    // Observe sensor count to rebind when sensors are discovered
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    std::weak_ptr<bool> weak_alive = alive_;
    version_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
        tsm.get_sensor_count_subject(), this, [weak_alive](ThermistorWidget* self, int /*count*/) {
            if (weak_alive.expired())
                return;
            self->bind_carousel_sensors();
        });

    // Bind immediately (deferred observer fire may be dropped during populate_widgets freeze)
    bind_carousel_sensors();

    spdlog::debug("[ThermistorWidget] Attached carousel ({} sensors)", sensors_.size());
}

void ThermistorWidget::bind_carousel_sensors() {
    if (!widget_obj_)
        return;

    // Reentrancy guard: drain() can process a queued version_observer callback
    // that recursively calls bind_carousel_sensors(). Without this guard, the
    // recursive call creates pages that the outer call then destroys via
    // lv_obj_clean(), leaving stale temp_label pointers in carousel_pages_.
    if (binding_in_progress_)
        return;
    binding_in_progress_ = true;

    lv_obj_t* carousel = lv_obj_find_by_name(widget_obj_, "thermistor_carousel");
    if (!carousel) {
        binding_in_progress_ = false;
        return;
    }

    // Freeze update queue while tearing down observers and widgets
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        carousel_observers_.clear();
        carousel_pages_.clear();

        auto* state_ptr = ui_carousel_get_state(carousel);
        if (state_ptr && state_ptr->scroll_container) {
            helix::ui::UpdateQueue::instance().drain();
            lv_obj_clean(state_ptr->scroll_container);
            state_ptr->real_tiles.clear();
            ui_carousel_rebuild_indicators(carousel);
        }
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();

    // Determine which sensors to show
    std::vector<std::string> display_sensors;
    if (sensors_.empty()) {
        // Show all available sensors
        for (const auto& s : tsm.get_sensors_sorted()) {
            display_sensors.push_back(s.klipper_name);
        }
    } else {
        display_sensors = sensors_;
    }

    if (display_sensors.empty()) {
        spdlog::debug("[ThermistorWidget] Carousel: no sensors available");
        return;
    }

    std::weak_ptr<bool> weak_alive = alive_;
    const lv_font_t* xs_font = theme_manager_get_font("font_xs");
    lv_color_t text_muted = theme_manager_get_color("text_muted");

    for (const auto& klipper_name : display_sensors) {
        // Resolve display name
        std::string display_name = klipper_name;
        for (const auto& s : tsm.get_sensors_sorted()) {
            if (s.klipper_name == klipper_name) {
                display_name = s.display_name;
                break;
            }
        }
        strip_temperature_suffix(display_name);

        // Create page: column layout with icon + temp + name
        lv_obj_t* page = lv_obj_create(lv_scr_act());
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_style_pad_gap(page, 0, 0);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

        // Thermometer icon
        const char* icon_attrs[] = {"src",     "thermometer", "size", "sm",
                                    "variant", "secondary",   nullptr};
        lv_xml_create(page, "icon", icon_attrs);

        // Temperature label
        lv_obj_t* temp_lbl = lv_label_create(page);
        lv_label_set_text(temp_lbl, "--\xC2\xB0"
                                    "C");
        lv_obj_set_style_text_font(temp_lbl, lv_font_get_default(), 0);

        // Sensor name label
        lv_obj_t* name_lbl = lv_label_create(page);
        lv_label_set_text(name_lbl, display_name.c_str());
        lv_obj_set_style_text_color(name_lbl, text_muted, 0);
        if (xs_font)
            lv_obj_set_style_text_font(name_lbl, xs_font, 0);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name_lbl, LV_PCT(100));
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

        // Make page clickable to open multi-select picker
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(page, this);
        lv_obj_add_event_cb(
            page,
            [](lv_event_t* e) {
                auto* self = static_cast<ThermistorWidget*>(lv_event_get_user_data(e));
                if (self)
                    self->show_configure_picker();
            },
            LV_EVENT_CLICKED, this);

        ui_carousel_add_item(carousel, page);

        // Track page
        size_t page_idx = carousel_pages_.size();
        CarouselPage cp;
        cp.temp_label = temp_lbl;
        cp.name_label = name_lbl;
        std::strcpy(cp.temp_buffer, "--\xC2\xB0"
                                    "C");
        carousel_pages_.push_back(cp);

        // Observe temperature subject
        SubjectLifetime lifetime;
        lv_subject_t* subject = tsm.get_temp_subject(klipper_name, lifetime);
        if (subject) {
            auto obs = helix::ui::observe_int_sync<ThermistorWidget>(
                subject, this,
                [weak_alive, page_idx](ThermistorWidget* self, int centidegrees) {
                    if (weak_alive.expired())
                        return;
                    if (page_idx >= self->carousel_pages_.size())
                        return;
                    auto& cp = self->carousel_pages_[page_idx];
                    float deg = centi_to_degrees_f(centidegrees);
                    format_temperature_f(deg, cp.temp_buffer, sizeof(cp.temp_buffer));
                    if (cp.temp_label)
                        lv_label_set_text(cp.temp_label, cp.temp_buffer);
                },
                lifetime);

            // Read current value immediately
            int current = lv_subject_get_int(subject);
            float deg = centi_to_degrees_f(current);
            format_temperature_f(deg, carousel_pages_.back().temp_buffer,
                                 sizeof(carousel_pages_.back().temp_buffer));
            lv_label_set_text(temp_lbl, carousel_pages_.back().temp_buffer);

            carousel_observers_.push_back(std::move(obs));
        }
    }

    int page_count = ui_carousel_get_page_count(carousel);
    spdlog::debug("[ThermistorWidget] Carousel bound {} sensor pages", page_count);

    binding_in_progress_ = false;
}

void ThermistorWidget::detach() {
    *alive_ = false;
    dismiss_sensor_picker();
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        version_observer_.reset();
        carousel_observers_.clear();
        carousel_pages_.clear();
        temp_observer_.reset();
    }

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;
    temp_label_ = nullptr;
    name_label_ = nullptr;

    spdlog::debug("[ThermistorWidget] Detached");
}

void ThermistorWidget::handle_clicked() {
    if (is_carousel_mode()) {
        spdlog::info("[ThermistorWidget] Clicked carousel - showing configure picker");
        show_configure_picker();
    } else {
        spdlog::info("[ThermistorWidget] Clicked - showing sensor picker");
        show_sensor_picker();
    }
}

void ThermistorWidget::resolve_display_name() {
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    display_name_ = selected_sensor_; // fallback
    for (const auto& s : tsm.get_sensors_sorted()) {
        if (s.klipper_name == selected_sensor_) {
            display_name_ = s.display_name;
            break;
        }
    }
    strip_temperature_suffix(display_name_);
}

void ThermistorWidget::select_sensor(const std::string& klipper_name) {
    if (klipper_name == selected_sensor_) {
        return;
    }

    // Reset existing observer
    temp_observer_.reset();

    selected_sensor_ = klipper_name;
    if (!is_carousel_mode()) {
        sensors_ = {klipper_name};
    }
    resolve_display_name();

    // Subscribe to this sensor's temperature subject
    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    SubjectLifetime lifetime;
    lv_subject_t* subject = tsm.get_temp_subject(klipper_name, lifetime);
    if (subject) {
        std::weak_ptr<bool> weak_alive = alive_;
        temp_observer_ = helix::ui::observe_int_sync<ThermistorWidget>(
            subject, this,
            [weak_alive](ThermistorWidget* self, int temp) {
                if (weak_alive.expired())
                    return;
                self->on_temp_changed(temp);
            },
            lifetime);
    } else {
        spdlog::warn("[ThermistorWidget] No subject for sensor: {}", klipper_name);
    }

    update_display();
    save_config();

    spdlog::info("[ThermistorWidget] Selected sensor: {} ({})", display_name_, klipper_name);
}

void ThermistorWidget::on_temp_changed(int centidegrees) {
    float deg = centi_to_degrees_f(centidegrees);
    format_temperature_f(deg, temp_buffer_, sizeof(temp_buffer_));

    if (temp_label_) {
        lv_label_set_text(temp_label_, temp_buffer_);
    }

    spdlog::trace("[ThermistorWidget] {} = {:.1f}°C", display_name_, deg);
}

void ThermistorWidget::update_display() {
    if (temp_label_) {
        if (selected_sensor_.empty()) {
            lv_label_set_text(temp_label_, "--\xC2\xB0"
                                           "C");
        } else {
            // Read current value from subject
            auto& tsm = helix::sensors::TemperatureSensorManager::instance();
            SubjectLifetime lifetime;
            lv_subject_t* subject = tsm.get_temp_subject(selected_sensor_, lifetime);
            if (subject) {
                float deg = centi_to_degrees_f(lv_subject_get_int(subject));
                format_temperature_f(deg, temp_buffer_, sizeof(temp_buffer_));
                lv_label_set_text(temp_label_, temp_buffer_);
            } else {
                lv_label_set_text(temp_label_, "--\xC2\xB0"
                                               "C");
            }
        }
    }

    if (name_label_) {
        if (selected_sensor_.empty()) {
            lv_label_set_text(name_label_, lv_tr("Select sensor"));
        } else {
            lv_label_set_text(name_label_, display_name_.c_str());
        }
    }
}

void ThermistorWidget::set_config(const nlohmann::json& config) {
    config_ = config;

    // Migrate old format: {"sensor": "x"} → sensors_ vector
    if (config.contains("sensor") && config["sensor"].is_string()) {
        selected_sensor_ = config["sensor"].get<std::string>();
        sensors_ = {selected_sensor_};
        resolve_display_name();
        spdlog::debug("[ThermistorWidget] Config: sensor={}", selected_sensor_);
    }
    // New format: {"sensors": [...], "display_mode": "carousel"}
    if (config.contains("sensors") && config["sensors"].is_array()) {
        sensors_.clear();
        for (const auto& s : config["sensors"]) {
            if (s.is_string()) {
                sensors_.push_back(s.get<std::string>());
            }
        }
        if (!sensors_.empty()) {
            selected_sensor_ = sensors_.front();
            resolve_display_name();
        }
        spdlog::debug("[ThermistorWidget] Config: {} sensors, mode={}", sensors_.size(),
                      is_carousel_mode() ? "carousel" : "single");
    }
}

std::string ThermistorWidget::get_component_name() const {
    if (is_carousel_mode()) {
        return "panel_widget_thermistor_carousel";
    }
    return "panel_widget_thermistor";
}

bool ThermistorWidget::on_edit_configure() {
    spdlog::info("[ThermistorWidget] Configure requested - showing sensor picker");
    show_configure_picker();
    return false; // no rebuild — picker updates and saves on Done
}

bool ThermistorWidget::is_carousel_mode() const {
    if (config_.contains("display_mode") && config_["display_mode"].is_string()) {
        return config_["display_mode"].get<std::string>() == "carousel";
    }
    return false;
}

void ThermistorWidget::save_config() {
    nlohmann::json config = config_;
    // Always write new format
    config["sensors"] = sensors_;
    config.erase("sensor"); // remove legacy key
    if (is_carousel_mode()) {
        config["display_mode"] = "carousel";
    }
    config_ = config;
    save_widget_config(config);
    spdlog::debug("[ThermistorWidget] Saved config: {} sensors, mode={}", sensors_.size(),
                  is_carousel_mode() ? "carousel" : "single");
}

void ThermistorWidget::show_sensor_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = tsm.get_sensors_sorted();
    if (sensors.empty()) {
        spdlog::warn("[ThermistorWidget] No sensors available for picker");
        return;
    }

    // Create picker from existing single-select XML
    picker_backdrop_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "thermistor_sensor_picker", nullptr));
    if (!picker_backdrop_) {
        spdlog::error("[ThermistorWidget] Failed to create sensor picker from XML");
        return;
    }

    lv_obj_t* sensor_list = lv_obj_find_by_name(picker_backdrop_, "sensor_list");
    if (!sensor_list) {
        spdlog::error("[ThermistorWidget] sensor_list not found in picker XML");
        helix::ui::safe_delete(picker_backdrop_);
        picker_backdrop_ = nullptr;
        return;
    }

    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);
    int screen_h = lv_obj_get_height(parent_screen_);
    lv_obj_set_style_max_height(sensor_list, screen_h * 2 / 3, 0);

    for (const auto& sensor : sensors) {
        bool is_selected = (sensor.klipper_name == selected_sensor_);
        lv_obj_t* row = create_sensor_row(sensor_list, sensor.display_name, sensor.klipper_name,
                                          is_selected, false, space_sm, space_xs);

        // Store klipper_name for click handler
        auto* klipper_name_copy = new std::string(sensor.klipper_name);
        lv_obj_set_user_data(row, klipper_name_copy);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] sensor_row_cb");
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(target));
                if (!name_ptr)
                    return;

                if (ThermistorWidget::s_active_picker_ &&
                    *ThermistorWidget::s_active_picker_->alive_) {
                    std::string sensor_name = *name_ptr;
                    ThermistorWidget::s_active_picker_->select_sensor(sensor_name);
                    ThermistorWidget::s_active_picker_->dismiss_sensor_picker();
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);
    }

    s_active_picker_ = this;

    // Self-clearing delete callback with heap string cleanup
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* e) {
            auto* self = static_cast<ThermistorWidget*>(lv_event_get_user_data(e));
            if (self) {
                lv_obj_t* backdrop = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                cleanup_picker_row_strings(backdrop);
                self->picker_backdrop_ = nullptr;
                if (s_active_picker_ == self) {
                    s_active_picker_ = nullptr;
                }
            }
        },
        LV_EVENT_DELETE, this);

    // Position card
    int screen_w = lv_obj_get_width(parent_screen_);
    int card_w = std::clamp(screen_w * 3 / 10, 160, 240);
    position_picker_card(picker_backdrop_, widget_obj_, parent_screen_, card_w);

    spdlog::debug("[ThermistorWidget] Sensor picker shown with {} sensors", sensors.size());
}

void ThermistorWidget::dismiss_sensor_picker() {
    if (!picker_backdrop_) {
        return;
    }

    // Nullify pointers BEFORE delete — the DELETE handler does cleanup
    // as a safety net (also handles parent-deletion case)
    lv_obj_t* backdrop = picker_backdrop_;
    picker_backdrop_ = nullptr;
    s_active_picker_ = nullptr;

    if (lv_obj_is_valid(backdrop)) {
        helix::ui::safe_delete(backdrop);
    }

    spdlog::debug("[ThermistorWidget] Sensor picker dismissed");
}

void ThermistorWidget::show_configure_picker() {
    if (picker_backdrop_ || !parent_screen_) {
        return;
    }

    auto& tsm = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = tsm.get_sensors_sorted();
    if (sensors.empty()) {
        spdlog::warn("[ThermistorWidget] No sensors available for picker");
        return;
    }

    // Create picker from new XML with header + Done button
    picker_backdrop_ = static_cast<lv_obj_t*>(
        lv_xml_create(parent_screen_, "thermistor_configure_picker", nullptr));
    if (!picker_backdrop_) {
        spdlog::error("[ThermistorWidget] Failed to create configure picker from XML");
        return;
    }

    lv_obj_t* sensor_list = lv_obj_find_by_name(picker_backdrop_, "sensor_list");
    if (!sensor_list) {
        spdlog::error("[ThermistorWidget] sensor_list not found in picker XML");
        helix::ui::safe_delete(picker_backdrop_);
        picker_backdrop_ = nullptr;
        return;
    }

    int space_xs = resolve_space_token("space_xs", 4);
    int space_sm = resolve_space_token("space_sm", 6);
    int screen_h = lv_obj_get_height(parent_screen_);
    lv_obj_set_style_max_height(sensor_list, screen_h * 2 / 3, 0);

    // Build set of currently selected sensors
    std::set<std::string> selected_set(sensors_.begin(), sensors_.end());

    for (const auto& sensor : sensors) {
        bool selected = selected_set.count(sensor.klipper_name) > 0;
        lv_obj_t* row = create_sensor_row(sensor_list, sensor.display_name, sensor.klipper_name,
                                          selected, true, space_sm, space_xs);

        // Store klipper_name for Done handler to read back
        auto* klipper_name_copy = new std::string(sensor.klipper_name);
        lv_obj_set_user_data(row, klipper_name_copy);

        // Click row to toggle checkbox
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                uint32_t count = lv_obj_get_child_count(target);
                for (uint32_t i = 0; i < count; ++i) {
                    lv_obj_t* child = lv_obj_get_child(target, static_cast<int32_t>(i));
                    if (lv_obj_check_type(child, &lv_checkbox_class)) {
                        if (lv_obj_has_state(child, LV_STATE_CHECKED)) {
                            lv_obj_remove_state(child, LV_STATE_CHECKED);
                        } else {
                            lv_obj_add_state(child, LV_STATE_CHECKED);
                        }
                        break;
                    }
                }
            },
            LV_EVENT_CLICKED, nullptr);
    }

    s_active_picker_ = this;

    // Self-clearing delete callback with heap string cleanup
    lv_obj_add_event_cb(
        picker_backdrop_,
        [](lv_event_t* e) {
            auto* self = static_cast<ThermistorWidget*>(lv_event_get_user_data(e));
            if (self) {
                lv_obj_t* backdrop = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                cleanup_picker_row_strings(backdrop);
                self->picker_backdrop_ = nullptr;
                if (s_active_picker_ == self) {
                    s_active_picker_ = nullptr;
                }
            }
        },
        LV_EVENT_DELETE, this);

    // Position card — wider for the header
    int screen_w = lv_obj_get_width(parent_screen_);
    int card_w = std::clamp(screen_w * 35 / 100, 180, 280);
    position_picker_card(picker_backdrop_, widget_obj_, parent_screen_, card_w);

    spdlog::debug("[ThermistorWidget] Configure picker shown with {} sensors", sensors.size());
}

void ThermistorWidget::apply_sensor_selection(const std::vector<std::string>& selected) {
    if (selected.empty()) {
        spdlog::warn("[ThermistorWidget] Cannot apply empty sensor selection — keeping previous");
        return;
    }

    sensors_ = selected;
    if (!sensors_.empty()) {
        selected_sensor_ = sensors_.front();
        resolve_display_name();
    }

    // Auto-detect mode: multiple sensors → carousel, single → single
    bool was_carousel = is_carousel_mode();
    bool want_carousel = sensors_.size() > 1;
    if (want_carousel) {
        config_["display_mode"] = "carousel";
    } else {
        config_.erase("display_mode");
    }

    save_config();

    if (was_carousel != want_carousel) {
        // Mode changed — need a full widget rebuild (different XML component).
        // If we're in edit mode, the rebuild is deferred to edit-mode exit.
        spdlog::info("[ThermistorWidget] Mode changed to {} — requesting rebuild",
                     want_carousel ? "carousel" : "single");
        PanelWidgetManager::instance().notify_config_changed(panel_id());
    } else if (is_carousel_mode()) {
        // Same mode, just different sensor selection — rebind inline
        bind_carousel_sensors();
    } else if (!sensors_.empty()) {
        select_sensor(sensors_.front());
    }

    spdlog::info("[ThermistorWidget] Applied sensor selection: {} sensors (mode={})",
                 sensors_.size(), is_carousel_mode() ? "carousel" : "single");
}

void ThermistorWidget::thermistor_picker_done_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] thermistor_picker_done_cb");
    (void)e;
    if (!s_active_picker_ || !*s_active_picker_->alive_)
        return;

    auto* self = s_active_picker_;
    auto* backdrop = self->picker_backdrop_;
    if (!backdrop)
        return;

    // Collect selected sensors from checkboxes
    lv_obj_t* sensor_list = lv_obj_find_by_name(backdrop, "sensor_list");
    if (!sensor_list)
        return;

    std::vector<std::string> selected;
    uint32_t count = lv_obj_get_child_count(sensor_list);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* row = lv_obj_get_child(sensor_list, static_cast<int32_t>(i));
        auto* name_ptr = static_cast<std::string*>(lv_obj_get_user_data(row));
        if (!name_ptr)
            continue;

        uint32_t row_children = lv_obj_get_child_count(row);
        for (uint32_t j = 0; j < row_children; ++j) {
            lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(j));
            if (lv_obj_check_type(child, &lv_checkbox_class)) {
                if (lv_obj_has_state(child, LV_STATE_CHECKED)) {
                    selected.push_back(*name_ptr);
                }
                break;
            }
        }
    }

    self->apply_sensor_selection(selected);
    self->dismiss_sensor_picker();
    LVGL_SAFE_EVENT_CB_END();
}

// Static callbacks
void ThermistorWidget::thermistor_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] thermistor_clicked_cb");
    auto* widget = panel_widget_from_event<ThermistorWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void ThermistorWidget::thermistor_picker_backdrop_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ThermistorWidget] thermistor_picker_backdrop_cb");
    (void)e;
    if (s_active_picker_ && *s_active_picker_->alive_) {
        s_active_picker_->dismiss_sensor_picker();
    }
    LVGL_SAFE_EVENT_CB_END();
}

// Static instance for picker callbacks
ThermistorWidget* ThermistorWidget::s_active_picker_ = nullptr;
