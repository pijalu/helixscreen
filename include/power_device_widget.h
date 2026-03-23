// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "panel_widget.h"

#include <memory>
#include <string>

class MoonrakerAPI;

namespace helix {

/// Home panel widget for toggling individual Moonraker power devices.
/// Uses the multi_instance system: base ID "power_device" with dynamic
/// instance IDs like "power_device:1", "power_device:2", etc.
/// Tap toggles device power; configure button opens device picker.
/// When unconfigured, tap also opens picker.
class PowerDeviceWidget : public PanelWidget {
  public:
    explicit PowerDeviceWidget(const std::string& instance_id);
    ~PowerDeviceWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;
    std::string get_component_name() const override {
        return "panel_widget_power_device";
    }
    const char* id() const override {
        return instance_id_.c_str();
    }

    void handle_clicked();
    static void power_device_clicked_cb(lv_event_t* e);

  private:
    std::string instance_id_;
    std::string device_name_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* badge_obj_ = nullptr;
    lv_obj_t* icon_obj_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* lock_icon_ = nullptr;

    SubjectLifetime subject_lifetime_;
    ObserverGuard status_observer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    // Picker state
    lv_obj_t* picker_backdrop_ = nullptr;
    static PowerDeviceWidget* s_active_picker_;

    MoonrakerAPI* get_api() const;
    void update_display(int status);
    void show_device_picker();
    void dismiss_device_picker();
    void select_device(const std::string& name);
    void save_config();
};

} // namespace helix
