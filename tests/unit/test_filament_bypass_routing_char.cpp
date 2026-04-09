// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_bypass_routing_char.cpp
 * @brief Characterization tests for filament load/unload routing with AMS bypass
 *
 * Run with: ./build/bin/helix-tests "[filament][bypass][char]"
 *
 * Tests the routing decision in FilamentPanel::execute_load() and execute_unload():
 * - AMS backend active + bypass OFF → route through AMS (slot selection / backend unload)
 * - AMS backend active + bypass ON  → use standard macro / G-code fallback
 * - No AMS backend                  → use standard macro / G-code fallback
 *
 * The actual FilamentPanel is tightly coupled to LVGL, so this characterization test
 * mirrors the routing decision logic with a lightweight state machine.
 */

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Helper - mirrors FilamentPanel::execute_load/execute_unload routing
// ============================================================================

/**
 * @brief Simulates the filament load/unload routing decision from FilamentPanel
 *
 * Mirrors the decision tree in execute_load() and execute_unload() without
 * requiring the full LVGL/MoonrakerAPI infrastructure.
 */
class FilamentRoutingStateMachine {
  public:
    enum class Route {
        AMS_PANEL,       // Load: navigate to AMS panel for slot selection
        AMS_UNLOAD,      // Unload: backend->unload_filament()
        STANDARD_MACRO,  // Run LOAD_FILAMENT / UNLOAD_FILAMENT macro
        FALLBACK_GCODE,  // Raw G-code extrusion
        BLOCKED_NO_LOAD, // AMS unload: no filament loaded
    };

    struct AmsState {
        bool backend_active = false;
        bool bypass_active = false;
        bool filament_loaded = false;
        int current_slot = -1; // -2 = bypass, -1 = none, 0+ = slot index
    };

    struct MacroState {
        bool load_macro_available = false;
        bool unload_macro_available = false;
    };

    AmsState ams;
    MacroState macros;

    /**
     * @brief Determine load routing — mirrors execute_load() decision tree
     *
     * From ui_panel_filament.cpp:
     *   AmsBackend* backend = AmsState::instance().get_backend();
     *   if (backend && !backend->is_bypass_active()) {
     *       // redirect to AMS panel
     *   }
     *   // else: standard macro or fallback G-code
     */
    Route resolve_load() const {
        if (ams.backend_active && !ams.bypass_active) {
            return Route::AMS_PANEL;
        }

        if (macros.load_macro_available) {
            return Route::STANDARD_MACRO;
        }

        return Route::FALLBACK_GCODE;
    }

    /**
     * @brief Determine unload routing — mirrors execute_unload() decision tree
     *
     * From ui_panel_filament.cpp:
     *   AmsBackend* backend = AmsState::instance().get_backend();
     *   if (backend && !backend->is_bypass_active()) {
     *       // check filament_loaded / current_slot, then backend->unload_filament()
     *   }
     *   // else: standard macro or fallback G-code
     */
    Route resolve_unload() const {
        if (ams.backend_active && !ams.bypass_active) {
            if (!ams.filament_loaded && ams.current_slot < 0) {
                return Route::BLOCKED_NO_LOAD;
            }
            return Route::AMS_UNLOAD;
        }

        if (macros.unload_macro_available) {
            return Route::STANDARD_MACRO;
        }

        return Route::FALLBACK_GCODE;
    }
};

using Route = FilamentRoutingStateMachine::Route;

// ============================================================================
// LOAD routing tests
// ============================================================================

TEST_CASE("Filament load routing", "[filament][bypass][char]") {
    FilamentRoutingStateMachine sm;

    SECTION("no AMS backend — uses standard macro") {
        sm.ams.backend_active = false;
        sm.macros.load_macro_available = true;

        REQUIRE(sm.resolve_load() == Route::STANDARD_MACRO);
    }

    SECTION("no AMS backend, no macro — uses fallback G-code") {
        sm.ams.backend_active = false;
        sm.macros.load_macro_available = false;

        REQUIRE(sm.resolve_load() == Route::FALLBACK_GCODE);
    }

    SECTION("AMS active, bypass OFF — redirects to AMS panel") {
        sm.ams.backend_active = true;
        sm.ams.bypass_active = false;
        sm.macros.load_macro_available = true; // should be ignored

        REQUIRE(sm.resolve_load() == Route::AMS_PANEL);
    }

    SECTION("AMS active, bypass ON — uses standard macro (not AMS panel)") {
        sm.ams.backend_active = true;
        sm.ams.bypass_active = true;
        sm.macros.load_macro_available = true;

        REQUIRE(sm.resolve_load() == Route::STANDARD_MACRO);
    }

    SECTION("AMS active, bypass ON, no macro — uses fallback G-code") {
        sm.ams.backend_active = true;
        sm.ams.bypass_active = true;
        sm.macros.load_macro_available = false;

        REQUIRE(sm.resolve_load() == Route::FALLBACK_GCODE);
    }
}

// ============================================================================
// UNLOAD routing tests
// ============================================================================

TEST_CASE("Filament unload routing", "[filament][bypass][char]") {
    FilamentRoutingStateMachine sm;

    SECTION("no AMS backend — uses standard macro") {
        sm.ams.backend_active = false;
        sm.macros.unload_macro_available = true;

        REQUIRE(sm.resolve_unload() == Route::STANDARD_MACRO);
    }

    SECTION("no AMS backend, no macro — uses fallback G-code") {
        sm.ams.backend_active = false;
        sm.macros.unload_macro_available = false;

        REQUIRE(sm.resolve_unload() == Route::FALLBACK_GCODE);
    }

    SECTION("AMS active, bypass OFF, filament loaded — AMS backend unload") {
        sm.ams.backend_active = true;
        sm.ams.bypass_active = false;
        sm.ams.filament_loaded = true;
        sm.ams.current_slot = 2;

        REQUIRE(sm.resolve_unload() == Route::AMS_UNLOAD);
    }

    SECTION("AMS active, bypass OFF, no filament — blocked") {
        sm.ams.backend_active = true;
        sm.ams.bypass_active = false;
        sm.ams.filament_loaded = false;
        sm.ams.current_slot = -1;

        REQUIRE(sm.resolve_unload() == Route::BLOCKED_NO_LOAD);
    }

    SECTION("AMS active, bypass ON — uses standard macro (not AMS unload)") {
        sm.ams.backend_active = true;
        sm.ams.bypass_active = true;
        sm.ams.current_slot = -2; // bypass slot
        sm.macros.unload_macro_available = true;

        REQUIRE(sm.resolve_unload() == Route::STANDARD_MACRO);
    }

    SECTION("AMS active, bypass ON, no macro — uses fallback G-code") {
        sm.ams.backend_active = true;
        sm.ams.bypass_active = true;
        sm.ams.current_slot = -2;
        sm.macros.unload_macro_available = false;

        REQUIRE(sm.resolve_unload() == Route::FALLBACK_GCODE);
    }
}

// ============================================================================
// AmsBackendMock integration — verify bypass state affects routing condition
// ============================================================================

#include "ams_backend_mock.h"

TEST_CASE("AMS bypass routing condition with real mock backend", "[filament][bypass][ams]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("backend active, bypass off — would route to AMS") {
        // Mirrors: if (backend && !backend->is_bypass_active())
        bool routes_to_ams = !backend.is_bypass_active();
        REQUIRE(routes_to_ams);
    }

    SECTION("backend active, bypass on — would skip AMS routing") {
        auto result = backend.enable_bypass();
        REQUIRE(result);

        bool routes_to_ams = !backend.is_bypass_active();
        REQUIRE_FALSE(routes_to_ams);
    }

    SECTION("bypass toggled off again — routes to AMS again") {
        backend.enable_bypass();
        backend.disable_bypass();

        bool routes_to_ams = !backend.is_bypass_active();
        REQUIRE(routes_to_ams);
    }

    backend.stop();
}
