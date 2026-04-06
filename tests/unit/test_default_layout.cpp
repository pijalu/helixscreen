// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace fs = std::filesystem;

// ============================================================================
// RAII helper: change CWD to a temp directory, restore on destruction
// ============================================================================
//
// build_default_grid() opens "config/default_layout.json" relative to CWD.
// Tests use this guard to control which file (if any) the function sees.
//
// NOTE: The breakpoint subject is a zero-initialized static lv_subject_t in
// theme_manager.cpp. In tests (no LVGL theme init), lv_subject_get_int()
// returns 0, which maps to breakpoint index 0 = "tiny". All test JSON
// placements must use "tiny" to match the runtime breakpoint.

class TempCwdGuard {
  public:
    TempCwdGuard() {
        char buf[4096];
        auto* r = getcwd(buf, sizeof(buf));
        (void)r;
        original_cwd_ = buf;
        tmp_dir_ = fs::temp_directory_path() / ("helix_test_layout_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);
        int rc = chdir(tmp_dir_.c_str());
        (void)rc;

        // Reset breakpoint subject to 0 (tiny) — prior tests may have
        // initialized it to a different breakpoint index via theme_manager_init
        lv_subject_t* bp = theme_manager_get_breakpoint_subject();
        if (bp && bp->type == LV_SUBJECT_TYPE_INT) {
            lv_subject_set_int(bp, 0);
        }
    }

    ~TempCwdGuard() {
        (void)chdir(original_cwd_.c_str());
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    /// Write config/default_layout.json with given content
    void write_layout(const std::string& content) {
        fs::create_directories(tmp_dir_ / "config");
        std::ofstream out(tmp_dir_ / "config" / "default_layout.json");
        out << content;
        out.close();
    }

    /// Remove config/default_layout.json if it exists
    void remove_layout() {
        std::error_code ec;
        fs::remove(tmp_dir_ / "config" / "default_layout.json", ec);
    }

    TempCwdGuard(const TempCwdGuard&) = delete;
    TempCwdGuard& operator=(const TempCwdGuard&) = delete;

  private:
    std::string original_cwd_;
    fs::path tmp_dir_;
};

// ============================================================================
// Helper: count of widget defs that build_default_grid() includes
// ============================================================================
//
// build_default_grid() skips multi_instance widgets (they use dynamic IDs like
// "favorite_macro:0"), so the expected entry count is grid_widget_count() minus
// the number of multi_instance defs.

static size_t grid_widget_count() {
    size_t count = 0;
    for (const auto& def : get_all_widget_defs()) {
        if (!def.multi_instance)
            ++count;
    }
    return count;
}

// ============================================================================
// Helper: find entry by ID in a vector
// ============================================================================

static const PanelWidgetEntry* find_entry(const std::vector<PanelWidgetEntry>& entries,
                                          const std::string& id) {
    for (const auto& e : entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("default_layout: valid JSON with tiny breakpoint produces correct anchors",
          "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            },
            {
                "id": "print_status",
                "placements": {
                    "tiny": { "col": 0, "row": 2, "colspan": 2, "rowspan": 2 }
                }
            },
            {
                "id": "tips",
                "placements": {
                    "tiny": { "col": 2, "row": 0, "colspan": 4, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);
    CHECK(pi->enabled);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->col == 0);
    CHECK(ps->row == 2);
    CHECK(ps->colspan == 2);
    CHECK(ps->rowspan == 2);
    CHECK(ps->enabled);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->col == 2);
    CHECK(tips->row == 0);
    CHECK(tips->colspan == 4);
    CHECK(tips->rowspan == 2);
    CHECK(tips->enabled);
}

TEST_CASE("default_layout: different breakpoints produce different placements",
          "[default_layout]") {
    // Runtime breakpoint is "tiny" (index 0). Providing both tiny and large
    // placements verifies that only the tiny values are selected.
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny":  { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 },
                    "large": { "col": 0, "row": 0, "colspan": 3, "rowspan": 3 }
                }
            },
            {
                "id": "tips",
                "placements": {
                    "tiny":  { "col": 2, "row": 0, "colspan": 2, "rowspan": 2 },
                    "large": { "col": 3, "row": 0, "colspan": 5, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    // Tiny values selected (not large: 3x3)
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    // Tiny values (not large: col=3 5x2)
    CHECK(tips->col == 2);
    CHECK(tips->colspan == 2);
    CHECK(tips->rowspan == 2);
}

TEST_CASE("default_layout: missing file falls back to hardcoded defaults", "[default_layout]") {
    TempCwdGuard guard;
    // No layout file written — config/default_layout.json does not exist

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Hardcoded fallback anchors: printer_image, print_status, tips
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->enabled);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->enabled);
    CHECK(ps->has_grid_position());
    CHECK(ps->col == 0);
    CHECK(ps->row == 2);
    CHECK(ps->colspan == 2);
    CHECK(ps->rowspan == 2);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->enabled);
    CHECK(tips->has_grid_position());
    CHECK(tips->col == 2);
    CHECK(tips->row == 0);
    CHECK(tips->colspan == 4);
    CHECK(tips->rowspan == 2);
}

TEST_CASE("default_layout: malformed JSON falls back gracefully", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout("{ this is not valid json }}}}");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Should get hardcoded fallback anchors
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->has_grid_position());

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->has_grid_position());
}

TEST_CASE("default_layout: empty anchors array falls back to hardcoded defaults",
          "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Empty anchors array -> no anchors loaded -> hardcoded fallback triggered
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->has_grid_position());

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->has_grid_position());
}

TEST_CASE("default_layout: unknown widget ID in JSON is ignored", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "totally_bogus_widget",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 1, "rowspan": 1 }
                }
            },
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // The bogus widget should not appear in entries
    auto* bogus = find_entry(entries, "totally_bogus_widget");
    CHECK(bogus == nullptr);

    // The valid widget should be anchored
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
}

TEST_CASE("default_layout: missing breakpoint in placements causes fallback", "[default_layout]") {
    TempCwdGuard guard;
    // Only define "large" placements — runtime breakpoint is "tiny", so no match.
    // With no anchors matched, the empty vector triggers hardcoded fallback.
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "large": { "col": 0, "row": 0, "colspan": 3, "rowspan": 3 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // No anchors matched for tiny breakpoint -> empty anchors -> hardcoded fallback
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);
}

TEST_CASE("default_layout: partial breakpoint match does not trigger fallback",
          "[default_layout]") {
    TempCwdGuard guard;
    // One anchor has "tiny" placement, one only has "large"
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            },
            {
                "id": "tips",
                "placements": {
                    "large": { "col": 3, "row": 0, "colspan": 5, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // printer_image has tiny placement -> anchored from JSON
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->colspan == 2);

    // tips has only large placement -> not matched for "tiny".
    // But since at least one anchor was loaded, fallback is NOT triggered.
    // So tips gets auto-placed (col=-1, row=-1).
    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK_FALSE(tips->has_grid_position());
}

TEST_CASE("default_layout: result always has at least some enabled widgets", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE_FALSE(entries.empty());

    bool any_enabled = std::any_of(entries.begin(), entries.end(),
                                   [](const PanelWidgetEntry& e) { return e.enabled; });
    CHECK(any_enabled);
}

TEST_CASE("default_layout: result always has at least some enabled widgets even with missing file",
          "[default_layout]") {
    TempCwdGuard guard;
    // No layout file at all

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE_FALSE(entries.empty());

    bool any_enabled = std::any_of(entries.begin(), entries.end(),
                                   [](const PanelWidgetEntry& e) { return e.enabled; });
    CHECK(any_enabled);
}

TEST_CASE("default_layout: non-anchor widgets get auto-place coordinates", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    for (const auto& e : entries) {
        if (e.id == "printer_image")
            continue;
        INFO("Widget " << e.id << " col=" << e.col << " row=" << e.row);
        // All non-anchor widgets should have col=-1, row=-1 (auto-placed)
        CHECK(e.col == -1);
        CHECK(e.row == -1);
    }
}

TEST_CASE("default_layout: anchor with empty id is skipped", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 1, "rowspan": 1 }
                }
            },
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Should not crash, printer_image should still be anchored
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
}

TEST_CASE("default_layout: JSON with missing anchors key falls back to hardcoded defaults",
          "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({ "something_else": true })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // anchors key missing -> .value("anchors", json::array()) returns empty ->
    // no anchors loaded -> hardcoded fallback
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);
}

TEST_CASE("default_layout: anchor placements default col/row/span values when omitted",
          "[default_layout]") {
    TempCwdGuard guard;
    // Placement exists for "tiny" but is missing some fields
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 1 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    // col from JSON, row/colspan/rowspan use .value() defaults (0, 1, 1)
    CHECK(pi->col == 1);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 1);
    CHECK(pi->rowspan == 1);
}

TEST_CASE("default_layout: custom anchor positions from JSON override hardcoded defaults",
          "[default_layout]") {
    TempCwdGuard guard;
    // Use non-default positions to verify JSON takes priority over hardcoded values
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 5, "row": 3, "colspan": 1, "rowspan": 1 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    // JSON values should override the hardcoded fallback positions
    CHECK(pi->col == 5);
    CHECK(pi->row == 3);
    CHECK(pi->colspan == 1);
    CHECK(pi->rowspan == 1);
}

TEST_CASE("default_layout: all registry widgets present in result regardless of JSON content",
          "[default_layout]") {
    TempCwdGuard guard;
    // Only anchor one widget — all others should still appear in result
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    const auto& defs = get_all_widget_defs();
    REQUIRE(entries.size() == grid_widget_count());

    // Every non-multi-instance registry widget must appear exactly once
    std::set<std::string> entry_ids;
    for (const auto& e : entries) {
        entry_ids.insert(e.id);
    }
    for (const auto& def : defs) {
        if (def.multi_instance)
            continue;
        CHECK(entry_ids.count(def.id) == 1);
    }
}
