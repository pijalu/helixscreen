// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "first_run_tour.h"

#include "config.h"
#include "static_panel_registry.h"
#include "tour_overlay.h"
#include "tour_steps.h"

#include <lvgl.h>
#include <spdlog/spdlog.h>

namespace helix::tour {

FirstRunTour& FirstRunTour::instance() {
    static FirstRunTour s;
    static bool registered = []() {
        // Tear the overlay down before LVGL deinits — otherwise the unique_ptr's
        // destructor runs during C++ static teardown, after lv_deinit(), and
        // ~TourOverlay's lv_obj_delete() touches freed LVGL state.
        StaticPanelRegistry::instance().register_destroy("FirstRunTour", []() {
            FirstRunTour::instance().overlay_.reset();
        });
        return true;
    }();
    (void)registered;
    return s;
}

bool FirstRunTour::should_auto_start() {
    auto* cfg = ::helix::Config::get_instance();
    if (!cfg) return false;

    if (cfg->is_wizard_required()) {
        spdlog::debug("[FirstRunTour] gate: wizard_required");
        return false;
    }

    const bool completed = cfg->get<bool>("/tour/completed", false);
    const int last_seen = cfg->get<int>("/tour/last_seen_version", 0);

    if (!completed) {
        spdlog::debug("[FirstRunTour] gate: fresh_auto_start");
        return true;
    }
    if (last_seen < kTourVersion) {
        spdlog::debug("[FirstRunTour] gate: version_bumped_since_last_seen "
                      "(last_seen={}, current={})",
                      last_seen, kTourVersion);
        return true;
    }
    spdlog::debug("[FirstRunTour] gate: already_completed_current_version");
    return false;
}

void FirstRunTour::mark_completed() {
    auto* cfg = ::helix::Config::get_instance();
    if (!cfg) return;
    cfg->set<bool>("/tour/completed", true);
    cfg->set<int>("/tour/last_seen_version", kTourVersion);
    cfg->save();
    spdlog::debug("[FirstRunTour] Marked completed (version={})", kTourVersion);
}

void FirstRunTour::maybe_start() {
    if (running_) return;
    if (!should_auto_start()) return;
    // Defer one tick so the caller (e.g., HomePanel::on_activate) completes first.
    lv_async_call(
        [](void* self) { static_cast<FirstRunTour*>(self)->start_impl(); }, this);
}

void FirstRunTour::start() {
    if (running_) return;
    start_impl();
}

void FirstRunTour::start_impl() {
    running_ = true;
    current_index_ = 0;
    steps_ = build_tour_steps(hardware_has_ams());

    // Overlay requires an active LVGL screen; unit tests run headless.
    if (lv_screen_active()) {
        overlay_ = std::make_unique<TourOverlay>(
            steps_, [this] { this->advance(); }, [this] { this->skip(); });
        render_current_step();
    }

    spdlog::debug("[FirstRunTour] Started ({} steps)", steps_.size());
}

void FirstRunTour::advance() {
    if (!running_) return;
    current_index_++;
    if (current_index_ >= steps_.size()) {
        finish();
        return;
    }
    render_current_step();
}

void FirstRunTour::skip() {
    if (!running_) return;
    spdlog::debug("[FirstRunTour] Skipped at step {}/{}", current_index_ + 1, steps_.size());
    mark_completed();
    running_ = false;
    overlay_.reset();
}

void FirstRunTour::finish() {
    spdlog::debug("[FirstRunTour] Finished");
    mark_completed();
    running_ = false;
    overlay_.reset();
}

void FirstRunTour::reset_for_test() {
    running_ = false;
    current_index_ = 0;
    steps_.clear();
    overlay_.reset();
}

void FirstRunTour::render_current_step() {
    if (overlay_) overlay_->show_step(current_index_);
}

} // namespace helix::tour
