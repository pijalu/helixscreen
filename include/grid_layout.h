// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {

/// Grid dimensions for a specific breakpoint
struct GridDimensions {
    int cols;
    int rows;
};

/// A widget placement on the grid
struct GridPlacement {
    std::string widget_id;
    int col;
    int row;
    int colspan;
    int rowspan;
};

/// Manages grid layout for the home panel dashboard.
/// Handles grid descriptor generation, widget placement, collision detection,
/// and breakpoint adaptation.
class GridLayout {
  public:
    /// Number of defined breakpoints
    static constexpr int NUM_BREAKPOINTS = 6;

    /// Target cell size in pixels for dynamic grid computation.
    /// ULTRAWIDE/PORTRAIT layouts divide screen resolution by this to determine
    /// cols/rows. Exposed for future features (half-width widgets, etc.).
    static constexpr int TARGET_CELL_PX = 160;

    /// Clamp range for dynamically computed grid dimensions
    static constexpr int MIN_DYNAMIC_COLS = 4;
    static constexpr int MAX_DYNAMIC_COLS = 16;
    static constexpr int MIN_DYNAMIC_ROWS = 3;
    static constexpr int MAX_DYNAMIC_ROWS = 12;

    /// Get grid dimensions for a given breakpoint index (0-4)
    static GridDimensions get_dimensions(int breakpoint);

    /// Get the number of columns for a breakpoint
    static int get_cols(int breakpoint);

    /// Get the number of rows for a breakpoint
    static int get_rows(int breakpoint);

    /// Generate LVGL column descriptor array for a breakpoint.
    /// Returns vector of int32_t values terminated by LV_GRID_TEMPLATE_LAST.
    static std::vector<int32_t> make_col_dsc(int breakpoint);

    /// Generate LVGL row descriptor array for a breakpoint.
    /// Returns vector of int32_t values terminated by LV_GRID_TEMPLATE_LAST.
    static std::vector<int32_t> make_row_dsc(int breakpoint);

    /// Construct a GridLayout for a specific breakpoint
    explicit GridLayout(int breakpoint);

    /// Get the breakpoint this layout was constructed for
    int breakpoint() const {
        return breakpoint_;
    }

    /// Get grid dimensions
    GridDimensions dimensions() const;
    int cols() const;
    int rows() const;

    /// Try to place a widget. Returns true if placed successfully.
    /// Fails if placement overlaps existing widgets or is out of bounds.
    bool place(const GridPlacement& placement);

    /// Remove a widget by ID. Returns true if found and removed.
    bool remove(const std::string& widget_id);

    /// Check if a placement would be valid (no collision, in bounds)
    bool can_place(int col, int row, int colspan, int rowspan) const;

    /// Find first available position for a widget of given size.
    /// Scans top-to-bottom, left-to-right (row-major order).
    std::optional<std::pair<int, int>> find_available(int colspan, int rowspan) const;

    /// Find first available position scanning bottom-to-top, right-to-left.
    /// Used by auto-placement to pack widgets toward the bottom of the grid.
    std::optional<std::pair<int, int>> find_available_bottom(int colspan, int rowspan) const;

    /// Get all current placements
    const std::vector<GridPlacement>& placements() const {
        return placements_;
    }

    /// Check which placements from a list fit within this layout's grid.
    /// Returns two vectors: (fits, does_not_fit)
    static std::pair<std::vector<GridPlacement>, std::vector<GridPlacement>>
    filter_for_breakpoint(int breakpoint, const std::vector<GridPlacement>& placements);

    /// Clear all placements
    void clear();

    /// Check if a cell is occupied by any existing placement
    bool is_occupied(int col, int row) const;

  private:
    int breakpoint_;
    std::vector<GridPlacement> placements_;
};

} // namespace helix
