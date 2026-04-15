// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "first_run_tour.h"

#include "config.h"

#include <spdlog/spdlog.h>

namespace helix::tour {

FirstRunTour& FirstRunTour::instance() {
    static FirstRunTour s;
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
    spdlog::info("[FirstRunTour] Marked completed (version={})", kTourVersion);
}

// Runtime methods implemented in Task 5.
void FirstRunTour::maybe_start() {}
void FirstRunTour::start() {}
void FirstRunTour::advance() {}
void FirstRunTour::skip() {}

} // namespace helix::tour
