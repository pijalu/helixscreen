// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_widget.h"

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_manager.h"
#include "moonraker_api.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "ui/ui_cleanup_helpers.h"

#include <spdlog/spdlog.h>

// Module-level subject for status text binding (static — shared across instances)
static lv_subject_t s_camera_status_subject;
static char s_camera_status_buffer[64];
static bool s_subjects_initialized = false;

static void camera_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    lv_subject_init_string(&s_camera_status_subject, s_camera_status_buffer, nullptr,
                           sizeof(s_camera_status_buffer), "No Camera");
    lv_xml_register_subject(nullptr, "camera_status_text", &s_camera_status_subject);
    SubjectDebugRegistry::instance().register_subject(
        &s_camera_status_subject, "camera_status_text", LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("CameraWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_camera_status_subject);
            s_subjects_initialized = false;
            spdlog::trace("[CameraWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[CameraWidget] Subjects initialized");
}

// Only one fullscreen camera overlay can be open at a time
static helix::CameraWidget* s_fullscreen_owner = nullptr;

static void on_camera_clicked(lv_event_t* e) {
    auto* self = helix::panel_widget_from_event<helix::CameraWidget>(e);
    if (!self)
        return;
    self->open_fullscreen();
}

static void on_camera_fullscreen_close(lv_event_t* /*e*/) {
    if (s_fullscreen_owner) {
        s_fullscreen_owner->close_fullscreen();
    }
}

namespace helix {
void register_camera_widget() {
    register_widget_factory("camera",
                            [](const std::string&) { return std::make_unique<CameraWidget>(); });
    register_widget_subjects("camera", camera_widget_init_subjects);
    lv_xml_register_event_cb(nullptr, "on_camera_clicked", on_camera_clicked);
    lv_xml_register_event_cb(nullptr, "on_camera_fullscreen_close", on_camera_fullscreen_close);
}

CameraWidget::CameraWidget() : alive_(std::make_shared<bool>(true)) {}

CameraWidget::~CameraWidget() {
    // detach() handles LVGL pointer cleanup, observer release, fullscreen close
    detach();
    // Full cleanup beyond detach: invalidate alive guard and stop stream
    if (alive_) {
        *alive_ = false;
    }
    stop_stream();
}

void CameraWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    camera_image_ = lv_obj_find_by_name(widget_obj_, "camera_image");
    camera_overlay_ = lv_obj_find_by_name(widget_obj_, "camera_overlay");
    camera_status_ = lv_obj_find_by_name(widget_obj_, "camera_status");

    lv_obj_set_user_data(widget_obj_, this);

    // ScopedFreeze may have discarded the queue_update() lambda that calls
    // frame_consumed(), leaving the stream thread blocked on frame_pending_.
    // Unblock it now — frame_consumed() is idempotent.
    if (stream_ && stream_->is_running()) {
        spdlog::trace("[CameraWidget] Unblocking stalled stream thread on reattach");
        stream_->frame_consumed();
    }

    // Observe printer_has_webcam — URLs may not be available yet at attach
    // time (Moonraker sends webcam list asynchronously). When the subject
    // transitions to 1, try starting the stream if we're active.
    lv_subject_t* gate = lv_xml_get_subject(nullptr, "printer_has_webcam");
    if (gate) {
        webcam_observer_ =
            helix::ui::observe_int_sync<CameraWidget>(gate, this, [](CameraWidget* self, int val) {
                if (val > 0) {
                    if (self->compact_) {
                        // Compact mode: icon only, status text already hidden
                    } else {
                        self->set_status_text("Connecting Camera...");
                        if (self->active_) {
                            self->start_stream();
                        }
                    }
                } else {
                    // Only stop if the stream isn't being actively displayed.
                    // observe_int_sync defers via queue_update(), so this callback can
                    // fire AFTER on_activate() already restarted the stream — killing it
                    // and leaving a gray rectangle. Also skip if fullscreen overlay is
                    // open — the stream must keep running for the fullscreen view.
                    if (!self->active_ && !self->fullscreen_overlay_) {
                        self->stop_stream();
                        self->set_status_text("No Camera");
                    }
                }
            });
    }

    spdlog::debug("[CameraWidget] Attached");
}

void CameraWidget::detach() {
    if (!widget_obj_) {
        return; // Already detached or never attached
    }

    // Synchronously destroy fullscreen overlay. Cannot use close_fullscreen()
    // because go_back() is deferred and 'this' may be destroyed before it fires.
    destroy_fullscreen();

    // Lightweight detach: release observer and LVGL pointers but preserve
    // the camera stream. alive_ is intentionally NOT invalidated here
    // (unlike other widgets) — the stream keeps running during the
    // detach→reattach gap. Frame callbacks check camera_image_ (null
    // after detach) and safely no-op until re-attach.
    webcam_observer_.reset();

    lv_obj_set_user_data(widget_obj_, nullptr);
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    camera_image_ = nullptr;
    camera_overlay_ = nullptr;
    camera_status_ = nullptr;

    spdlog::debug("[CameraWidget] Detached (stream preserved)");
}

void CameraWidget::on_activate() {
    spdlog::debug("[CameraWidget] on_activate (was active={})", active_);
    active_ = true;

    // Register for display sleep/wake notifications to suspend the camera
    // stream while the screen is off (saves ~20% CPU on Pi).
    // Uses weak_ptr to alive_ so the callback safely no-ops after destruction.
    if (!sleep_cb_registered_) {
        std::weak_ptr<bool> weak_alive = alive_;
        DisplayManager::instance()->register_sleep_callback([this, weak_alive](bool sleeping) {
            auto alive = weak_alive.lock();
            if (!alive || !*alive)
                return;
            if (sleeping) {
                // Don't stop the stream if fullscreen overlay is open — the
                // user is actively viewing the camera feed.
                if (!fullscreen_overlay_) {
                    spdlog::debug("[CameraWidget] Display sleeping — stopping camera stream");
                    stop_stream();
                }
            } else if (fullscreen_overlay_ || (active_ && !compact_)) {
                spdlog::debug("[CameraWidget] Display waking — restarting camera stream");
                start_stream();
            }
        });
        sleep_cb_registered_ = true;
    }

    if (!compact_) {
        start_stream();
    } else {
        // Compact mode: just show the icon overlay, no stream
        if (camera_overlay_) {
            lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        if (camera_status_) {
            lv_obj_add_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CameraWidget::on_deactivate() {
    spdlog::debug("[CameraWidget] on_deactivate (was active={}, fullscreen={})", active_,
                  fullscreen_overlay_ != nullptr);
    active_ = false;
    // Don't stop the stream if fullscreen is open — we're still showing frames
    if (!fullscreen_overlay_) {
        stop_stream();
    }
}

void CameraWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/, int /*height_px*/) {
    bool was_compact = compact_;
    compact_ = (colspan <= 1 && rowspan <= 1);

    // Ensure scale-to-cover after any resize
    if (camera_image_) {
        lv_image_set_inner_align(camera_image_, LV_IMAGE_ALIGN_COVER);
    }

    if (compact_ && !was_compact) {
        // Transitioning to compact: stop stream, show icon-only overlay
        if (!fullscreen_overlay_) {
            stop_stream();
        }
        if (camera_image_) {
            lv_image_set_src(camera_image_, nullptr);
        }
        if (camera_overlay_) {
            lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        if (camera_status_) {
            lv_obj_add_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (!compact_ && was_compact) {
        // Transitioning from compact to larger: show status text, start streaming
        if (camera_status_) {
            lv_obj_remove_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
        }
        if (active_) {
            start_stream();
        }
    }
}

void CameraWidget::start_stream() {
    if (stream_ && stream_->is_running()) {
        spdlog::debug("[CameraWidget] start_stream: already running, skipping");
        return;
    }

    // Ensure alive guard is valid — it may be stale (false) after a
    // stop_stream() → detach() → reattach cycle where stop invalidated
    // the old guard and detach preserved the (now-false) shared_ptr.
    if (!alive_ || !*alive_) {
        alive_ = std::make_shared<bool>(true);
    }

    stream_ = std::make_unique<CameraStream>();
    std::string stream_url, snapshot_url;
    if (!stream_->configure_from_printer(stream_url, snapshot_url)) {
        spdlog::debug("[CameraWidget] start_stream: no URLs available yet, waiting for observer");
        stream_.reset();
        return;
    }

    set_status_text("Connecting Camera...");

    // Unhide the overlay so the user sees "Connecting Camera..." instead of
    // a bare gray rectangle. The overlay was hidden when the previous stream's
    // first frame arrived — we need to re-show it for the new connection attempt.
    if (camera_overlay_) {
        lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    // Capture alive guard by value for safe callback
    std::weak_ptr<bool> weak_alive = alive_;

    stream_->start(
        stream_url, snapshot_url,
        [this, weak_alive](lv_draw_buf_t* frame) {
            auto alive = weak_alive.lock();
            if (!alive || !*alive)
                return;

            helix::ui::queue_update([this, weak_alive, frame]() {
                auto alive = weak_alive.lock();
                if (!alive || !*alive)
                    return;

                // Deliver frame to fullscreen image if open, otherwise widget image
                lv_obj_t* target = fullscreen_image_ ? fullscreen_image_ : camera_image_;
                if (target) {
                    lv_image_set_src(target, frame);
                    set_status_text("");
                    // Hide spinner overlay on first frame
                    if (camera_overlay_ && !lv_obj_has_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
                    }
                }

                // Always mark frame consumed — even if no target is available
                // (e.g. during detach→reattach). Without this, frame_pending_
                // stays true and the stream thread stalls permanently.
                if (stream_)
                    stream_->frame_consumed();
            });
        },
        [this, weak_alive](const char* msg) {
            auto alive = weak_alive.lock();
            if (!alive || !*alive)
                return;

            std::string status(msg);
            helix::ui::queue_update([this, weak_alive, status]() {
                auto alive = weak_alive.lock();
                if (!alive || !*alive)
                    return;
                set_status_text(status.c_str());
            });
        });

    spdlog::info("[CameraWidget] Stream started (stream={}, snapshot={})", stream_url,
                 snapshot_url);
}

void CameraWidget::stop_stream() {
    if (!stream_) {
        spdlog::trace("[CameraWidget] stop_stream: no stream to stop");
        return;
    }
    spdlog::debug("[CameraWidget] stop_stream: stopping active stream (active={})", active_);

    // Invalidate alive guard FIRST — queued UI callbacks check this and
    // become no-ops, preventing use-after-free on freed draw buffers
    *alive_ = false;

    // Clear image sources before stopping — stop() frees the draw buffers
    // that LVGL may still reference for rendering
    if (lv_is_initialized()) {
        if (fullscreen_image_) {
            lv_image_set_src(fullscreen_image_, nullptr);
        }
        if (camera_image_) {
            lv_image_set_src(camera_image_, nullptr);
        }
    }

    stream_->stop();
    stream_.reset();

    // Reset alive guard so the stream can be restarted (on_activate)
    alive_ = std::make_shared<bool>(true);

    spdlog::debug("[CameraWidget] Stream stopped");
}

void CameraWidget::set_status_text(const char* text) {
    if (s_subjects_initialized) {
        lv_subject_copy_string(&s_camera_status_subject, text);
    }
}

void CameraWidget::open_fullscreen() {
    if (fullscreen_overlay_ || !parent_screen_) {
        return;
    }

    // In compact mode, the stream isn't running yet — start it now
    if (!stream_) {
        start_stream();
    }
    if (!stream_) {
        return; // No URLs available
    }
    // Only one fullscreen camera at a time across all instances
    if (s_fullscreen_owner) {
        return;
    }

    // Create overlay as child of active screen (standard overlay pattern)
    lv_obj_t* screen = lv_display_get_screen_active(nullptr);
    if (!screen) {
        return;
    }

    auto* overlay = static_cast<lv_obj_t*>(lv_xml_create(screen, "camera_fullscreen", nullptr));
    if (!overlay) {
        spdlog::warn("[CameraWidget] Failed to create camera_fullscreen component");
        return;
    }

    // Start hidden — push_overlay handles showing it
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    fullscreen_overlay_ = overlay;
    fullscreen_image_ = lv_obj_find_by_name(overlay, "fullscreen_camera_image");
    s_fullscreen_owner = this;

    if (fullscreen_image_) {
        lv_image_set_inner_align(fullscreen_image_, LV_IMAGE_ALIGN_COVER);

        // Copy the current frame to the fullscreen image immediately
        if (camera_image_) {
            const void* src = lv_image_get_src(camera_image_);
            if (src) {
                lv_image_set_src(fullscreen_image_, src);
            }
        }
    }

    // Register with NavigationManager for lifecycle and cleanup.
    // The close callback handles both explicit close (close button) and
    // NavigationManager-initiated close (backdrop click, back gesture).
    NavigationManager::instance().register_overlay_instance(overlay, nullptr);
    NavigationManager::instance().register_overlay_close_callback(overlay, [this]() {
        // Clear image source before deletion to prevent dangling draw buf reference
        if (fullscreen_image_) {
            lv_image_set_src(fullscreen_image_, nullptr);
        }
        fullscreen_image_ = nullptr;
        s_fullscreen_owner = nullptr;

        // Delete the overlay widget tree
        helix::ui::safe_delete_obj(fullscreen_overlay_);

        // In compact mode, stop the stream — the widget only shows an icon
        if (compact_) {
            stop_stream();
            if (camera_overlay_) {
                lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
            }
            if (camera_status_) {
                lv_obj_add_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
            }
        }

        spdlog::debug("[CameraWidget] Fullscreen overlay closed");
    });

    NavigationManager::instance().push_overlay(overlay);

    spdlog::info("[CameraWidget] Opened fullscreen camera");
}

void CameraWidget::close_fullscreen() {
    if (!fullscreen_overlay_) {
        return;
    }

    // go_back() fires the registered close callback which handles all cleanup
    NavigationManager::instance().go_back();
}

void CameraWidget::destroy_fullscreen() {
    if (!fullscreen_overlay_) {
        return;
    }

    // Synchronous cleanup — used by detach() when 'this' may be destroyed
    // before deferred go_back() callbacks fire.
    if (fullscreen_image_) {
        lv_image_set_src(fullscreen_image_, nullptr);
    }
    fullscreen_image_ = nullptr;
    s_fullscreen_owner = nullptr;

    NavigationManager::instance().unregister_overlay_close_callback(fullscreen_overlay_);
    NavigationManager::instance().unregister_overlay_instance(fullscreen_overlay_);
    helix::ui::safe_delete_obj(fullscreen_overlay_);

    spdlog::debug("[CameraWidget] Fullscreen overlay destroyed (synchronous)");
}

} // namespace helix

#endif // HELIX_HAS_CAMERA
