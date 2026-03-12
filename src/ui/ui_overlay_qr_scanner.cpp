// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_overlay_qr_scanner.cpp
 * @brief Fullscreen QR scanner overlay implementation
 */

#include "ui_overlay_qr_scanner.h"

#include "app_globals.h"
#include "moonraker_api.h"
#include "printer_state.h"
#include "sound_manager.h"
#include "static_panel_registry.h"
#include "ui/ui_lazy_panel_helper.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#if HELIX_HAS_CAMERA
#include "camera_stream.h"
#endif

#include <spdlog/spdlog.h>

namespace helix::ui {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<QrScannerOverlay> g_qr_scanner_overlay;

QrScannerOverlay& get_qr_scanner_overlay() {
    if (!g_qr_scanner_overlay) {
        g_qr_scanner_overlay = std::make_unique<QrScannerOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "QrScannerOverlay", []() { g_qr_scanner_overlay.reset(); });
    }
    return *g_qr_scanner_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

QrScannerOverlay::QrScannerOverlay() {
#if HELIX_HAS_CAMERA
    qr_decoder_ = std::make_unique<helix::QrDecoder>();
    camera_ = std::make_unique<helix::CameraStream>();
#endif
    spdlog::debug("[{}] Created", get_name());
}

QrScannerOverlay::~QrScannerOverlay() {
    stop_scanning();
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void QrScannerOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_,
                                  "Point camera at QR code on spool",
                                  "qr_scanner_status", subjects_);
        UI_MANAGED_SUBJECT_INT(success_subject_, 0,
                               "qr_scanner_success", subjects_);
    });
}

void QrScannerOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_qr_scanner_close", on_close_clicked);
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* QrScannerOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    parent_screen_ = parent;
    cleanup_called_ = false;

    // Create fullscreen overlay from XML (not using create_overlay_from_xml since
    // this is a fullscreen overlay, not the standard overlay_panel with header)
    overlay_root_ = static_cast<lv_obj_t*>(
        lv_xml_create(parent, "qr_scanner_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find child widgets by name
    viewfinder_ = lv_obj_find_by_name(overlay_root_, "viewfinder");
    status_text_ = lv_obj_find_by_name(overlay_root_, "status_text");
    success_flash_ = lv_obj_find_by_name(overlay_root_, "success_flash");

    // Scale camera frames to cover the viewfinder area
    if (viewfinder_) {
        lv_image_set_inner_align(viewfinder_, LV_IMAGE_ALIGN_COVER);
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void QrScannerOverlay::on_ui_destroyed() {
    viewfinder_ = nullptr;
    status_text_ = nullptr;
    success_flash_ = nullptr;
    cached_overlay_ = nullptr;
}

// ============================================================================
// SHOW / LIFECYCLE
// ============================================================================

void QrScannerOverlay::show(lv_obj_t* parent, int slot_index,
                             ResultCallback on_result, CancelCallback on_cancel) {
    slot_index_ = slot_index;
    result_callback_ = std::move(on_result);
    cancel_callback_ = std::move(on_cancel);

    spdlog::debug("[{}] show() called, parent={}, cached_overlay_={}",
                  get_name(), fmt::ptr(parent), fmt::ptr(cached_overlay_));

    bool ok = lazy_create_and_push_overlay<QrScannerOverlay>(
        get_qr_scanner_overlay, cached_overlay_, parent,
        "QR Scanner", "QrScannerOverlay", true /* destroy_on_close */);
    if (!ok) {
        spdlog::error("[{}] Failed to show overlay", get_name());
    }
}

void QrScannerOverlay::show_for_active_spool(lv_obj_t* parent,
                                              ResultCallback on_result,
                                              CancelCallback on_cancel) {
    show(parent, -1, std::move(on_result), std::move(on_cancel));
}

void QrScannerOverlay::on_activate() {
    OverlayBase::on_activate();
    *alive_ = true;
    start_scanning();

    // Auto-close after 60 seconds if no scan result
    auto weak = std::weak_ptr<bool>(alive_);
    timeout_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* w = static_cast<std::weak_ptr<bool>*>(lv_timer_get_user_data(timer));
            if (auto alive = w->lock(); alive && *alive) {
                auto& overlay = get_qr_scanner_overlay();
                spdlog::info("[{}] Timeout — auto-closing", overlay.get_name());
                overlay.timeout_timer_ = nullptr;
                overlay.stop_scanning();
                if (overlay.cancel_callback_) {
                    overlay.cancel_callback_();
                }
                NavigationManager::instance().go_back();
            }
            delete w;
            lv_timer_delete(timer);
        },
        60000, new std::weak_ptr<bool>(weak));
    lv_timer_set_repeat_count(timeout_timer_, 1);

    spdlog::debug("[{}] Activated", get_name());
}

void QrScannerOverlay::on_deactivate() {
    *alive_ = false;
    stop_scanning();

    // Clear image source before camera is stopped to prevent dangling frame ref
    if (viewfinder_ && lv_is_initialized()) {
        lv_image_set_src(viewfinder_, nullptr);
    }

    // Cancel pending timers
    if (success_timer_) {
        lv_timer_delete(success_timer_);
        success_timer_ = nullptr;
    }
    if (timeout_timer_) {
        lv_timer_delete(timeout_timer_);
        timeout_timer_ = nullptr;
    }

    // Clear callbacks to prevent stale references on reuse
    result_callback_ = nullptr;
    cancel_callback_ = nullptr;

    OverlayBase::on_deactivate();
    spdlog::debug("[{}] Deactivated", get_name());
}

// ============================================================================
// SCANNING
// ============================================================================

void QrScannerOverlay::start_scanning() {
    scan_active_ = true;
    last_decoded_id_ = -1;
    lv_subject_set_int(&success_subject_, 0);
    update_status("Point camera at QR code on spool");

#if HELIX_HAS_CAMERA
    // Start camera if webcam URL is available
    std::string stream_url, snapshot_url;
    if (camera_->configure_from_printer(stream_url, snapshot_url)) {
        decode_busy_ = false;
        auto weak = std::weak_ptr<bool>(alive_);
        camera_->start(stream_url, snapshot_url,
            [this, weak](lv_draw_buf_t* frame) {
                if (auto alive = weak.lock(); alive && *alive) {
                    on_camera_frame(frame);
                }
            },
            [weak](const char* msg) {
                if (auto alive = weak.lock(); alive && *alive) {
                    spdlog::warn("[QR Scanner] Camera error: {}", msg);
                }
            });
        spdlog::info("[{}] Camera started: stream={}", get_name(), stream_url);
    } else {
        update_status("No camera \xe2\x80\x94 use USB barcode scanner");
        spdlog::info("[{}] No webcam URL, USB scanner only", get_name());
    }
#else
    update_status("No camera \xe2\x80\x94 use USB barcode scanner");
    spdlog::info("[{}] Camera support not compiled in, USB scanner only", get_name());
#endif

    // Start USB barcode scanner monitor
    auto weak = std::weak_ptr<bool>(alive_);
    usb_monitor_.start([this, weak](int spool_id) {
        helix::ui::queue_update([this, weak, spool_id]() {
            if (auto alive = weak.lock(); alive && *alive) {
                on_spool_id_detected(spool_id);
            }
        });
    });
    spdlog::debug("[{}] USB scanner monitor started", get_name());
}

void QrScannerOverlay::stop_scanning() {
    scan_active_ = false;

#if HELIX_HAS_CAMERA
    if (camera_ && camera_->is_running()) {
        camera_->stop();
    }
#endif

    if (usb_monitor_.is_running()) {
        usb_monitor_.stop();
    }

    spdlog::debug("[QR Scanner] Scanning stopped");
}

// ============================================================================
// CAMERA FRAME PROCESSING
// ============================================================================

#if HELIX_HAS_CAMERA
void QrScannerOverlay::on_camera_frame(lv_draw_buf_t* frame) {
    // Called on background camera thread

    if (!scan_active_.load() || !frame) {
        camera_->frame_consumed();
        return;
    }

    // Extract frame dimensions and pixel data for grayscale conversion
    const int width = static_cast<int>(frame->header.w);
    const int height = static_cast<int>(frame->header.h);
    const auto* pixels = static_cast<const uint8_t*>(frame->data);

    if (!pixels || width <= 0 || height <= 0) {
        camera_->frame_consumed();
        return;
    }

    // Convert to grayscale for QR detection (copies pixel data so we can
    // release the frame after the UI thread displays it)
    const int stride = static_cast<int>(frame->header.stride);
    const int pixel_size = (frame->header.cf == LV_COLOR_FORMAT_ARGB8888) ? 4 : 3;

    const auto buf_size = static_cast<size_t>(width * height);
    grayscale_buf_.resize(buf_size);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = pixels + y * stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t* px = row + x * pixel_size;
            grayscale_buf_[static_cast<size_t>(y * width + x)] = px[1];
        }
    }

    // Display the frame on the UI thread, then release it.
    // frame pointer stays valid until frame_consumed() is called.
    auto weak = std::weak_ptr<bool>(alive_);
    helix::ui::queue_update([this, weak, frame]() {
        if (auto alive = weak.lock(); alive && *alive) {
            if (viewfinder_) {
                lv_image_set_src(viewfinder_, frame);
            }
        }
        camera_->frame_consumed();
    });

    // Decode QR from grayscale copy (runs on background thread, doesn't
    // need the frame buffer — we already copied the pixels above)
    if (!decode_busy_.exchange(true)) {
        auto result = qr_decoder_->decode(grayscale_buf_.data(), width, height);
        decode_busy_ = false;

        if (result.success && result.spool_id >= 0) {
            int spool_id = result.spool_id;
            helix::ui::queue_update([this, weak, spool_id]() {
                if (auto alive = weak.lock(); alive && *alive) {
                    on_spool_id_detected(spool_id);
                }
            });
        }
    }
}
#endif

// ============================================================================
// SPOOL LOOKUP
// ============================================================================

void QrScannerOverlay::on_spool_id_detected(int spool_id) {
    // On UI thread
    if (!scan_active_.load()) {
        return;
    }

    // Debounce: skip if same ID detected again
    if (spool_id == last_decoded_id_.load()) {
        return;
    }

    last_decoded_id_ = spool_id;
    spdlog::info("[{}] Spool ID detected: {}", get_name(), spool_id);
    update_status("Looking up spool #" + std::to_string(spool_id) + "...");
    lookup_spool(spool_id);
}

void QrScannerOverlay::lookup_spool(int spool_id) {
    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::error("[{}] No MoonrakerAPI available", get_name());
        update_status("Error: not connected to printer");
        last_decoded_id_ = -1;
        return;
    }

    auto weak = std::weak_ptr<bool>(alive_);
    api->spoolman().get_spoolman_spool(spool_id,
        [this, weak, spool_id](const std::optional<SpoolInfo>& spool) {
            helix::ui::queue_update([this, weak, spool_id, spool]() {
                if (auto alive = weak.lock(); alive && *alive) {
                    if (spool.has_value()) {
                        on_spool_found(spool.value());
                    } else {
                        spdlog::warn("[{}] Spool #{} not found in Spoolman", get_name(), spool_id);
                        update_status("Spool #" + std::to_string(spool_id) + " not found");
                        last_decoded_id_ = -1; // Allow retry
                    }
                }
            });
        },
        [this, weak, spool_id](const MoonrakerError& err) {
            helix::ui::queue_update([this, weak, spool_id, err]() {
                if (auto alive = weak.lock(); alive && *alive) {
                    spdlog::error("[{}] Spool lookup failed: {}", get_name(), err.message);
                    update_status("Spool #" + std::to_string(spool_id) + " lookup failed");
                    last_decoded_id_ = -1; // Allow retry
                }
            });
        });
}

void QrScannerOverlay::on_spool_found(const SpoolInfo& spool) {
    spdlog::info("[{}] Spool found: #{} {} {}", get_name(),
                 spool.id, spool.vendor, spool.material);

    // Stop scanning immediately
    stop_scanning();

    // Play success sound
    SoundManager::instance().play("print_complete");

    // Show success flash
    show_success_flash();

    // Fire result callback and close after a brief delay
    // Capture weak alive guard per [L051] for timer lifetime safety
    struct TimerData {
        ResultCallback callback;
        SpoolInfo spool;
        std::weak_ptr<bool> alive;
    };
    auto* data = new TimerData{result_callback_, spool, std::weak_ptr<bool>(alive_)};

    success_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* d = static_cast<TimerData*>(lv_timer_get_user_data(timer));
            if (auto alive = d->alive.lock(); alive && *alive) {
                // Null member before delete to prevent double-free in on_deactivate()
                get_qr_scanner_overlay().success_timer_ = nullptr;
                if (d->callback) {
                    d->callback(d->spool);
                }
                NavigationManager::instance().go_back();
            }
            delete d;
            lv_timer_delete(timer);
        },
        500, data);
}

void QrScannerOverlay::show_success_flash() {
    lv_subject_set_int(&success_subject_, 1);
}

void QrScannerOverlay::update_status(const std::string& text) {
    // Set the string subject for XML binding
    lv_subject_copy_string(&status_subject_, text.c_str());
    spdlog::debug("[{}] Status: {}", get_name(), text);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void QrScannerOverlay::on_close_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[QrScannerOverlay] on_close_clicked");

    auto& overlay = get_qr_scanner_overlay();
    spdlog::info("[{}] Close clicked", overlay.get_name());

    overlay.stop_scanning();

    // Fire cancel callback
    if (overlay.cancel_callback_) {
        overlay.cancel_callback_();
    }

    // Navigate back
    NavigationManager::instance().go_back();

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
