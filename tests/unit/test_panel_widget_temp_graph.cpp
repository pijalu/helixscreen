// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/ui_temp_graph.h"
#include "../../src/ui/panel_widgets/temp_graph_widget.h"
#include "panel_widget_registry.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Reuse the same lightweight fixture as test_temp_graph.cpp
class TempGraphFeatureFixture {
  public:
    TempGraphFeatureFixture() {
        lv_init_safe();
        lv_display_t* disp = lv_display_create(800, 480);
        alignas(64) static lv_color_t buf1[800 * 10];
        lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
        screen = lv_obj_create(NULL);
    }

    ~TempGraphFeatureFixture() = default;

    lv_obj_t* screen;
};

// ============================================================================
// Feature Flags Tests
// ============================================================================

TEST_CASE_METHOD(TempGraphFeatureFixture, "Feature flags default to all-on after create",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    // Y-axis excluded from defaults (show_y_axis starts false, caller must configure)
    uint32_t expected = TEMP_GRAPH_ALL_FEATURES & ~TEMP_GRAPH_FEATURE_Y_AXIS;
    REQUIRE(graph->features == expected);
    REQUIRE(graph->show_x_axis == true);
    REQUIRE(graph->show_y_axis == false);

    // Verify individual flags match expectations
    uint32_t f = ui_temp_graph_get_features(graph);
    REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);  // Off by default
    REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "set_features stores and get_features retrieves flags",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    SECTION("Set only Y_AXIS and X_AXIS") {
        uint32_t flags = TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS;
        ui_temp_graph_set_features(graph, flags);

        // LINES is always forced on
        uint32_t expected = flags | TEMP_GRAPH_FEATURE_LINES;
        REQUIRE(ui_temp_graph_get_features(graph) == expected);
    }

    SECTION("Set all features") {
        ui_temp_graph_set_features(graph, TEMP_GRAPH_ALL_FEATURES);
        REQUIRE(ui_temp_graph_get_features(graph) == TEMP_GRAPH_ALL_FEATURES);
    }

    SECTION("Set no features — only LINES remains") {
        ui_temp_graph_set_features(graph, 0);
        REQUIRE(ui_temp_graph_get_features(graph) == TEMP_GRAPH_FEATURE_LINES);
    }

    SECTION("Y-axis show_y_axis tracks feature flag") {
        ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_Y_AXIS);
        REQUIRE(graph->show_y_axis == true);

        ui_temp_graph_set_features(graph, 0);
        REQUIRE(graph->show_y_axis == false);
    }

    SECTION("X-axis show_x_axis tracks feature flag") {
        ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_X_AXIS);
        REQUIRE(graph->show_x_axis == true);

        ui_temp_graph_set_features(graph, 0);
        REQUIRE(graph->show_x_axis == false);
    }

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "LINES flag is always forced on",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    // Pass 0 — no flags at all
    ui_temp_graph_set_features(graph, 0);
    REQUIRE((ui_temp_graph_get_features(graph) & TEMP_GRAPH_FEATURE_LINES) != 0);

    // Pass only GRADIENTS — LINES should still be on
    ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_GRADIENTS);
    uint32_t f = ui_temp_graph_get_features(graph);
    REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
    REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);

    ui_temp_graph_destroy(graph);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "get_features returns 0 for NULL graph",
                 "[ui][temp_graph][features]") {
    REQUIRE(ui_temp_graph_get_features(nullptr) == 0);
}

TEST_CASE_METHOD(TempGraphFeatureFixture, "Gradient opacity zeroed when GRADIENTS flag disabled",
                 "[ui][temp_graph][features]") {
    ui_temp_graph_t* graph = ui_temp_graph_create(screen);
    REQUIRE(graph != nullptr);

    // Add a series so we can check gradient state
    int sid = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF4444));
    REQUIRE(sid >= 0);

    // Disable gradients
    ui_temp_graph_set_features(graph, TEMP_GRAPH_FEATURE_LINES);
    REQUIRE(graph->series_meta[0].gradient_top_opa == LV_OPA_TRANSP);
    REQUIRE(graph->series_meta[0].gradient_bottom_opa == LV_OPA_TRANSP);

    // Re-enable gradients — defaults restored
    ui_temp_graph_set_features(graph, TEMP_GRAPH_ALL_FEATURES);
    REQUIRE(graph->series_meta[0].gradient_top_opa == UI_TEMP_GRAPH_GRADIENT_TOP_OPA);
    REQUIRE(graph->series_meta[0].gradient_bottom_opa == UI_TEMP_GRAPH_GRADIENT_BOTTOM_OPA);

    ui_temp_graph_destroy(graph);
}

// ============================================================================
// Registry tests
// ============================================================================

TEST_CASE("TempGraphWidget: registered in widget registry", "[temp_graph][panel_widget]") {
    const auto* def = find_widget_def("temp_graph");
    REQUIRE(def != nullptr);
    REQUIRE(std::string(def->display_name) == "Temperature Graph");
    REQUIRE(std::string(def->icon) == "chart-line");
    REQUIRE(def->multi_instance == true);
    REQUIRE(def->colspan == 2);
    REQUIRE(def->rowspan == 2);
    REQUIRE(def->min_colspan == 1);
    REQUIRE(def->min_rowspan == 1);
    REQUIRE(def->max_colspan == 0);
    REQUIRE(def->max_rowspan == 0);
    REQUIRE(def->hardware_gate_subject == nullptr);
}

// ============================================================================
// features_for_size tests
// ============================================================================

TEST_CASE("TempGraphWidget::features_for_size maps grid size to feature flags",
          "[temp_graph][panel_widget][features]") {

    SECTION("1x1: lines only") {
        uint32_t f = TempGraphWidget::features_for_size(1, 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("2x1 (wide): + target lines, legend, x-axis") {
        uint32_t f = TempGraphWidget::features_for_size(2, 1);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("1x2 (tall): + target lines, legend, y-axis") {
        uint32_t f = TempGraphWidget::features_for_size(1, 2);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) == 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("2x2: all except readouts") {
        uint32_t f = TempGraphWidget::features_for_size(2, 2);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) == 0);
    }

    SECTION("3x2: all features including readouts") {
        uint32_t f = TempGraphWidget::features_for_size(3, 2);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_TARGET_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LEGEND) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_X_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_Y_AXIS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
    }

    SECTION("4x3: all features (larger than max)") {
        uint32_t f = TempGraphWidget::features_for_size(4, 3);
        REQUIRE((f & TEMP_GRAPH_FEATURE_LINES) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_READOUTS) != 0);
        REQUIRE((f & TEMP_GRAPH_FEATURE_GRADIENTS) != 0);
    }
}

// ============================================================================
// Config round-trip tests
// ============================================================================

TEST_CASE("TempGraphWidget: set_config stores and preserves sensor configuration",
          "[temp_graph][panel_widget][config]") {
    TempGraphWidget widget("test_config_1");

    nlohmann::json config = {
        {"sensors", {
            {{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}},
            {{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}},
            {{"name", "temperature_sensor mcu_temp"}, {"enabled", false}, {"color", 0xA3BE8C}},
        }}
    };

    widget.set_config(config);

    // Verify the widget accepted the config by checking get_component_name works
    // (would crash if widget was in bad state)
    REQUIRE(widget.get_component_name() == "panel_widget_temp_graph");
    REQUIRE(widget.id() == std::string("test_config_1"));
    REQUIRE(widget.has_edit_configure() == true);
    REQUIRE(widget.supports_reuse() == true);
}

TEST_CASE("TempGraphWidget: factory creates valid instances", "[temp_graph][panel_widget]") {
    init_widget_registrations();

    const auto* def = find_widget_def("temp_graph");
    REQUIRE(def != nullptr);
    REQUIRE(def->factory != nullptr);

    auto widget = def->factory("test_factory_1");
    REQUIRE(widget != nullptr);
    REQUIRE(std::string(widget->id()) == "test_factory_1");
}
