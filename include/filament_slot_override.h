// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include "hv/json.hpp"

namespace helix::ams {

struct FilamentSlotOverride {
    // User metadata
    std::string brand;
    std::string spool_name;
    int spoolman_id = 0;
    int spoolman_vendor_id = 0;
    float remaining_weight_g = -1.0f;
    float total_weight_g = -1.0f;
    // Hardware-truth fields, override-wins
    uint32_t color_rgb = 0;
    std::string color_name;
    std::string material;
    // Conflict avoidance for third-party writers.
    // ISO-8601 UTC on the wire. Second precision only — sub-second fractions
    // are truncated on format/parse.
    std::chrono::system_clock::time_point updated_at{};
};

nlohmann::json to_json(const FilamentSlotOverride& o);
FilamentSlotOverride from_json(const nlohmann::json& j);

}  // namespace helix::ams
