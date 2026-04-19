// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "hv/json.hpp"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

using helix::ams::FilamentSlotOverride;
using helix::ams::FilamentSlotOverrideStore;
using nlohmann::json;

TEST_CASE("FilamentSlotOverride roundtrips through JSON", "[filament_slot_override]") {
    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite PLA Orange";
    ovr.spoolman_id = 42;
    ovr.spoolman_vendor_id = 7;
    ovr.remaining_weight_g = 850.0f;
    ovr.total_weight_g = 1000.0f;
    ovr.color_rgb = 0xFF5500;
    ovr.color_name = "Orange";
    ovr.material = "PLA";
    ovr.updated_at = std::chrono::system_clock::from_time_t(1713441296);

    json j = helix::ams::to_json(ovr);
    FilamentSlotOverride round = helix::ams::from_json(j);

    CHECK(round.brand == ovr.brand);
    CHECK(round.spool_name == ovr.spool_name);
    CHECK(round.spoolman_id == ovr.spoolman_id);
    CHECK(round.spoolman_vendor_id == ovr.spoolman_vendor_id);
    CHECK(round.remaining_weight_g == ovr.remaining_weight_g);
    CHECK(round.total_weight_g == ovr.total_weight_g);
    CHECK(round.color_rgb == ovr.color_rgb);
    CHECK(round.color_name == ovr.color_name);
    CHECK(round.material == ovr.material);
    CHECK(round.updated_at == ovr.updated_at);
}

TEST_CASE("FilamentSlotOverrideStore load returns empty when namespace absent",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    // lane_data namespace has no entries for any slot.
    FilamentSlotOverrideStore store(&api, "ifs");

    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore load_blocking parses lane_data entries",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed lane_data namespace with two AFC-shaped entries + our extensions.
    json lane1 = {
        {"lane", "0"},
        {"color", "#FF5500"},
        {"material", "PLA"},
        {"vendor", "Polymaker"},
        {"spool_id", 42},
        {"spool_name", "PolyLite PLA Orange"},
        {"remaining_weight_g", 850.0},
    };
    json lane2 = {
        {"lane", "1"},
        {"color", "0x00FF00"}, // 0x prefix accepted
        {"material", "PETG"},
    };
    api.mock_set_db_value("lane_data", "lane1", lane1);
    api.mock_set_db_value("lane_data", "lane2", lane2);

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();

    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].brand == "Polymaker");
    CHECK(overrides[0].material == "PLA");
    CHECK(overrides[0].color_rgb == 0xFF5500u);
    CHECK(overrides[0].spoolman_id == 42);
    CHECK(overrides[0].spool_name == "PolyLite PLA Orange");
    CHECK(overrides[0].remaining_weight_g == 850.0f);

    REQUIRE(overrides.count(1) == 1);
    CHECK(overrides[1].material == "PETG");
    CHECK(overrides[1].color_rgb == 0x00FF00u);
    // brand / spoolman_id not present in lane2 entry - default values
    CHECK(overrides[1].brand == "");
    CHECK(overrides[1].spoolman_id == 0);
}

TEST_CASE("FilamentSlotOverrideStore load_blocking skips entries missing lane field",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    // Entry without the required "lane" field - should be skipped silently.
    json bad = {{"material", "PLA"}};
    api.mock_set_db_value("lane_data", "lane1", bad);

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore load_blocking rejects negative lane values",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    json bad = {{"lane", "-1"}, {"material", "PLA"}};
    api.mock_set_db_value("lane_data", "lane1", bad);

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore save_async writes AFC-shaped record to lane_data",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.spoolman_id = 42;
    ovr.remaining_weight_g = 850.0f;
    ovr.total_weight_g = 1000.0f;

    bool cb_done = false;
    bool cb_ok = false;
    std::string cb_err;

    // slot index 0 → lane1 key, lane="0" field
    store.save_async(0, ovr, [&](bool ok, std::string err) {
        cb_ok = ok;
        cb_err = std::move(err);
        cb_done = true;
    });

    // MoonrakerAPIMock fires callbacks synchronously in-call.
    REQUIRE(cb_done);
    CHECK(cb_ok);
    CHECK(cb_err.empty());

    // Verify the stored record.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["lane"] == "0");
    CHECK(stored["color"] == "#FF5500");
    CHECK(stored["material"] == "PLA");
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["remaining_weight_g"] == 850.0f);
    CHECK(stored["total_weight_g"] == 1000.0f);
    CHECK(stored.contains("scan_time"));  // set by save_async
}

TEST_CASE("FilamentSlotOverrideStore save_async sets updated_at on the stored record",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");

    FilamentSlotOverride ovr;
    ovr.brand = "eSUN";
    // updated_at deliberately left at default (epoch)

    bool cb_done = false;
    store.save_async(2, ovr, [&](bool, std::string) { cb_done = true; });
    REQUIRE(cb_done);

    // slot 2 → lane3 key
    auto stored = api.mock_get_db_value("lane_data", "lane3");
    REQUIRE(stored.contains("scan_time"));
    REQUIRE(stored["scan_time"].is_string());

    // Spot check the timestamp is ISO-8601-shaped.
    std::string ts = stored["scan_time"].get<std::string>();
    CHECK(ts.size() >= 20);  // "YYYY-MM-DDTHH:MM:SSZ" = 20 chars
    CHECK(ts.back() == 'Z'); // UTC
    CHECK(ts[10] == 'T');    // ISO-8601 separator
    // Confirm the stamp is recent, not leaking the epoch sentinel through the
    // serializer's "only emit when time_since_epoch > 0" guard.
    CHECK(ts != "1970-01-01T00:00:00Z");

    // Caller's override must NOT have been mutated.
    CHECK(ovr.updated_at == std::chrono::system_clock::time_point{});
}

TEST_CASE("FilamentSlotOverrideStore save_async reports error on MR DB failure",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");

    api.mock_reject_next_db_post();

    FilamentSlotOverride ovr;
    ovr.brand = "X";

    bool cb_done = false;
    bool cb_ok = true;
    std::string cb_err;
    store.save_async(0, ovr, [&](bool ok, std::string err) {
        cb_ok = ok;
        cb_err = std::move(err);
        cb_done = true;
    });

    REQUIRE(cb_done);
    CHECK(!cb_ok);
    CHECK(!cb_err.empty());

    // Record must NOT have been written on rejection.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    CHECK(stored.is_null());
}
