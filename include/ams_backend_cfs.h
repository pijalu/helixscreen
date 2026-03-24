// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace helix::printer {

/// Material info from CFS RFID database
struct CfsMaterialInfo {
    std::string id;
    std::string brand;
    std::string name;
    std::string material_type;
    int min_temp = 0;
    int max_temp = 0;
};

/// Static material database + CFS utility functions
class CfsMaterialDb {
  public:
    static const CfsMaterialDb& instance();

    /// Lookup material by 5-digit ID (e.g., "01001")
    const CfsMaterialInfo* lookup(const std::string& id) const;

    /// Strip CFS material_type code prefix: "101001" -> "01001", "-1" -> ""
    static std::string strip_code(const std::string& code);

    /// Parse CFS color: "0RRGGBB" -> 0xRRGGBB, sentinels -> 0x808080
    static uint32_t parse_color(const std::string& color_str);

    /// Global slot index -> TNN name: 0 -> "T1A", 4 -> "T2A"
    static std::string slot_to_tnn(int global_index);

    /// TNN name -> global slot index: "T1A" -> 0, "T2A" -> 4, invalid -> -1
    static int tnn_to_slot(const std::string& tnn);

    /// Default color for unknown/sentinel slots
    static constexpr uint32_t DEFAULT_COLOR = 0x808080;

  private:
    CfsMaterialDb();
    void load_database();
    std::unordered_map<std::string, CfsMaterialInfo> materials_;
};

} // namespace helix::printer
