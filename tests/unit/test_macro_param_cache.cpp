// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_param_cache.h"

#include "../catch_amalgamated.hpp"

using helix::CachedMacroInfo;
using helix::MacroParamCache;
using helix::MacroParamKnowledge;

// ============================================================================
// MacroParamCache Tests
// ============================================================================

TEST_CASE("MacroParamCache populate categorizes params correctly", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro clean_nozzle"] = {
        {"gcode", "{% set TEMP = params.TEMP|default(240)|int %}\nG1 E10"}};
    config["gcode_macro cancel_print"] = {
        {"gcode", "TURN_OFF_HEATERS\nBASE_CANCEL"}};
    config["gcode_macro pause"] = {
        {"gcode", "SAVE_GCODE_STATE\nBASE_PAUSE"}};

    std::unordered_set<std::string> known_macros = {
        "CLEAN_NOZZLE", "CANCEL_PRINT", "PAUSE", "GET_VARIABLE"};

    cache.populate_from_configfile(config, known_macros);

    SECTION("macro with params is KNOWN_PARAMS") {
        auto info = cache.get("CLEAN_NOZZLE");
        REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
        REQUIRE(info.params.size() == 1);
        REQUIRE(info.params[0].name == "TEMP");
        REQUIRE(info.params[0].default_value == "240");
    }

    SECTION("macro without params is KNOWN_NO_PARAMS") {
        auto info = cache.get("CANCEL_PRINT");
        REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
        REQUIRE(info.params.empty());
    }

    SECTION("macro without params (pause) is KNOWN_NO_PARAMS") {
        auto info = cache.get("PAUSE");
        REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
        REQUIRE(info.params.empty());
    }

    SECTION("known macro not in configfile is UNKNOWN") {
        auto info = cache.get("GET_VARIABLE");
        REQUIRE(info.knowledge == MacroParamKnowledge::UNKNOWN);
        REQUIRE(info.params.empty());
    }
}

TEST_CASE("MacroParamCache get returns UNKNOWN for totally unknown macro", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    auto info = cache.get("NONEXISTENT_MACRO");
    REQUIRE(info.knowledge == MacroParamKnowledge::UNKNOWN);
    REQUIRE(info.params.empty());
}

TEST_CASE("MacroParamCache clear resets state", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro test_macro"] = {
        {"gcode", "{% set X = params.X|default(0) %}"}};

    cache.populate_from_configfile(config, {});

    REQUIRE(cache.get("TEST_MACRO").knowledge == MacroParamKnowledge::KNOWN_PARAMS);

    cache.clear();

    REQUIRE(cache.get("TEST_MACRO").knowledge == MacroParamKnowledge::UNKNOWN);
}

TEST_CASE("MacroParamCache case-insensitive lookup", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro print_start"] = {
        {"gcode", "{% set BED = params.BED_TEMP|default(60) %}"}};

    cache.populate_from_configfile(config, {});

    // All case variations should work
    REQUIRE(cache.get("PRINT_START").knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(cache.get("print_start").knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(cache.get("Print_Start").knowledge == MacroParamKnowledge::KNOWN_PARAMS);
}

TEST_CASE("MacroParamCache uppercase configfile keys match known_macros", "[macro_param_cache]") {
    // Real Klipper returns uppercase keys like "gcode_macro CLEAN_NOZZLE"
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro CLEAN_NOZZLE"] = {
        {"gcode", "G1 X{start_x}"},
        {"variable_start_x", "265"}};

    std::unordered_set<std::string> known_macros = {"CLEAN_NOZZLE"};
    cache.populate_from_configfile(config, known_macros);

    // Must NOT be UNKNOWN — the uppercase configfile key must match
    auto info = cache.get("CLEAN_NOZZLE");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(info.params.size() == 1);
    CHECK(info.params[0].name == "START_X");
    CHECK(info.params[0].is_variable);
}

TEST_CASE("MacroParamCache multiple params extracted", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro start_print"] = {
        {"gcode",
         "{% set BED_TEMP = params.BED_TEMP|default(60)|float %}\n"
         "{% set EXTRUDER_TEMP = params.EXTRUDER_TEMP|default(200)|float %}\n"
         "{% set CHAMBER_TEMP = params.CHAMBER_TEMP|default(0)|float %}\nG28"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("START_PRINT");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(info.params.size() == 3);
}

TEST_CASE("MacroParamCache handles missing gcode field", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro no_gcode"] = {{"description", "test"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("NO_GCODE");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_NO_PARAMS);
}

// ============================================================================
// variable_* field parsing tests
// ============================================================================

TEST_CASE("MacroParamCache extracts variable_* fields", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro clean_nozzle"] = {
        {"gcode", "G1 X{start_x} Y{start_y}"},
        {"variable_start_x", "265"},
        {"variable_start_y", "298"},
        {"variable_wipe_qty", "4"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("CLEAN_NOZZLE");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(info.params.size() == 3);

    // All should be variables
    std::map<std::string, std::string> var_map;
    for (const auto& p : info.params) {
        REQUIRE(p.is_variable);
        var_map[p.name] = p.default_value;
    }
    CHECK(var_map["START_X"] == "265");
    CHECK(var_map["START_Y"] == "298");
    CHECK(var_map["WIPE_QTY"] == "4");
}

TEST_CASE("MacroParamCache mixed params and variables", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro start_print"] = {
        {"gcode", "{% set BED = params.BED_TEMP|default(60) %}"},
        {"variable_idle_state", "false"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("START_PRINT");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(info.params.size() == 2);

    // One param, one variable
    bool found_param = false;
    bool found_var = false;
    for (const auto& p : info.params) {
        if (p.name == "BED_TEMP") {
            CHECK_FALSE(p.is_variable);
            CHECK(p.default_value == "60");
            found_param = true;
        }
        if (p.name == "IDLE_STATE") {
            CHECK(p.is_variable);
            CHECK(p.default_value == "false");
            found_var = true;
        }
    }
    CHECK(found_param);
    CHECK(found_var);
}

TEST_CASE("MacroParamCache variable-only macro is KNOWN_PARAMS", "[macro_param_cache]") {
    auto& cache = MacroParamCache::instance();
    cache.clear();

    nlohmann::json config;
    config["gcode_macro bedfanvars"] = {
        {"gcode", ""},
        {"variable_threshold", "100"},
        {"variable_fast", "0.6"},
        {"variable_slow", "0.2"}};

    cache.populate_from_configfile(config, {});

    auto info = cache.get("BEDFANVARS");
    REQUIRE(info.knowledge == MacroParamKnowledge::KNOWN_PARAMS);
    REQUIRE(info.params.size() == 3);
    for (const auto& p : info.params) {
        CHECK(p.is_variable);
    }
}

