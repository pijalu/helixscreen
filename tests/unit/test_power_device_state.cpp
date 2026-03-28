// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "moonraker_types.h"
#include "power_device_state.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState tracks device state", "[power_device_state]") {
    auto& state = PowerDeviceState::instance();

    std::vector<PowerDevice> devices = {
        {"printer_psu", "gpio", "off", false},
        {"chamber_light", "klipper_device", "on", true},
    };
    state.set_devices(devices);

    REQUIRE(state.device_names().size() == 2);
    REQUIRE(state.is_locked_while_printing("chamber_light") == true);
    REQUIRE(state.is_locked_while_printing("printer_psu") == false);

    SubjectLifetime lt;
    auto* psu_subj = state.get_status_subject("printer_psu", lt);
    REQUIRE(psu_subj != nullptr);
    REQUIRE(lv_subject_get_int(psu_subj) == 0); // off

    auto* light_subj = state.get_status_subject("chamber_light", lt);
    REQUIRE(light_subj != nullptr);
    REQUIRE(lv_subject_get_int(light_subj) == 1); // on

    REQUIRE(state.get_status_subject("nonexistent", lt) == nullptr);

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState replaces devices on re-discovery",
                 "[power_device_state]") {
    auto& state = PowerDeviceState::instance();

    // Initial set
    state.set_devices({{"dev_a", "gpio", "on", false}});
    REQUIRE(state.device_names().size() == 1);

    // Replace with different set
    state.set_devices({{"dev_b", "gpio", "off", true}, {"dev_c", "klipper_device", "on", false}});
    REQUIRE(state.device_names().size() == 2);

    SubjectLifetime lt;
    REQUIRE(state.get_status_subject("dev_a", lt) == nullptr);
    REQUIRE(state.get_status_subject("dev_b", lt) != nullptr);
    REQUIRE(lv_subject_get_int(state.get_status_subject("dev_b", lt)) == 0); // off

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState deinit clears all subjects",
                 "[power_device_state]") {
    auto& state = PowerDeviceState::instance();

    state.set_devices({{"psu", "gpio", "on", false}});
    REQUIRE(state.device_names().size() == 1);

    state.deinit_subjects();
    REQUIRE(state.device_names().empty());

    SubjectLifetime lt;
    REQUIRE(state.get_status_subject("psu", lt) == nullptr);
}
