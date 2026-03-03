// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_param_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string_view>

namespace helix {

namespace {
/// Convert a string to lowercase in-place and return a reference.
std::string& to_lower_inplace(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

MacroParamCache& MacroParamCache::instance() {
    static MacroParamCache cache;
    return cache;
}

void MacroParamCache::populate_from_configfile(
    const nlohmann::json& config,
    const std::unordered_set<std::string>& known_macros) {

    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();

    if (!config.is_object()) {
        spdlog::warn("[MacroParamCache] configfile.config is not an object");
        return;
    }

    static constexpr std::string_view PREFIX = "gcode_macro ";

    size_t config_count = 0;
    for (auto it = config.begin(); it != config.end(); ++it) {
        const auto& key = it.key();

        if (key.size() <= PREFIX.size() || key.compare(0, PREFIX.size(), PREFIX) != 0) {
            continue;
        }

        std::string macro_name_lower = key.substr(PREFIX.size());
        to_lower_inplace(macro_name_lower);

        // Extract gcode template for param parsing
        std::string gcode_template;
        if (it.value().is_object() && it.value().contains("gcode") &&
            it.value()["gcode"].is_string()) {
            gcode_template = it.value()["gcode"].get<std::string>();
        }

        CachedMacroInfo info;
        auto params = parse_macro_params(gcode_template);

        // Extract variable_* keys from the config section.
        // These are Klipper gcode_macro variables set via SET_GCODE_VARIABLE.
        static constexpr std::string_view VAR_PREFIX = "variable_";
        if (it.value().is_object()) {
            for (auto vit = it.value().begin(); vit != it.value().end(); ++vit) {
                if (vit.key().size() > VAR_PREFIX.size() &&
                    vit.key().compare(0, VAR_PREFIX.size(), VAR_PREFIX) == 0) {
                    std::string var_name = vit.key().substr(VAR_PREFIX.size());
                    std::transform(var_name.begin(), var_name.end(), var_name.begin(),
                                   [](unsigned char c) { return std::toupper(c); });

                    std::string var_value;
                    if (vit.value().is_string()) {
                        var_value = vit.value().get<std::string>();
                    } else {
                        var_value = vit.value().dump();
                    }

                    params.push_back({var_name, var_value, /*is_variable=*/true});
                }
            }
        }

        if (params.empty()) {
            info.knowledge = MacroParamKnowledge::KNOWN_NO_PARAMS;
        } else {
            info.knowledge = MacroParamKnowledge::KNOWN_PARAMS;
            info.params = std::move(params);
        }

        cache_[macro_name_lower] = std::move(info);
        ++config_count;
    }

    // Mark known macros not found in configfile as UNKNOWN
    size_t unknown_count = 0;
    for (const auto& macro : known_macros) {
        std::string lower = macro;
        to_lower_inplace(lower);
        if (cache_.find(lower) == cache_.end()) {
            cache_[lower] = CachedMacroInfo{MacroParamKnowledge::UNKNOWN, {}};
            ++unknown_count;
        }
    }

    spdlog::info("[MacroParamCache] Populated: {} entries from configfile, {} unknown macros",
                 config_count, unknown_count);
}

CachedMacroInfo MacroParamCache::get(const std::string& macro_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string lower = macro_name;
    to_lower_inplace(lower);

    auto it = cache_.find(lower);
    if (it != cache_.end()) {
        return it->second;
    }
    return CachedMacroInfo{MacroParamKnowledge::UNKNOWN, {}};
}

void MacroParamCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    spdlog::debug("[MacroParamCache] Cache cleared");
}

} // namespace helix
