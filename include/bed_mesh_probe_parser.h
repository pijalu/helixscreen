// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <regex>
#include <string>

namespace helix {

/**
 * @brief Result of parsing a bed mesh probe line
 *
 * current: 1-based probe index (from "Probing point X/Y" or fallback count)
 * total:   total expected probes (0 if unknown, e.g. fallback "probe at" lines
 *          without a known grid size)
 */
struct ProbeProgress {
    int current;
    int total; ///< 0 = unknown
};

/**
 * @brief Parse a G-code response line for bed mesh probe progress
 *
 * Handles two formats:
 *  1. "Probing point 5/25", "Probe point 5 of 25", "Probing mesh point 5/25"
 *  2. "probe at X,Y is z=Z" (fallback — caller must maintain a running count)
 *
 * For format (1), returns {current, total}.
 * For format (2), returns std::nullopt — use is_probe_result_line() to detect
 * these and maintain your own counter.
 *
 * @param line G-code response line
 * @return Parsed {current, total} or std::nullopt
 */
inline std::optional<ProbeProgress> parse_probe_progress(const std::string& line) {
    // Static regex — handles "Probing point 5/25", "Probe point 5 of 25",
    // "Probing mesh point 5/25"
    static const std::regex probe_regex(
        R"(Prob(?:ing (?:mesh )?point|e point) (\d+)[/\s]+(?:of\s+)?(\d+))");

    std::smatch match;
    if (std::regex_search(line, match, probe_regex) && match.size() == 3) {
        try {
            return ProbeProgress{std::stoi(match[1].str()), std::stoi(match[2].str())};
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/**
 * @brief Check if a line is a "probe at X,Y is z=Z" result line
 *
 * These lines appear on firmware that doesn't emit "Probing point X/Y" progress
 * markers. Callers should maintain their own running count when this returns true.
 */
inline bool is_probe_result_line(const std::string& line) {
    return line.find("probe at ") != std::string::npos && line.find(" is z=") != std::string::npos;
}

} // namespace helix
