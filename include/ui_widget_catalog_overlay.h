// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace helix {

class PanelWidgetConfig;

/// Callback invoked when the user selects a widget from the catalog.
/// Receives the widget definition ID (e.g. "temperature", "network").
using WidgetSelectedCallback = std::function<void(const std::string& widget_id)>;

/// Callback invoked when the catalog overlay is closed (selection or back navigation).
using CatalogClosedCallback = std::function<void()>;

/// Shows a half-width overlay listing available widgets for grid placement.
/// Widgets already placed are shown dimmed with a "Placed" badge.
/// On selection, the callback fires and the overlay closes itself.
class WidgetCatalogOverlay {
  public:
    /// Open the catalog overlay.
    /// @param parent_screen  Screen to parent the overlay on
    /// @param config         Current widget config (to determine which are already placed)
    /// @param on_select      Called with the chosen widget ID when user taps a row
    /// @param on_close       Called when the overlay is closed for any reason
    static void show(lv_obj_t* parent_screen, const PanelWidgetConfig& config,
                     WidgetSelectedCallback on_select, CatalogClosedCallback on_close = nullptr);

  private:
    /// Populate the scroll container with one row per registered widget
    static void populate_rows(lv_obj_t* scroll, const PanelWidgetConfig& config,
                              WidgetSelectedCallback on_select);

    /// Create a single catalog row widget
    static lv_obj_t* create_row(lv_obj_t* parent, const char* name, const char* icon,
                                const char* description, int colspan, int rowspan,
                                bool already_placed, bool hardware_gated);
};

} // namespace helix
