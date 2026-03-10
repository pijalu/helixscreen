// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock_widget.h"

#include "lock_manager.h"
#include "panel_widget.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "static_subject_registry.h"
#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_lock_screen.h"

#include <lvgl.h>
#include <spdlog/spdlog.h>

// ============================================================================
// Module-level subject (no PIN state needed in widget — LockManager owns it)
// ============================================================================

// Forward declaration for callback
static void lock_screen_clicked_cb(lv_event_t* e);

// ============================================================================
// LockWidget class — minimal 1x1 tap-to-lock button
// ============================================================================

namespace helix {

class LockWidget : public PanelWidget {
  public:
    LockWidget() = default;
    ~LockWidget() override { detach(); }

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override {
        widget_obj_ = widget_obj;
        parent_screen_ = parent_screen;

        lock_btn_ = lv_obj_find_by_name(widget_obj_, "lock_button");
        if (lock_btn_) {
            lv_obj_set_user_data(lock_btn_, this);
        } else {
            spdlog::warn("[LockWidget] Could not find lock_button in widget tree");
        }
    }

    void detach() override {
        if (lock_btn_) {
            lv_obj_set_user_data(lock_btn_, nullptr);
            lock_btn_ = nullptr;
        }
        widget_obj_ = nullptr;
        parent_screen_ = nullptr;
    }

    const char* id() const override { return "lock"; }

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* lock_btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
};

// ============================================================================
// Widget registration
// ============================================================================

void register_lock_widget() {
    register_widget_factory("lock", []() { return std::make_unique<LockWidget>(); });

    // Register XML event callback before any XML is parsed
    lv_xml_register_event_cb(nullptr, "lock_screen_clicked_cb", lock_screen_clicked_cb);
}

} // namespace helix

// ============================================================================
// Static XML event callback
// ============================================================================

static void lock_screen_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LockWidget] lock_screen_clicked_cb");
    (void)e;

    spdlog::info("[LockWidget] Lock button clicked");
    helix::LockManager::instance().lock();
    helix::ui::LockScreenOverlay::instance().show();

    LVGL_SAFE_EVENT_CB_END();
}
