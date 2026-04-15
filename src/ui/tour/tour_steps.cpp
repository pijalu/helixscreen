// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_steps.h"

#include "ams_state.h"

namespace helix::tour {

bool hardware_has_ams() {
    return AmsState::instance().backend_count() > 0;
}

std::vector<TourStep> build_tour_steps(bool has_ams) {
    std::vector<TourStep> steps;
    steps.reserve(8);

    // 1. Welcome (centered card, no target)
    steps.push_back({"", "tour.step.welcome.title", "tour.step.welcome.body",
                     TooltipAnchor::Center, {}});

    // 2. Home widget examples — sub-spotlight nozzle, fan, (AMS if present)
    {
        TourStep s{"carousel_host", "tour.step.home_grid.title",
                   "tour.step.home_grid.body", TooltipAnchor::PreferBelow, {}};
        s.sub_spotlights.push_back("widget_nozzle_temp");
        s.sub_spotlights.push_back("widget_fan");
        if (has_ams) s.sub_spotlights.push_back("widget_ams");
        steps.push_back(s);
    }

    // 3. Long-press to customize
    steps.push_back({"carousel_host", "tour.step.customize.title",
                     "tour.step.customize.body", TooltipAnchor::PreferBelow, {}});

    // 4-8. Navbar tour
    steps.push_back({"nav_btn_print_select", "tour.step.print.title",
                     "tour.step.print.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_controls", "tour.step.controls.title",
                     "tour.step.controls.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_filament", "tour.step.filament.title",
                     "tour.step.filament.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_advanced", "tour.step.advanced.title",
                     "tour.step.advanced.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_settings", "tour.step.settings.title",
                     "tour.step.settings.body", TooltipAnchor::PreferRight, {}});

    return steps;
}

}  // namespace helix::tour
