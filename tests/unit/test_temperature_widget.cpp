// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_service.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/temperature_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Minimal fixture: LVGL display + static PrinterState for subjects
class TempWidgetFixture : public LVGLTestFixture {
  public:
    TempWidgetFixture() {
        if (!s_state) {
            s_state = new PrinterState();
            s_state->init_subjects();
        }
    }

    PrinterState& state() {
        return *s_state;
    }

  protected:
    static PrinterState* s_state;
};

PrinterState* TempWidgetFixture::s_state = nullptr;

// Helper: build a mock widget tree that mirrors panel_widget_temperature.xml
// Returns the outer container; creates a child named "temp_btn".
static lv_obj_t* create_mock_temperature_widget(lv_obj_t* parent) {
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_name(container, "panel_widget_temperature");

    lv_obj_t* btn = lv_obj_create(container);
    lv_obj_set_name(btn, "temp_btn");

    return container;
}

TEST_CASE_METHOD(TempWidgetFixture,
                 "TemperatureWidget: user_data on container, per-callback user_data on button",
                 "[temperature_widget][regression]") {
    // Simulate TemperatureService shared resource
    auto tcp = std::make_shared<TemperatureService>(state(), nullptr);
    auto& mgr = PanelWidgetManager::instance();
    mgr.register_shared_resource<TemperatureService>(tcp.get());

    // Build mock widget tree
    lv_obj_t* container = create_mock_temperature_widget(test_screen());
    lv_obj_t* btn = lv_obj_find_by_name(container, "temp_btn");
    REQUIRE(btn != nullptr);

    // Create and attach widget
    TemperatureWidget widget(state(), tcp.get());
    widget.attach(container, test_screen());

    SECTION("user_data is on the container") {
        auto* recovered = static_cast<TemperatureWidget*>(lv_obj_get_user_data(container));
        REQUIRE(recovered == &widget);
    }

    SECTION("user_data is NOT on the button (per-callback user_data used instead)") {
        auto* btn_data = lv_obj_get_user_data(btn);
        // Button uses per-callback user_data via lv_obj_add_event_cb, not obj user_data
        REQUIRE(btn_data == nullptr);
    }

    SECTION("detach clears container user_data") {
        widget.detach();
        auto* recovered = lv_obj_get_user_data(container);
        REQUIRE(recovered == nullptr);
    }

    // Clean up (detach is idempotent)
    widget.detach();
    mgr.clear_shared_resources();
}

TEST_CASE_METHOD(TempWidgetFixture,
                 "TemperatureWidget: click callback recovers widget via per-callback user_data",
                 "[temperature_widget][regression]") {
    auto tcp = std::make_shared<TemperatureService>(state(), nullptr);
    auto& mgr = PanelWidgetManager::instance();
    mgr.register_shared_resource<TemperatureService>(tcp.get());

    lv_obj_t* container = create_mock_temperature_widget(test_screen());
    lv_obj_t* btn = lv_obj_find_by_name(container, "temp_btn");
    REQUIRE(btn != nullptr);

    TemperatureWidget widget(state(), tcp.get());
    widget.attach(container, test_screen());

    // temp_clicked_cb now uses lv_event_get_user_data(e) (per-callback user_data)
    // rather than lv_obj_get_user_data(btn). Verify the container holds the widget
    // pointer (set during attach) so the widget is recoverable.
    auto* recovered = static_cast<TemperatureWidget*>(lv_obj_get_user_data(container));
    REQUIRE(recovered != nullptr);
    REQUIRE(recovered == &widget);

    widget.detach();
    mgr.clear_shared_resources();
}
