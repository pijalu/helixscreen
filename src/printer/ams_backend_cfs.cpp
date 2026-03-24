// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_cfs.h"

#include "hv/json.hpp"

#include <spdlog/spdlog.h>

#include <fstream>

namespace helix::printer {

const CfsMaterialDb& CfsMaterialDb::instance()
{
    static CfsMaterialDb db;
    return db;
}

CfsMaterialDb::CfsMaterialDb()
{
    load_database();
}

void CfsMaterialDb::load_database()
{
    for (const auto& path : {"assets/cfs_materials.json",
                              "../assets/cfs_materials.json",
                              "/opt/helixscreen/assets/cfs_materials.json"}) {
        std::ifstream f(path);
        if (!f.is_open())
            continue;

        try {
            auto j = nlohmann::json::parse(f);
            for (auto& [id, entry] : j.items()) {
                CfsMaterialInfo info;
                info.id            = id;
                info.brand         = entry.value("brand", "");
                info.name          = entry.value("name", "");
                info.material_type = entry.value("type", "");
                info.min_temp      = entry.value("min_temp", 0);
                info.max_temp      = entry.value("max_temp", 0);
                materials_[id]     = std::move(info);
            }
            spdlog::info("[AMS CFS] Loaded {} materials from {}", materials_.size(), path);
            return;
        } catch (const std::exception& e) {
            spdlog::warn("[AMS CFS] Failed to parse {}: {}", path, e.what());
        }
    }
    spdlog::warn("[AMS CFS] Material database not found");
}

const CfsMaterialInfo* CfsMaterialDb::lookup(const std::string& id) const
{
    auto it = materials_.find(id);
    return it != materials_.end() ? &it->second : nullptr;
}

std::string CfsMaterialDb::strip_code(const std::string& code)
{
    if (code == "-1" || code == "None" || code.empty())
        return "";
    if (code.size() == 6 && code[0] == '1')
        return code.substr(1);
    return code;
}

uint32_t CfsMaterialDb::parse_color(const std::string& color_str)
{
    if (color_str == "-1" || color_str == "None" || color_str.empty())
        return DEFAULT_COLOR;
    std::string hex = color_str;
    if (hex.size() == 7 && hex[0] == '0')
        hex = hex.substr(1);
    try {
        return static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
        return DEFAULT_COLOR;
    }
}

std::string CfsMaterialDb::slot_to_tnn(int global_index)
{
    if (global_index < 0 || global_index > 15)
        return "";
    int unit   = global_index / 4 + 1;
    int slot   = global_index % 4;
    char letter = 'A' + static_cast<char>(slot);
    return "T" + std::to_string(unit) + letter;
}

int CfsMaterialDb::tnn_to_slot(const std::string& tnn)
{
    if (tnn.size() != 3 || tnn[0] != 'T')
        return -1;
    int unit = tnn[1] - '0';
    int slot = tnn[2] - 'A';
    if (unit < 1 || unit > 4 || slot < 0 || slot > 3)
        return -1;
    return (unit - 1) * 4 + slot;
}

} // namespace helix::printer
