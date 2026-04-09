// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_filament_bypass_routing_char.cpp
 * @brief Characterization tests for filament load/unload routing with AMS bypass
 *
 * Run with: ./build/bin/helix-tests "[filament][bypass][char]"
 *
 * Tests the routing decision in FilamentPanel::execute_load() and execute_unload():
 *
 * LOAD:
 * - Backend says requires_slot_selection_for_load() → redirect to AMS panel
 * - Backend says !requires_slot_selection_for_load() → standard macro / G-code
 * - No backend → standard macro / G-code
 *
 * UNLOAD:
 * - Backend active → always route through backend (it handles bypass internally)
 * - No backend → standard macro / G-code
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
        bool requires_slot_selection = true; // backend->requires_slot_selection_for_load()
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
     *   if (backend && backend->requires_slot_selection_for_load()) {
     *       // redirect to AMS panel
     *   }
     *   // else: standard macro or fallback G-code
     */
    Route resolve_load() const {
        if (ams.backend_active && ams.requires_slot_selection) {
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
     *   if (backend) {
     *       // check filament_loaded / current_slot, then backend->unload_filament()
     *   }
     *   // else: standard macro or fallback G-code
     *
     * Unload always routes through the backend when one is active — the backend
     * handles bypass internally (e.g. AFC calls the user's unload_filament macro).
     */
    Route resolve_unload() const {
        if (ams.backend_active) {
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

    SECTION("AMS active, requires slot selection — redirects to AMS panel") {
        sm.ams.backend_active = true;
        sm.ams.requires_slot_selection = true;
        sm.macros.load_macro_available = true; // should be ignored

        REQUIRE(sm.resolve_load() == Route::AMS_PANEL);
    }

    SECTION("AMS active, bypass (no slot selection) — uses standard macro") {
        sm.ams.backend_active = true;
        sm.ams.requires_slot_selection = false;
        sm.macros.load_macro_available = true;

        REQUIRE(sm.resolve_load() == Route::STANDARD_MACRO);
    }

    SECTION("AMS active, bypass, no macro — uses fallback G-code") {
        sm.ams.backend_active = true;
        sm.ams.requires_slot_selection = false;
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

    SECTION("AMS active, filament loaded — routes through AMS backend") {
        sm.ams.backend_active = true;
        sm.ams.filament_loaded = true;
        sm.ams.current_slot = 2;

        REQUIRE(sm.resolve_unload() == Route::AMS_UNLOAD);
    }

    SECTION("AMS active, no filament — blocked") {
        sm.ams.backend_active = true;
        sm.ams.filament_loaded = false;
        sm.ams.current_slot = -1;

        REQUIRE(sm.resolve_unload() == Route::BLOCKED_NO_LOAD);
    }

    SECTION("AMS active, bypass ON — still routes through AMS backend") {
        // Backend handles bypass unload internally (e.g. AFC calls user's
        // unload_filament macro when bypass is enabled)
        sm.ams.backend_active = true;
        sm.ams.filament_loaded = true;
        sm.ams.current_slot = -2; // bypass slot

        REQUIRE(sm.resolve_unload() == Route::AMS_UNLOAD);
    }
}

// ============================================================================
// AmsBackendMock integration — verify requires_slot_selection_for_load()
// ============================================================================

#include "ams_backend_mock.h"

TEST_CASE("AMS backend requires_slot_selection_for_load", "[filament][bypass][ams]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("default — requires slot selection") {
        REQUIRE(backend.requires_slot_selection_for_load());
    }

    SECTION("bypass active — does not require slot selection") {
        auto result = backend.enable_bypass();
        REQUIRE(result);

        REQUIRE_FALSE(backend.requires_slot_selection_for_load());
    }

    SECTION("bypass toggled off — requires slot selection again") {
        backend.enable_bypass();
        backend.disable_bypass();

        REQUIRE(backend.requires_slot_selection_for_load());
    }

    backend.stop();
}
