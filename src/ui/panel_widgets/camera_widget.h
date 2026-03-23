// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "camera_stream.h"
#include "observer_factory.h"
#include "panel_widget.h"

#include <memory>

namespace helix {

class CameraConfigModal;

class CameraWidget : public PanelWidget {
  public:
    CameraWidget();
    ~CameraWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override { return "camera"; }
    void set_config(const nlohmann::json& config) override;
    bool has_edit_configure() const override { return true; }
    bool on_edit_configure() override;

    void on_activate() override;
    void on_deactivate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool has_overlay_open() const override { return fullscreen_overlay_ != nullptr; }

    /// Open/close fullscreen camera overlay
    void open_fullscreen();
    void close_fullscreen();

  private:
    void start_stream();
    void stop_stream();
    void apply_transform(); // Push rotation/flip config to stream
    void set_status_text(const char* text);
    void destroy_fullscreen(); // Synchronous cleanup of fullscreen overlay

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* camera_image_ = nullptr;
    lv_obj_t* camera_overlay_ = nullptr;  // Spinner overlay, hidden on first frame
    lv_obj_t* camera_status_ = nullptr;   // Status text label inside overlay

    // Fullscreen overlay state
    lv_obj_t* fullscreen_overlay_ = nullptr;
    lv_obj_t* fullscreen_image_ = nullptr;

    std::unique_ptr<CameraStream> stream_;
    bool active_ = false;              // true when on_activate() has been called
    bool compact_ = false;             // true at 1x1 — icon only, no live stream
    bool sleep_cb_registered_ = false; // true after display sleep callback registered

    // Observer for webcam availability — starts stream when URLs arrive
    ObserverGuard webcam_observer_;

    // Alive guard — prevents use-after-free in queued callbacks
    std::shared_ptr<bool> alive_;

    // Config modal (owned, destroyed when modal closes)
    std::unique_ptr<CameraConfigModal> config_modal_;

    // Persisted transform config
    nlohmann::json config_;
};

} // namespace helix

#endif // HELIX_HAS_CAMERA
