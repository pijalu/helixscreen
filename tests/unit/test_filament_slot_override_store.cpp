// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "hv/json.hpp"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using helix::ams::FilamentSlotOverride;
using helix::ams::FilamentSlotOverrideStore;
using nlohmann::json;

// Grants tests access to private tunables on FilamentSlotOverrideStore.
// Declared friend in the header (per L065: prefer friend-class over test-only
// public setters on production classes).
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_load_timeout(helix::ams::FilamentSlotOverrideStore& store,
                                 std::chrono::milliseconds ms) {
        store.load_timeout_ = ms;
    }
    // Redirect the read-cache to a per-test tmp dir so tests never touch the
    // user's real config. Empty path restores the default (get_user_config_dir).
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

namespace {
// Per-test isolation helper: returns a fresh tmp directory that is unique to
// the calling test (different `suffix` per test), creates it, and cleans up
// on scope exit. Any test that triggers a successful save/clear MUST redirect
// the store's cache_dir_ to this — otherwise Task 6's cache write lands in
// the developer's real config dir and pollutes state across runs.
struct TmpCacheDir {
    std::filesystem::path path;
    explicit TmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("filament_slot_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

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
    TmpCacheDir tmp("save_afc");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

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
    TmpCacheDir tmp("save_updated_at");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

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

TEST_CASE("FilamentSlotOverrideStore clear_async removes single slot",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_single");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed two entries; clearing slot 0 should leave slot 1 untouched.
    nlohmann::json lane1 = {{"lane", "0"}, {"material", "PLA"}};
    nlohmann::json lane2 = {{"lane", "1"}, {"material", "PETG"}};
    api.mock_set_db_value("lane_data", "lane1", lane1);
    api.mock_set_db_value("lane_data", "lane2", lane2);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    bool cb_done = false;
    bool cb_ok = false;
    store.clear_async(0, [&](bool ok, std::string) { cb_ok = ok; cb_done = true; });
    REQUIRE(cb_done);
    CHECK(cb_ok);

    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
    auto lane2_after = api.mock_get_db_value("lane_data", "lane2");
    CHECK(lane2_after["material"] == "PETG");
}

TEST_CASE("FilamentSlotOverrideStore clear_async succeeds for absent slot (idempotent)",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_idempotent");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);

    bool cb_done = false;
    bool cb_ok = false;
    std::string cb_err;
    store.clear_async(3, [&](bool ok, std::string err) {
        cb_ok = ok;
        cb_err = std::move(err);
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(cb_ok);
    CHECK(cb_err.empty());
}

TEST_CASE("FilamentSlotOverrideStore clear_async rejects negative slot_index",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");

    bool cb_done = false;
    bool cb_ok = true;
    store.clear_async(-1, [&](bool ok, std::string) { cb_ok = ok; cb_done = true; });
    REQUIRE(cb_done);
    CHECK(!cb_ok);
}

TEST_CASE("FilamentSlotOverrideStore clear_async handles null callback gracefully",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_null_cb");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    api.mock_set_db_value("lane_data", "lane1", nlohmann::json{{"lane", "0"}});
    // Should not crash with no callback provided.
    store.clear_async(0, {});
    // Verify delete still happened.
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("FilamentSlotOverrideStore clear_async maps 404 error to success",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_404");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    MoonrakerError err;
    err.code = 404;
    err.message = "Key 'lane1' not found";
    api.mock_reject_next_db_delete(err);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    bool cb_done = false;
    bool cb_ok = false;
    store.clear_async(0, [&](bool ok, std::string) {
        cb_ok = ok;
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(cb_ok); // 404 → treated as success
}

TEST_CASE("FilamentSlotOverrideStore clear_async propagates non-missing-key errors",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    MoonrakerError err;
    err.code = 500;
    err.message = "internal server error";
    api.mock_reject_next_db_delete(err);

    FilamentSlotOverrideStore store(&api, "ifs");
    bool cb_done = false;
    bool cb_ok = true;
    std::string cb_err;
    store.clear_async(0, [&](bool ok, std::string e) {
        cb_ok = ok;
        cb_err = std::move(e);
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(!cb_ok);
    CHECK(cb_err.find("internal server error") != std::string::npos);
}

TEST_CASE("FilamentSlotOverrideStore clear_async maps message-based missing-key error to success",
          "[filament_slot_override][slow]") {
    TmpCacheDir tmp("clear_msg_missing");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    MoonrakerError err;
    err.code = 0; // no code, only message
    err.message = "Key 'lane1' in namespace 'lane_data' not found";
    api.mock_reject_next_db_delete(err);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    bool cb_done = false;
    bool cb_ok = false;
    store.clear_async(0, [&](bool ok, std::string) {
        cb_ok = ok;
        cb_done = true;
    });
    REQUIRE(cb_done);
    CHECK(cb_ok); // "not found" substring → treated as success
}

// ============================================================================
// Lifetime safety: callback fires after store destroyed (no use-after-free).
//
// The store's async paths capture only value-copied strings + the user's
// callback — never `this`. These tests prove that discipline: a deferred
// callback is fired AFTER the store has been destroyed. If a future edit
// accidentally captured `this` (or any ref to a store member), this would
// UAF under ASan. With the correct discipline, it must be harmless.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore save_async callback fires after store destroyed (no UAF)",
          "[filament_slot_override][slow][lifetime]") {
    // TmpCacheDir declared outside the store scope so it outlives the deferred
    // cache-write fired below — cache path is value-captured into the lambda
    // before store destruction, but the dir still needs to exist when the
    // fire happens.
    TmpCacheDir tmp("lifetime_save");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_post();

    bool cb_fired = false;
    {
        FilamentSlotOverrideStore store(&api, "ifs");
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
        FilamentSlotOverride ovr;
        ovr.brand = "Polymaker";
        store.save_async(0, ovr, [&](bool, std::string) { cb_fired = true; });
        // Store goes out of scope here; the deferred callback has NOT yet fired.
    }

    // Now fire the captured success callback. If save_async captured `this` by
    // reference anywhere, this would UAF under ASan. With value-capture
    // discipline, it must be harmless.
    api.fire_deferred_db_post_success();
    CHECK(cb_fired); // user callback still fires — captured by value into the lambda
}

TEST_CASE("FilamentSlotOverrideStore clear_async callback fires after store destroyed (no UAF)",
          "[filament_slot_override][slow][lifetime]") {
    TmpCacheDir tmp("lifetime_clear");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_delete();

    bool cb_fired = false;
    {
        FilamentSlotOverrideStore store(&api, "ifs");
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
        store.clear_async(0, [&](bool, std::string) { cb_fired = true; });
    }

    api.fire_deferred_db_delete_success();
    CHECK(cb_fired);
}

TEST_CASE("FilamentSlotOverrideStore save_async error callback fires after store destroyed (no UAF)",
          "[filament_slot_override][slow][lifetime]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_post();

    bool cb_fired = false;
    bool cb_ok = true;
    {
        FilamentSlotOverrideStore store(&api, "ifs");
        FilamentSlotOverride ovr;
        store.save_async(0, ovr, [&](bool ok, std::string) {
            cb_ok = ok;
            cb_fired = true;
        });
    }

    // Fire the ERROR path after destruction. This is where the spdlog::warn
    // lambda runs — if it captured `this` or accessed a freed member, we'd
    // crash here. The backend_id_copy + key value-capture make this harmless.
    MoonrakerError err;
    err.code = 500;
    err.message = "internal";
    api.fire_deferred_db_post_error(err);

    CHECK(cb_fired);
    CHECK(!cb_ok);
}

// ============================================================================
// load_blocking() cv.wait_for timeout path.
//
// Uses mock_defer_next_db_get() so the namespace GET never completes, forcing
// load_blocking()'s 5s (default) wait to hit its timeout. Overrides the
// timeout to 50ms via the test-access friend class so the test runs fast.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore load_blocking returns empty on timeout",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    api.mock_defer_next_db_get(); // namespace GET never completes

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_load_timeout(store, std::chrono::milliseconds(50));

    auto before = std::chrono::steady_clock::now();
    auto overrides = store.load_blocking();
    auto elapsed = std::chrono::steady_clock::now() - before;

    CHECK(overrides.empty());
    CHECK(elapsed >= std::chrono::milliseconds(50));
    CHECK(elapsed < std::chrono::milliseconds(500)); // didn't hang at 5s default

    // Clean up: fire the deferred callback so the mock's internal state is
    // tidy before the test exits. The shared_ptr-captured state makes this a
    // harmless flip-of-flags on a now-orphaned structure — matching what would
    // happen if a real Moonraker error fired ~55s after we timed out.
    api.fire_deferred_db_get_error(MoonrakerError{});
}

// ============================================================================
// Malformed-data robustness: Moonraker's lane_data namespace could legitimately
// contain non-lane-prefixed keys (AFC metadata) or even corrupt non-object
// values (mis-seeded by another tool). The store must not crash, must skip
// such entries, and must return a clean best-effort result.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore load_blocking handles non-object namespace value",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed the namespace with a non-object (string) value under a lane-prefixed
    // key — malformed. from_lane_data_record guards on !is_object() and returns
    // nullopt, so the entry is silently skipped.
    api.mock_set_db_value("lane_data", "lane1", nlohmann::json("not an object"));

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();
    CHECK(overrides.empty());
}

TEST_CASE("FilamentSlotOverrideStore load_blocking skips non-lane-prefixed keys",
          "[filament_slot_override][slow]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // An AFC printer might store config metadata at non-lane keys alongside our
    // entries. Verify we ignore them without crashing. The prefix filter in
    // load_blocking() (key.rfind("lane", 0) != 0) should drop "metadata".
    api.mock_set_db_value("lane_data", "metadata",
                          nlohmann::json{{"version", 1}, {"note", "AFC config"}});
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"lane", "0"}, {"material", "PLA"}});

    FilamentSlotOverrideStore store(&api, "ifs");
    auto overrides = store.load_blocking();

    REQUIRE(overrides.count(0) == 1);
    CHECK(overrides[0].material == "PLA");
    CHECK(overrides.size() == 1); // metadata key did not leak in
}

// ============================================================================
// Task 6: local JSON read-cache — write side.
//
// The store mirrors every successful save/clear into a local JSON file under
// the user config dir. This cache is a fallback for offline display (Task 7
// wires the load-time fallback); it is NEVER authoritative. These tests
// verify WRITE behavior: save populates, clear erases, other backends'
// entries are preserved, and a corrupt existing cache doesn't kill the save.
// ============================================================================

TEST_CASE("FilamentSlotOverrideStore save_async writes local cache on success",
          "[filament_slot_override][slow]") {
    auto tmp = std::filesystem::temp_directory_path() /
               ("filament_slot_cache_save_" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;

    bool cb_ok = false;
    bool cb_done = false;
    store.save_async(0, ovr, [&](bool ok, std::string) {
        cb_ok = ok;
        cb_done = true;
    });
    REQUIRE(cb_done);
    REQUIRE(cb_ok);

    auto cache = tmp / "filament_slot_overrides.json";
    REQUIRE(std::filesystem::exists(cache));

    std::ifstream in(cache);
    auto doc = nlohmann::json::parse(in);
    CHECK(doc["version"] == 1);
    REQUIRE(doc.contains("ifs"));
    REQUIRE(doc["ifs"]["slots"].contains("0"));
    CHECK(doc["ifs"]["slots"]["0"]["brand"] == "Polymaker");
    CHECK(doc["ifs"]["slots"]["0"]["material"] == "PLA");
    CHECK(doc["ifs"]["slots"]["0"]["color_rgb"] == 0xFF5500u);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("FilamentSlotOverrideStore clear_async erases from local cache on success",
          "[filament_slot_override][slow]") {
    auto tmp = std::filesystem::temp_directory_path() /
               ("filament_slot_cache_clear_" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    // First save to populate cache.
    bool save_done = false;
    store.save_async(0, ovr, [&](bool, std::string) { save_done = true; });
    REQUIRE(save_done);

    // Now clear.
    bool clear_done = false;
    store.clear_async(0, [&](bool, std::string) { clear_done = true; });
    REQUIRE(clear_done);

    auto cache = tmp / "filament_slot_overrides.json";
    std::ifstream in(cache);
    auto doc = nlohmann::json::parse(in);
    CHECK(!doc["ifs"]["slots"].contains("0"));

    std::filesystem::remove_all(tmp);
}

TEST_CASE("FilamentSlotOverrideStore save_async preserves other backends in cache",
          "[filament_slot_override][slow]") {
    auto tmp = std::filesystem::temp_directory_path() /
               ("filament_slot_cache_multi_" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    // Seed the cache with an ACE entry that our IFS save must leave untouched.
    nlohmann::json seeded = {
        {"version", 1},
        {"ace", {{"slots", {{"0", {{"brand", "eSUN"}}}}}}}
    };
    std::ofstream(tmp / "filament_slot_overrides.json") << seeded.dump(2);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    bool save_done = false;
    store.save_async(0, ovr, [&](bool, std::string) { save_done = true; });
    REQUIRE(save_done);

    std::ifstream in(tmp / "filament_slot_overrides.json");
    auto doc = nlohmann::json::parse(in);
    CHECK(doc["ifs"]["slots"]["0"]["brand"] == "Polymaker");
    CHECK(doc["ace"]["slots"]["0"]["brand"] == "eSUN"); // untouched!

    std::filesystem::remove_all(tmp);
}

TEST_CASE("FilamentSlotOverrideStore save_async survives corrupt existing cache",
          "[filament_slot_override][slow]") {
    auto tmp = std::filesystem::temp_directory_path() /
               ("filament_slot_cache_corrupt_" + std::to_string(::getpid()));
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    // Seed with unparseable JSON.
    std::ofstream(tmp / "filament_slot_overrides.json") << "not json at all {{{ ";

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    FilamentSlotOverrideStore store(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp);

    FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    bool save_ok = false;
    bool save_done = false;
    store.save_async(0, ovr, [&](bool ok, std::string) {
        save_ok = ok;
        save_done = true;
    });
    REQUIRE(save_done);
    CHECK(save_ok); // The MR DB save succeeded; cache corruption is not fatal.

    // After save, cache should be reset to a well-formed doc containing our entry.
    std::ifstream in(tmp / "filament_slot_overrides.json");
    auto doc = nlohmann::json::parse(in);
    CHECK(doc["ifs"]["slots"]["0"]["brand"] == "Polymaker");

    std::filesystem::remove_all(tmp);
}
