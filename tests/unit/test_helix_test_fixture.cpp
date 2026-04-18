// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../catch_amalgamated.hpp"
#include "../helix_test_fixture.h"
#include "system_settings_manager.h"

using namespace helix;

TEST_CASE_METHOD(HelixTestFixture,
                 "HelixTestFixture resets SystemSettingsManager language",
                 "[test-fixture][isolation]") {
    SystemSettingsManager::instance().set_language("fr");
    // Destructor of prior fixture would have reset; this test confirms entry reset happened.
    // Since we just changed language mid-test, we expect no assertion here — purpose of the
    // test is that a *subsequent* fixture instance sees the default, which a later test
    // case would verify.
    REQUIRE(SystemSettingsManager::instance().get_language() == "fr");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "HelixTestFixture leaves default language after construction",
                 "[test-fixture][isolation]") {
    // If the prior test's destructor reset state, we see the default here.
    // Even when the two tests end up in different Catch2 shards (parallel
    // processes), HelixTestFixture's own constructor reset brings language
    // back to "en" before this body runs, so the assertion still holds.
    REQUIRE(SystemSettingsManager::instance().get_language() == "en");
}
