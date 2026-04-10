// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_layout.cpp
 * @brief Unit tests for GridLayout — grid dimensions, descriptor generation,
 *        widget placement, collision detection, and breakpoint adaptation.
 */

#include "grid_layout.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// =============================================================================
// Grid dimensions per breakpoint
// =============================================================================

TEST_CASE("GridLayout dimensions: TINY (bp 0) = 6x4", "[grid_layout][dimensions]") {
    auto dims = GridLayout::get_dimensions(0);
    CHECK(dims.cols == 6);
    CHECK(dims.rows == 4);
    CHECK(GridLayout::get_cols(0) == 6);
    CHECK(GridLayout::get_rows(0) == 4);
}

TEST_CASE("GridLayout dimensions: SMALL (bp 1) = 6x4", "[grid_layout][dimensions]") {
    auto dims = GridLayout::get_dimensions(1);
    CHECK(dims.cols == 6);
    CHECK(dims.rows == 4);
}

TEST_CASE("GridLayout dimensions: MEDIUM (bp 2) = 6x4", "[grid_layout][dimensions]") {
    auto dims = GridLayout::get_dimensions(2);
    CHECK(dims.cols == 6);
    CHECK(dims.rows == 4);
}

TEST_CASE("GridLayout dimensions: LARGE (bp 4) = 8x5", "[grid_layout][dimensions]") {
    auto dims = GridLayout::get_dimensions(4);
    CHECK(dims.cols == 8);
    CHECK(dims.rows == 5);
}

TEST_CASE("GridLayout dimensions: XLARGE (bp 4) = 8x5", "[grid_layout][dimensions]") {
    auto dims = GridLayout::get_dimensions(4);
    CHECK(dims.cols == 8);
    CHECK(dims.rows == 5);
}

TEST_CASE("GridLayout dimensions: out-of-range breakpoints are clamped",
          "[grid_layout][dimensions]") {
    // Negative clamps to 0 (TINY)
    CHECK(GridLayout::get_cols(-1) == 6);
    CHECK(GridLayout::get_rows(-1) == 4);

    // Above max clamps to 4 (XLARGE)
    CHECK(GridLayout::get_cols(99) == 8);
    CHECK(GridLayout::get_rows(99) == 5);
}

// =============================================================================
// Descriptor array generation
// =============================================================================

TEST_CASE("GridLayout make_col_dsc: correct length and values", "[grid_layout][descriptor]") {
    SECTION("TINY (6 cols)") {
        auto dsc = GridLayout::make_col_dsc(0);
        REQUIRE(dsc.size() == 7); // 6 FR values + terminator
        for (int i = 0; i < 6; ++i) {
            CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
        }
        CHECK(dsc[6] == LV_GRID_TEMPLATE_LAST);
    }

    SECTION("LARGE (8 cols)") {
        auto dsc = GridLayout::make_col_dsc(3);
        REQUIRE(dsc.size() == 9); // 8 FR values + terminator
        for (int i = 0; i < 8; ++i) {
            CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
        }
        CHECK(dsc[8] == LV_GRID_TEMPLATE_LAST);
    }
}

TEST_CASE("GridLayout make_row_dsc: correct length and values", "[grid_layout][descriptor]") {
    SECTION("TINY (4 rows)") {
        auto dsc = GridLayout::make_row_dsc(0);
        REQUIRE(dsc.size() == 5); // 4 FR values + terminator
        for (int i = 0; i < 4; ++i) {
            CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
        }
        CHECK(dsc[4] == LV_GRID_TEMPLATE_LAST);
    }

    SECTION("LARGE (5 rows)") {
        auto dsc = GridLayout::make_row_dsc(3);
        REQUIRE(dsc.size() == 6); // 5 FR values + terminator
        CHECK(dsc[5] == LV_GRID_TEMPLATE_LAST);
    }
}

// =============================================================================
// Widget placement — successful
// =============================================================================

TEST_CASE("GridLayout place: single widget at origin", "[grid_layout][placement]") {
    GridLayout grid(0); // TINY 6x4
    REQUIRE(grid.place({"widget_a", 0, 0, 2, 1}));
    REQUIRE(grid.placements().size() == 1);
    CHECK(grid.placements()[0].widget_id == "widget_a");
}

TEST_CASE("GridLayout place: multiple non-overlapping widgets", "[grid_layout][placement]") {
    GridLayout grid(1); // SMALL 6x4
    REQUIRE(grid.place({"w1", 0, 0, 2, 2}));
    REQUIRE(grid.place({"w2", 2, 0, 2, 2}));
    REQUIRE(grid.place({"w3", 4, 0, 2, 2}));
    REQUIRE(grid.place({"w4", 0, 2, 3, 2}));
    CHECK(grid.placements().size() == 4);
}

TEST_CASE("GridLayout place: widget filling entire grid", "[grid_layout][placement]") {
    GridLayout grid(0); // TINY 6x4
    REQUIRE(grid.place({"full", 0, 0, 6, 4}));
    CHECK(grid.placements().size() == 1);
}

// =============================================================================
// Collision detection
// =============================================================================

TEST_CASE("GridLayout place: rejects overlapping placements", "[grid_layout][collision]") {
    GridLayout grid(1);                      // SMALL 6x4
    REQUIRE(grid.place({"w1", 1, 1, 2, 2})); // occupies (1,1)-(2,2)

    // Exact overlap
    CHECK_FALSE(grid.place({"w2", 1, 1, 2, 2}));

    // Partial overlap — top-left corner overlaps
    CHECK_FALSE(grid.place({"w3", 2, 2, 2, 2}));

    // Partial overlap — single cell
    CHECK_FALSE(grid.place({"w4", 2, 1, 1, 1}));

    // Adjacent — no overlap, should succeed
    CHECK(grid.place({"w5", 3, 1, 2, 2}));
}

TEST_CASE("GridLayout can_place: returns false for occupied cells", "[grid_layout][collision]") {
    GridLayout grid(0); // TINY 6x4
    grid.place({"w1", 0, 0, 2, 2});

    CHECK_FALSE(grid.can_place(0, 0, 1, 1));
    CHECK_FALSE(grid.can_place(1, 1, 1, 1));
    CHECK(grid.can_place(2, 0, 1, 1));
    CHECK(grid.can_place(0, 2, 1, 1));
}

// =============================================================================
// Out-of-bounds rejection
// =============================================================================

TEST_CASE("GridLayout place: rejects out-of-bounds placements", "[grid_layout][bounds]") {
    GridLayout grid(0); // TINY 6x4

    // Exceeds columns
    CHECK_FALSE(grid.place({"oob1", 5, 0, 2, 1})); // col 5 + span 2 = 7 > 6

    // Exceeds rows
    CHECK_FALSE(grid.place({"oob2", 0, 3, 1, 2})); // row 3 + span 2 = 5 > 4

    // Negative position
    CHECK_FALSE(grid.place({"oob3", -1, 0, 1, 1}));

    // Zero span
    CHECK_FALSE(grid.place({"oob4", 0, 0, 0, 1}));
    CHECK_FALSE(grid.place({"oob5", 0, 0, 1, 0}));

    // Exactly at boundary — should succeed
    CHECK(grid.place({"edge", 5, 3, 1, 1}));
}

// =============================================================================
// find_available()
// =============================================================================

TEST_CASE("GridLayout find_available: finds first open position", "[grid_layout][find]") {
    GridLayout grid(0); // TINY 6x4
    grid.place({"w1", 0, 0, 2, 1});

    auto pos = grid.find_available(2, 1);
    REQUIRE(pos.has_value());
    // First available 2x1 slot: (2,0) — same row, after w1
    CHECK(pos->first == 2);
    CHECK(pos->second == 0);
}

TEST_CASE("GridLayout find_available: scans top-to-bottom, left-to-right", "[grid_layout][find]") {
    GridLayout grid(1); // SMALL 6x4

    // Fill top row completely
    grid.place({"r0a", 0, 0, 3, 1});
    grid.place({"r0b", 3, 0, 3, 1});

    // Next available 1x1 should be at row 1
    auto pos = grid.find_available(1, 1);
    REQUIRE(pos.has_value());
    CHECK(pos->first == 0);
    CHECK(pos->second == 1);
}

TEST_CASE("GridLayout find_available: returns nullopt when no space", "[grid_layout][find]") {
    GridLayout grid(0); // TINY 6x4

    // Fill the entire grid with 1x1 widgets
    int id = 0;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 6; ++c) {
            REQUIRE(grid.place({"fill_" + std::to_string(id++), c, r, 1, 1}));
        }
    }

    CHECK_FALSE(grid.find_available(1, 1).has_value());
}

TEST_CASE("GridLayout find_available: large widget in fragmented grid", "[grid_layout][find]") {
    GridLayout grid(1); // SMALL 6x4

    // Place checkerboard-style: occupy (0,0), (2,0), (4,0) with 1x1 widgets
    grid.place({"c1", 0, 0, 1, 1});
    grid.place({"c2", 2, 0, 1, 1});
    grid.place({"c3", 4, 0, 1, 1});

    // A 2x1 widget can fit at (0,1) on the second row
    auto pos = grid.find_available(2, 1);
    REQUIRE(pos.has_value());
    // Actually it should find something on row 0 at position (0,0) is occupied,
    // (1,0) is free — so (1,0) with span 2 needs (1,0) and (2,0). But (2,0) is occupied.
    // Next candidate: (3,0) with span 2 needs (3,0) and (4,0). (4,0) is occupied.
    // Then (5,0) needs (5,0)+(6,0)=out of bounds for 6 col grid? 5+2=7>6, no.
    // Row 1: (0,1) is free and (1,1) is free — so (0,1) works.
    CHECK(pos->first == 0);
    CHECK(pos->second == 1);
}

// =============================================================================
// remove()
// =============================================================================

TEST_CASE("GridLayout remove: removes existing widget", "[grid_layout][remove]") {
    GridLayout grid(0); // TINY 6x4
    grid.place({"w1", 0, 0, 2, 2});
    grid.place({"w2", 2, 0, 2, 2});

    REQUIRE(grid.remove("w1"));
    CHECK(grid.placements().size() == 1);
    CHECK(grid.placements()[0].widget_id == "w2");

    // Space freed: can place at (0,0) again
    CHECK(grid.can_place(0, 0, 2, 2));
}

TEST_CASE("GridLayout remove: returns false for nonexistent widget", "[grid_layout][remove]") {
    GridLayout grid(0);
    CHECK_FALSE(grid.remove("nonexistent"));
}

// =============================================================================
// clear()
// =============================================================================

TEST_CASE("GridLayout clear: removes all placements", "[grid_layout][clear]") {
    GridLayout grid(0); // TINY 6x4
    grid.place({"w1", 0, 0, 1, 1});
    grid.place({"w2", 1, 0, 1, 1});
    REQUIRE(grid.placements().size() == 2);

    grid.clear();
    CHECK(grid.placements().empty());
    CHECK(grid.can_place(0, 0, 6, 4)); // full grid available
}

// =============================================================================
// filter_for_breakpoint()
// =============================================================================

TEST_CASE("GridLayout filter_for_breakpoint: separates fitting vs non-fitting",
          "[grid_layout][filter]") {
    std::vector<GridPlacement> all = {
        {"fits_1", 0, 0, 2, 2},   // fits in 6x4
        {"fits_2", 2, 0, 2, 1},   // fits in 6x4
        {"too_wide", 0, 0, 7, 1}, // needs 7 cols, TINY has 6
        {"too_tall", 0, 0, 1, 5}, // needs 5 rows, TINY has 4
    };

    auto [fits, no_fit] = GridLayout::filter_for_breakpoint(0, all); // TINY 6x4

    REQUIRE(fits.size() == 2);
    REQUIRE(no_fit.size() == 2);

    CHECK(fits[0].widget_id == "fits_1");
    CHECK(fits[1].widget_id == "fits_2");
    CHECK(no_fit[0].widget_id == "too_wide");
    CHECK(no_fit[1].widget_id == "too_tall");
}

TEST_CASE("GridLayout filter_for_breakpoint: all fit in LARGE", "[grid_layout][filter]") {
    std::vector<GridPlacement> all = {
        {"w1", 0, 0, 4, 3},
        {"w2", 4, 0, 4, 2},
    };

    auto [fits, no_fit] = GridLayout::filter_for_breakpoint(4, all); // LARGE 8x5
    CHECK(fits.size() == 2);
    CHECK(no_fit.empty());
}

// =============================================================================
// Breakpoint transition scenarios
// =============================================================================

TEST_CASE("GridLayout breakpoint transition: 8x5 placement does not fit in TINY",
          "[grid_layout][transition]") {
    // A widget placed at col 7 in an 8-col (LARGE) grid should not fit in TINY (6-col)
    std::vector<GridPlacement> placements = {
        {"corner", 7, 4, 1, 1}, // col 7 + span 1 = 8, TINY only has 6 cols; row 4 + 1 = 5 > 4
    };

    auto [fits, no_fit] = GridLayout::filter_for_breakpoint(0, placements); // TINY 6x4
    CHECK(fits.empty());
    CHECK(no_fit.size() == 1);

    // Same placement fits in LARGE (8x5)
    auto [fits2, no_fit2] = GridLayout::filter_for_breakpoint(4, placements);
    CHECK(fits2.size() == 1);
    CHECK(no_fit2.empty());
}

TEST_CASE("GridLayout breakpoint transition: LARGE placement partially fits in SMALL",
          "[grid_layout][transition]") {
    std::vector<GridPlacement> placements = {
        {"top_left", 0, 0, 2, 2},   // fits everywhere
        {"wide_right", 6, 0, 2, 1}, // needs col 6+2=8, only fits LARGE/XLARGE
        {"bottom_row", 0, 4, 3, 1}, // needs row 4+1=5, only fits LARGE/XLARGE
    };

    // SMALL (6x4): only top_left fits
    auto [small_fits, small_no] = GridLayout::filter_for_breakpoint(1, placements);
    CHECK(small_fits.size() == 1);
    CHECK(small_fits[0].widget_id == "top_left");
    CHECK(small_no.size() == 2);

    // LARGE (8x5): all fit
    auto [large_fits, large_no] = GridLayout::filter_for_breakpoint(4, placements);
    CHECK(large_fits.size() == 3);
    CHECK(large_no.empty());
}

// =============================================================================
// Instance breakpoint accessor
// =============================================================================

TEST_CASE("GridLayout instance: breakpoint and dimensions match", "[grid_layout][instance]") {
    for (int bp = 0; bp < GridLayout::NUM_BREAKPOINTS; ++bp) {
        GridLayout grid(bp);
        CHECK(grid.breakpoint() == bp);
        CHECK(grid.cols() == GridLayout::get_cols(bp));
        CHECK(grid.rows() == GridLayout::get_rows(bp));
    }
}

// =============================================================================
// PanelWidgetDef scalability constraints
// =============================================================================

TEST_CASE("PanelWidgetDef: default (non-scalable) widget", "[widget_def][scalability]") {
    PanelWidgetDef def{};
    def.colspan = 1;
    def.rowspan = 1;
    // min/max all 0 = use colspan/rowspan

    CHECK(def.effective_min_colspan() == 1);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_colspan() == 1);
    CHECK(def.effective_max_rowspan() == 1);
    CHECK_FALSE(def.is_scalable());
}

TEST_CASE("PanelWidgetDef: scalable widget with explicit min/max", "[widget_def][scalability]") {
    PanelWidgetDef def{};
    def.colspan = 2;
    def.rowspan = 2;
    def.min_colspan = 1;
    def.min_rowspan = 1;
    def.max_colspan = 4;
    def.max_rowspan = 3;

    CHECK(def.effective_min_colspan() == 1);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_colspan() == 4);
    CHECK(def.effective_max_rowspan() == 3);
    CHECK(def.is_scalable());
}

TEST_CASE("PanelWidgetDef: horizontally scalable only", "[widget_def][scalability]") {
    PanelWidgetDef def{};
    def.colspan = 2;
    def.rowspan = 1;
    def.min_colspan = 2;
    def.max_colspan = 6;
    // min/max rowspan = 0, so effective = rowspan = 1

    CHECK(def.effective_min_colspan() == 2);
    CHECK(def.effective_max_colspan() == 6);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_rowspan() == 1);
    CHECK(def.is_scalable()); // max_col > min_col
}

TEST_CASE("PanelWidgetDef: registry entries have valid scalability constraints",
          "[widget_def][scalability]") {
    // Force registration so defs have their final state
    init_widget_registrations();

    for (const auto& def : get_all_widget_defs()) {
        INFO("Widget: " << def.id);
        // Min must not exceed max
        CHECK(def.effective_min_colspan() <= def.effective_max_colspan());
        CHECK(def.effective_min_rowspan() <= def.effective_max_rowspan());
        // Default must be within min..max range
        CHECK(def.colspan >= def.effective_min_colspan());
        CHECK(def.colspan <= def.effective_max_colspan());
        CHECK(def.rowspan >= def.effective_min_rowspan());
        CHECK(def.rowspan <= def.effective_max_rowspan());
    }
}

// =============================================================================
// Dynamic grid dimensions for non-standard layouts
// =============================================================================

#include "layout_manager.h"

// Access LayoutManager internals for test setup.
// Note: LayoutManagerTestAccess is also defined in test_layout_manager.cpp but
// Catch2 amalgamated builds compile each test file separately, so no ODR conflict.
class LayoutManagerTestAccess {
  public:
    static void reset(helix::LayoutManager& lm) {
        lm.type_ = helix::LayoutType::STANDARD;
        lm.name_ = "standard";
        lm.override_name_.clear();
        lm.initialized_ = false;
        lm.width_ = 0;
        lm.height_ = 0;
    }
};

struct GridLayoutFixture {
    GridLayoutFixture() {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }
    ~GridLayoutFixture() {
        LayoutManagerTestAccess::reset(helix::LayoutManager::instance());
    }
};

TEST_CASE_METHOD(GridLayoutFixture, "GridLayout dimensions: ULTRAWIDE scales cols from width",
                 "[grid_layout][dimensions][ultrawide]") {
    auto& lm = helix::LayoutManager::instance();

    SECTION("1920x440 -> 12 cols, rows from SMALL breakpoint (4)") {
        lm.init(1920, 440);                        // ULTRAWIDE, SMALL breakpoint
        auto dims = GridLayout::get_dimensions(1); // SMALL
        CHECK(dims.cols == 12);                    // 1920 / 160 = 12
        CHECK(dims.rows == 4);                     // SMALL base rows
    }
    SECTION("2560x600 -> 16 cols (clamped), rows from LARGE breakpoint (5)") {
        lm.init(2560, 600);                        // ULTRAWIDE, LARGE breakpoint
        auto dims = GridLayout::get_dimensions(4); // LARGE
        CHECK(dims.cols == 16);                    // 2560 / 160 = 16 (at max clamp)
        CHECK(dims.rows == 5);                     // LARGE base rows
    }
    SECTION("640x200 -> 4 cols (min clamp)") {
        lm.init(640, 200);                         // ratio 3.2 -> ULTRAWIDE
        auto dims = GridLayout::get_dimensions(0); // TINY
        CHECK(dims.cols == 4);                     // 640 / 160 = 4 (at min clamp)
        CHECK(dims.rows == 4);                     // TINY base rows
    }
}

TEST_CASE_METHOD(GridLayoutFixture, "GridLayout dimensions: PORTRAIT scales rows from height",
                 "[grid_layout][dimensions][portrait]") {
    auto& lm = helix::LayoutManager::instance();

    SECTION("480x1600 -> base cols, 10 rows") {
        lm.init(480, 1600);                        // PORTRAIT, XLARGE breakpoint
        auto dims = GridLayout::get_dimensions(4); // XLARGE
        CHECK(dims.cols == 8);                     // XLARGE base cols (unchanged)
        CHECK(dims.rows == 10);                    // 1600 / 160 = 10
    }
    SECTION("480x800 -> base cols, 5 rows (same as table)") {
        lm.init(480, 800);                         // PORTRAIT
        auto dims = GridLayout::get_dimensions(4); // XLARGE
        CHECK(dims.cols == 8);                     // XLARGE base cols
        CHECK(dims.rows == 5);                     // 800 / 160 = 5
    }
    SECTION("480x1920 -> 12 rows (max clamp)") {
        lm.init(480, 1920);                        // PORTRAIT
        auto dims = GridLayout::get_dimensions(4); // XLARGE
        CHECK(dims.cols == 8);
        CHECK(dims.rows == 12); // 1920 / 160 = 12, at max clamp
    }
    SECTION("320x480 -> 3 rows (min clamp)") {
        lm.init(320, 480); // TINY_PORTRAIT (max_dim <=480), not PORTRAIT
        // TINY_PORTRAIT uses table, not dynamic
        auto dims = GridLayout::get_dimensions(0); // TINY
        CHECK(dims.cols == 6);                     // TINY base
        CHECK(dims.rows == 4);                     // TINY base (table, not dynamic)
    }
}

TEST_CASE_METHOD(GridLayoutFixture, "GridLayout dimensions: STANDARD layout uses table (unchanged)",
                 "[grid_layout][dimensions][standard]") {
    auto& lm = helix::LayoutManager::instance();
    lm.init(800, 480); // STANDARD

    auto dims = GridLayout::get_dimensions(2); // MEDIUM
    CHECK(dims.cols == 6);
    CHECK(dims.rows == 4);
}

TEST_CASE_METHOD(GridLayoutFixture, "GridLayout dimensions: uninitialized LayoutManager uses table",
                 "[grid_layout][dimensions]") {
    // LayoutManager not initialized (reset by fixture) — should fall back to table
    auto dims = GridLayout::get_dimensions(2); // MEDIUM
    CHECK(dims.cols == 6);
    CHECK(dims.rows == 4);
}

// =============================================================================
// Descriptor generation and instance methods with dynamic sizing
// =============================================================================

TEST_CASE_METHOD(GridLayoutFixture,
                 "GridLayout make_col_dsc: ultrawide produces correct descriptor length",
                 "[grid_layout][descriptor][ultrawide]") {
    auto& lm = helix::LayoutManager::instance();
    lm.init(1920, 440); // ULTRAWIDE -> 12 cols

    auto dsc = GridLayout::make_col_dsc(1); // SMALL breakpoint
    REQUIRE(dsc.size() == 13);              // 12 FR values + terminator
    for (int i = 0; i < 12; ++i) {
        CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
    }
    CHECK(dsc[12] == LV_GRID_TEMPLATE_LAST);
}

TEST_CASE_METHOD(GridLayoutFixture,
                 "GridLayout make_row_dsc: portrait produces correct descriptor length",
                 "[grid_layout][descriptor][portrait]") {
    auto& lm = helix::LayoutManager::instance();
    lm.init(480, 1600); // PORTRAIT -> 10 rows

    auto dsc = GridLayout::make_row_dsc(4); // XLARGE breakpoint
    REQUIRE(dsc.size() == 11);              // 10 FR values + terminator
    for (int i = 0; i < 10; ++i) {
        CHECK(dsc[static_cast<size_t>(i)] == LV_GRID_FR(1));
    }
    CHECK(dsc[10] == LV_GRID_TEMPLATE_LAST);
}

TEST_CASE_METHOD(GridLayoutFixture,
                 "GridLayout instance: ultrawide dimensions match static accessors",
                 "[grid_layout][instance][ultrawide]") {
    auto& lm = helix::LayoutManager::instance();
    lm.init(1920, 440); // ULTRAWIDE

    GridLayout grid(1); // SMALL breakpoint
    CHECK(grid.cols() == 12);
    CHECK(grid.rows() == 4);
    CHECK(grid.cols() == GridLayout::get_cols(1));
    CHECK(grid.rows() == GridLayout::get_rows(1));
}
