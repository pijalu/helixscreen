// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../../include/temp_graph_controller.h"
#include "../../include/ui_temp_graph.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

class TempGraphControllerFixture {
  public:
    TempGraphControllerFixture() {
        lv_init_safe();

        lv_display_t* disp = lv_display_create(800, 480);
        alignas(64) static lv_color_t buf1[800 * 10];
        lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

        screen = lv_obj_create(NULL);

        // Initialize PrinterState subjects (needed by controller's setup_observers)
        get_printer_state().init_subjects(false);
    }

    ~TempGraphControllerFixture() {}

    lv_obj_t* screen;
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller creates graph with minimal config",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series.clear(); // No series — avoids printer state lookups for sensors

    auto controller = std::make_unique<TempGraphController>(screen, cfg);

    REQUIRE(controller->is_valid());
    REQUIRE(controller->graph() != nullptr);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller with series specs returns valid IDs",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);

    REQUIRE(controller->is_valid());

    // series_id_for should return valid (>= 0) IDs for added series
    int extruder_id = controller->series_id_for("extruder");
    int bed_id = controller->series_id_for("heater_bed");
    REQUIRE(extruder_id >= 0);
    REQUIRE(bed_id >= 0);
    REQUIRE(extruder_id != bed_id);

    // Nonexistent series returns -1
    REQUIRE(controller->series_id_for("nonexistent_sensor") == -1);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller pause and resume do not crash",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    REQUIRE_NOTHROW(controller->pause());
    REQUIRE_NOTHROW(controller->resume());
    REQUIRE_NOTHROW(controller->pause());
    REQUIRE_NOTHROW(controller->resume());
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller set_features applies feature flags",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Set a reduced feature set (lines are always forced on)
    uint32_t features = TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_Y_AXIS;
    controller->set_features(features);

    uint32_t active = ui_temp_graph_get_features(controller->graph());
    // LINES is always forced on
    REQUIRE((active & TEMP_GRAPH_FEATURE_LINES) != 0);
    REQUIRE((active & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
    // Features we did NOT set should be off
    REQUIRE((active & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
    REQUIRE((active & TEMP_GRAPH_FEATURE_GRADIENTS) == 0);
}

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Controller rebuild keeps graph valid and series intact",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    REQUIRE(controller->series_id_for("extruder") >= 0);

    // Rebuild should recreate graph and series without crash
    REQUIRE_NOTHROW(controller->rebuild());

    REQUIRE(controller->is_valid());
    REQUIRE(controller->graph() != nullptr);
    REQUIRE(controller->series_id_for("extruder") >= 0);
    REQUIRE(controller->series_id_for("heater_bed") >= 0);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller with custom scale params does not crash",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.scale_params.step = 25.0f;
    cfg.scale_params.floor = 100.0f;
    cfg.scale_params.ceiling = 400.0f;
    cfg.scale_params.expand_threshold = 0.85f;
    cfg.scale_params.shrink_threshold = 0.55f;

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());
    REQUIRE(controller->graph() != nullptr);
}

TEST_CASE_METHOD(TempGraphControllerFixture, "Controller destruction with series is safe",
                 "[controller][temp_graph_controller]") {
    TempGraphControllerConfig cfg;
    cfg.series = {
        {"extruder", lv_color_hex(0xFF4444), true},
        {"heater_bed", lv_color_hex(0x88C0D0), true},
    };

    // Create with active series and observers, then immediately destroy
    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Destruction tears down observers, drains queue, destroys graph
    REQUIRE_NOTHROW(controller.reset());
    REQUIRE(controller == nullptr);
}

// Reproducer for: chamber series klipper_name is "heater_generic chamber" (full Klipper
// object name), not "chamber". setup_observers() must resolve it to chamber temp/target
// subjects — an exact match on "chamber" would silently skip, leaving the graph empty.
TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Chamber series with heater_generic prefix resolves to chamber subjects",
                 "[controller][temp_graph_controller][chamber]") {
    auto& ps = get_printer_state();

    // Set chamber temp/target subjects to known values
    lv_subject_set_int(ps.get_chamber_temp_subject(), 423);   // 42.3°C
    lv_subject_set_int(ps.get_chamber_target_subject(), 500); // 50.0°C

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"heater_generic chamber", lv_color_hex(0xA3BE8C), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    // Verify series ID was assigned
    int chamber_id = controller->series_id_for("heater_generic chamber");
    REQUIRE(chamber_id >= 0);

    // Pump the observer callbacks through UpdateQueue
    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain();
    lv_timer_handler_safe();

    // The chart should have received data — verify the series has points
    auto* graph = controller->graph();
    REQUIRE(graph != nullptr);
    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    REQUIRE(chart != nullptr);
    uint32_t point_cnt = lv_chart_get_point_count(chart);
    REQUIRE(point_cnt > 0);

    // Verify the actual data point was set.
    // The chart stores values as deci-degrees (temp * 10).
    // 42.3°C → 423 in chart storage.
    lv_chart_series_t* ser = lv_chart_get_series_next(chart, nullptr);
    REQUIRE(ser != nullptr);
    int32_t* y_points = lv_chart_get_series_y_array(chart, ser);
    REQUIRE(y_points != nullptr);
    // On first value, the series is backfilled with the initial temp → 423 (deci-degrees)
    REQUIRE(y_points[0] == 423);
}

TEST_CASE_METHOD(TempGraphControllerFixture,
                 "Chamber series with temperature_fan prefix resolves to chamber subjects",
                 "[controller][temp_graph_controller][chamber]") {
    auto& ps = get_printer_state();

    // Set chamber temp/target to known values
    lv_subject_set_int(ps.get_chamber_temp_subject(), 385);   // 38.5°C
    lv_subject_set_int(ps.get_chamber_target_subject(), 450); // 45.0°C

    TempGraphControllerConfig cfg;
    cfg.series = {
        {"temperature_fan chamber", lv_color_hex(0xA3BE8C), true},
    };

    auto controller = std::make_unique<TempGraphController>(screen, cfg);
    REQUIRE(controller->is_valid());

    int chamber_id = controller->series_id_for("temperature_fan chamber");
    REQUIRE(chamber_id >= 0);

    // Pump observer callbacks
    auto& queue = helix::ui::UpdateQueue::instance();
    queue.drain();
    lv_timer_handler_safe();

    auto* graph = controller->graph();
    REQUIRE(graph != nullptr);
    lv_obj_t* chart = ui_temp_graph_get_chart(graph);
    REQUIRE(chart != nullptr);

    lv_chart_series_t* ser = lv_chart_get_series_next(chart, nullptr);
    REQUIRE(ser != nullptr);
    int32_t* y_points = lv_chart_get_series_y_array(chart, ser);
    REQUIRE(y_points != nullptr);
    // 38.5°C → 385 in chart storage (deci-degrees)
    REQUIRE(y_points[0] == 385);
}
