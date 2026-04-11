// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_layout.h"

#include "layout_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

namespace helix {

// Grid dimensions per breakpoint: {cols, rows}
// MICRO (≤272px height):  6x4 (same as TINY/SMALL/MEDIUM — cells are smaller)
// TINY (273-390px):       6x4
// SMALL (391-460px):      6x4
// MEDIUM (461-550px):     6x4
// LARGE (551-700px):      8x5
// XLARGE (>700px):        8x5
static constexpr std::array<GridDimensions, GridLayout::NUM_BREAKPOINTS> GRID_DIMS = {{
    {6, 4}, // MICRO (same grid as TINY/SMALL/MEDIUM)
    {6, 4}, // TINY
    {6, 4}, // SMALL
    {6, 4}, // MEDIUM
    {8, 5}, // LARGE
    {8, 5}, // XLARGE
}};

static int clamp_bp(UiBreakpoint bp) {
    int32_t v = to_int(bp);
    if (v < 0)
        return 0;
    if (v >= GridLayout::NUM_BREAKPOINTS)
        return GridLayout::NUM_BREAKPOINTS - 1;
    return v;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

GridDimensions GridLayout::get_dimensions(UiBreakpoint bp) {
    auto base = GRID_DIMS[static_cast<size_t>(clamp_bp(bp))];

    auto& lm = LayoutManager::instance();
    switch (lm.type()) {
    case LayoutType::ULTRAWIDE: {
        int w = lm.width();
        if (w > 0) {
            base.cols = std::clamp(w / TARGET_CELL_PX, MIN_DYNAMIC_COLS, MAX_DYNAMIC_COLS);
        }
        break;
    }
    case LayoutType::PORTRAIT: {
        int h = lm.height();
        if (h > 0) {
            base.rows = std::clamp(h / TARGET_CELL_PX, MIN_DYNAMIC_ROWS, MAX_DYNAMIC_ROWS);
        }
        break;
    }
    default:
        break;
    }

    return base;
}

int GridLayout::get_cols(UiBreakpoint bp) {
    return get_dimensions(bp).cols;
}

int GridLayout::get_rows(UiBreakpoint bp) {
    return get_dimensions(bp).rows;
}

std::vector<int32_t> GridLayout::make_col_dsc(UiBreakpoint bp) {
    int ncols = get_cols(bp);
    std::vector<int32_t> dsc;
    dsc.reserve(static_cast<size_t>(ncols) + 1);
    for (int i = 0; i < ncols; ++i) {
        dsc.push_back(LV_GRID_FR(1));
    }
    dsc.push_back(LV_GRID_TEMPLATE_LAST);
    return dsc;
}

std::vector<int32_t> GridLayout::make_row_dsc(UiBreakpoint bp) {
    int nrows = get_rows(bp);
    std::vector<int32_t> dsc;
    dsc.reserve(static_cast<size_t>(nrows) + 1);
    for (int i = 0; i < nrows; ++i) {
        dsc.push_back(LV_GRID_FR(1));
    }
    dsc.push_back(LV_GRID_TEMPLATE_LAST);
    return dsc;
}

// ---------------------------------------------------------------------------
// Instance methods
// ---------------------------------------------------------------------------

GridLayout::GridLayout(UiBreakpoint bp) : breakpoint_(bp) {}

GridDimensions GridLayout::dimensions() const {
    return get_dimensions(breakpoint_);
}

int GridLayout::cols() const {
    return get_cols(breakpoint_);
}

int GridLayout::rows() const {
    return get_rows(breakpoint_);
}

bool GridLayout::is_occupied(int col, int row) const {
    for (const auto& p : placements_) {
        if (col >= p.col && col < p.col + p.colspan && row >= p.row && row < p.row + p.rowspan) {
            return true;
        }
    }
    return false;
}

bool GridLayout::can_place(int col, int row, int colspan, int rowspan) const {
    int ncols = cols();
    int nrows = rows();

    // Bounds check
    if (col < 0 || row < 0 || colspan <= 0 || rowspan <= 0)
        return false;
    if (col + colspan > ncols || row + rowspan > nrows)
        return false;

    // Collision check
    for (int c = col; c < col + colspan; ++c) {
        for (int r = row; r < row + rowspan; ++r) {
            if (is_occupied(c, r))
                return false;
        }
    }
    return true;
}

bool GridLayout::place(const GridPlacement& placement) {
    if (!can_place(placement.col, placement.row, placement.colspan, placement.rowspan)) {
        spdlog::debug("GridLayout: cannot place '{}' at ({},{}) span {}x{} in {}x{} grid",
                      placement.widget_id, placement.col, placement.row, placement.colspan,
                      placement.rowspan, cols(), rows());
        return false;
    }
    placements_.push_back(placement);
    return true;
}

bool GridLayout::remove(const std::string& widget_id) {
    auto it = std::find_if(placements_.begin(), placements_.end(),
                           [&](const GridPlacement& p) { return p.widget_id == widget_id; });
    if (it == placements_.end())
        return false;
    placements_.erase(it);
    return true;
}

std::optional<std::pair<int, int>> GridLayout::find_available(int colspan, int rowspan) const {
    int ncols = cols();
    int nrows = rows();

    // Scan top-to-bottom, left-to-right
    for (int r = 0; r <= nrows - rowspan; ++r) {
        for (int c = 0; c <= ncols - colspan; ++c) {
            if (can_place(c, r, colspan, rowspan)) {
                return std::make_pair(c, r);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> GridLayout::find_available_bottom(int colspan,
                                                                     int rowspan) const {
    int ncols = cols();
    int nrows = rows();

    // Scan bottom-to-top, right-to-left
    for (int r = nrows - rowspan; r >= 0; --r) {
        for (int c = ncols - colspan; c >= 0; --c) {
            if (can_place(c, r, colspan, rowspan)) {
                return std::make_pair(c, r);
            }
        }
    }
    return std::nullopt;
}

std::pair<std::vector<GridPlacement>, std::vector<GridPlacement>>
GridLayout::filter_for_breakpoint(UiBreakpoint bp, const std::vector<GridPlacement>& placements) {
    auto dims = get_dimensions(bp);
    std::vector<GridPlacement> fits;
    std::vector<GridPlacement> does_not_fit;

    for (const auto& p : placements) {
        if (p.col >= 0 && p.row >= 0 && p.colspan > 0 && p.rowspan > 0 &&
            p.col + p.colspan <= dims.cols && p.row + p.rowspan <= dims.rows) {
            fits.push_back(p);
        } else {
            does_not_fit.push_back(p);
        }
    }
    return {std::move(fits), std::move(does_not_fit)};
}

void GridLayout::clear() {
    placements_.clear();
}

} // namespace helix
