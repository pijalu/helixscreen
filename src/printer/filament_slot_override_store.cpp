// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>

namespace helix::ams {

namespace {

std::string format_iso8601(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    std::tm tm{};
    std::istringstream is(s);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (is.fail()) return {};
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

// Convert FilamentSlotOverride + slot_index to the AFC-shaped JSON Orca expects,
// plus our extension fields (prefixed comment fields are HelixScreen-only, silently
// ignored by Orca 2.3.2 which only reads the top 5 required fields).
//
// NOTE on indexing: the Moonraker DB key is 1-based (AFC convention: lane1,
// lane2, ...) but the "lane" field inside the record is 0-based (matches Orca's
// tool-index interpretation). The 1-based key is produced by lane_key() in the
// header; this function writes the 0-based inner field.
nlohmann::json to_lane_data_record(int slot_index, const FilamentSlotOverride& o) {
    nlohmann::json j;
    j["lane"] = std::to_string(slot_index); // REQUIRED by Orca (0-based)
    if (o.color_rgb != 0) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%06X", o.color_rgb);
        j["color"] = buf;
    }
    if (!o.material.empty()) j["material"] = o.material;
    if (!o.brand.empty()) j["vendor"] = o.brand;
    if (o.spoolman_id > 0) j["spool_id"] = o.spoolman_id;
    if (o.updated_at.time_since_epoch().count() > 0) {
        j["scan_time"] = format_iso8601(o.updated_at);
    }
    // bed_temp, nozzle_temp deliberately omitted - unknown to HelixScreen today.
    if (!o.spool_name.empty()) j["spool_name"] = o.spool_name;
    if (o.spoolman_vendor_id > 0) j["spoolman_vendor_id"] = o.spoolman_vendor_id;
    if (o.remaining_weight_g >= 0) j["remaining_weight_g"] = o.remaining_weight_g;
    if (o.total_weight_g >= 0) j["total_weight_g"] = o.total_weight_g;
    if (!o.color_name.empty()) j["color_name"] = o.color_name;
    return j;
}

// Parse AFC-shaped record (+ our extensions) back into FilamentSlotOverride.
// Returns (slot_index, override) where slot_index comes from the "lane" field
// (which Orca requires). nullopt if the record is malformed (non-object or
// missing/invalid "lane" field).
std::optional<std::pair<int, FilamentSlotOverride>>
from_lane_data_record(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("lane")) return std::nullopt;
    int slot_index = 0;
    if (j["lane"].is_string()) {
        try {
            slot_index = std::stoi(j["lane"].get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    } else if (j["lane"].is_number_integer()) {
        slot_index = j["lane"].get<int>();
    } else {
        return std::nullopt;
    }
    // Matches OrcaSlicer's MoonrakerPrinterAgent.cpp:796 — negative lane
    // values are never valid slot indices.
    if (slot_index < 0) return std::nullopt;

    FilamentSlotOverride o;
    if (j.contains("color") && j["color"].is_string()) {
        std::string s = j["color"].get<std::string>();
        if (!s.empty() && s[0] == '#') {
            s = s.substr(1);
        } else if (s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
            s = s.substr(2);
        }
        try {
            o.color_rgb = static_cast<uint32_t>(std::stoul(s, nullptr, 16));
        } catch (...) {
            // Leave color_rgb at default 0 on parse failure.
        }
    }
    o.material = j.value("material", "");
    o.brand = j.value("vendor", "");
    o.spoolman_id = j.value("spool_id", 0);
    if (j.contains("scan_time") && j["scan_time"].is_string()) {
        o.updated_at = parse_iso8601(j["scan_time"].get<std::string>());
    }
    o.spool_name = j.value("spool_name", "");
    o.spoolman_vendor_id = j.value("spoolman_vendor_id", 0);
    o.remaining_weight_g = j.value("remaining_weight_g", -1.0f);
    o.total_weight_g = j.value("total_weight_g", -1.0f);
    o.color_name = j.value("color_name", "");
    return std::make_pair(slot_index, o);
}

} // namespace

nlohmann::json to_json(const FilamentSlotOverride& o) {
    return {
        {"brand", o.brand},
        {"spool_name", o.spool_name},
        {"spoolman_id", o.spoolman_id},
        {"spoolman_vendor_id", o.spoolman_vendor_id},
        {"remaining_weight_g", o.remaining_weight_g},
        {"total_weight_g", o.total_weight_g},
        {"color_rgb", o.color_rgb},
        {"color_name", o.color_name},
        {"material", o.material},
        {"updated_at", format_iso8601(o.updated_at)},
    };
}

FilamentSlotOverride from_json(const nlohmann::json& j) {
    FilamentSlotOverride o;
    o.brand = j.value("brand", "");
    o.spool_name = j.value("spool_name", "");
    o.spoolman_id = j.value("spoolman_id", 0);
    o.spoolman_vendor_id = j.value("spoolman_vendor_id", 0);
    o.remaining_weight_g = j.value("remaining_weight_g", -1.0f);
    o.total_weight_g = j.value("total_weight_g", -1.0f);
    o.color_rgb = j.value("color_rgb", 0u);
    o.color_name = j.value("color_name", "");
    o.material = j.value("material", "");
    if (j.contains("updated_at") && j["updated_at"].is_string()) {
        o.updated_at = parse_iso8601(j["updated_at"].get<std::string>());
    }
    return o;
}

// ============================================================================
// FilamentSlotOverrideStore skeleton (Task 2). Real load/save wiring lands
// in Tasks 3-5; this skeleton exists so other components can depend on the
// class shape now.
// ============================================================================

FilamentSlotOverrideStore::FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id)
    : api_(api), backend_id_(std::move(backend_id)) {}

std::unordered_map<int, FilamentSlotOverride> FilamentSlotOverrideStore::load_blocking() {
    std::unordered_map<int, FilamentSlotOverride> result;
    if (!api_) return result;

    // Wrap sync state in shared_ptr so callbacks firing after our 5s local
    // timeout don't touch a freed stack frame. Moonraker's request tracker has
    // its own ~60s boundary, so an error callback can fire ~55s after we've
    // already returned. Captured by value, the shared_ptr keeps the state
    // alive for the orphaned callback to flip done/got harmlessly.
    // (Same pattern as AmsBackendAce::poll_info in src/printer/ams_backend_ace.cpp)
    struct SyncState {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
        bool got{false};
        nlohmann::json received;
    };
    auto state = std::make_shared<SyncState>();
    const std::string backend_id_copy = backend_id_;
    const std::string namespace_copy = namespace_;

    api_->database_get_namespace(
        namespace_,
        [state](const nlohmann::json& value) {
            std::lock_guard<std::mutex> lk(state->m);
            state->received = value;
            state->got = true;
            state->done = true;
            state->cv.notify_one();
        },
        [state, backend_id_copy, namespace_copy](const MoonrakerError& err) {
            spdlog::debug("[FilamentSlotOverrideStore:{}] database_get_namespace({}) failed: {}",
                          backend_id_copy, namespace_copy, err.message);
            std::lock_guard<std::mutex> lk(state->m);
            state->done = true;
            state->cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> lk(state->m);
        state->cv.wait_for(lk, std::chrono::seconds(5), [&] { return state->done; });
    }

    if (!state->got || !state->received.is_object()) return result;

    for (auto it = state->received.begin(); it != state->received.end(); ++it) {
        const std::string& key = it.key();
        // Only consider lane-prefixed keys (AFC convention). Ignore any
        // unrelated data that may live in the lane_data namespace.
        if (key.rfind("lane", 0) != 0) continue;
        auto parsed = from_lane_data_record(it.value());
        if (!parsed) continue;
        result[parsed->first] = parsed->second;
    }
    return result;
}

void FilamentSlotOverrideStore::save_async(int /*slot_index*/,
                                           const FilamentSlotOverride& /*override*/,
                                           SaveCallback cb) {
    if (cb) {
        cb(false, "not implemented");
    }
}

void FilamentSlotOverrideStore::clear_async(int /*slot_index*/, SaveCallback cb) {
    if (cb) {
        cb(false, "not implemented");
    }
}

}  // namespace helix::ams
