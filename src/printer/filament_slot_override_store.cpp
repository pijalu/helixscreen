// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "i_moonraker_api.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace helix::ams {

static std::string format_iso8601(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

static std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    std::tm tm{};
    std::istringstream is(s);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (is.fail()) return {};
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

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
    return {};
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
