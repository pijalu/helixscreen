// SPDX-License-Identifier: GPL-3.0-or-later

#include "preprint_predictor.h"

#include "config.h"
#include "printer_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace helix {

void PreprintPredictor::load_entries(const std::vector<PreprintEntry>& entries,
                                     StartCondition condition) {
    entries_.clear();

    for (const auto& e : entries) {
        // temp_bucket 0 = legacy/unknown, always include
        if (e.temp_bucket == 0) {
            entries_.push_back(e);
        } else if (condition == StartCondition::COLD && e.temp_bucket == 1) {
            entries_.push_back(e);
        } else if (condition == StartCondition::WARM && e.temp_bucket == 2) {
            entries_.push_back(e);
        }
    }

    // FIFO trim to MAX_ENTRIES (keep newest)
    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}

void PreprintPredictor::load_entries(const std::vector<PreprintEntry>& entries, int temp_bucket) {
    entries_.clear();

    if (temp_bucket == 0) {
        // No filter — load all entries
        entries_ = entries;
    } else if (temp_bucket == 1) {
        // Cold start bucket
        load_entries(entries, StartCondition::COLD);
        return;
    } else if (temp_bucket == 2) {
        // Warm start bucket
        load_entries(entries, StartCondition::WARM);
        return;
    } else {
        // Legacy temp_bucket values (e.g. 200, 250): filter by matching bucket or legacy 0
        for (const auto& e : entries) {
            if (e.temp_bucket == temp_bucket || e.temp_bucket == 0) {
                entries_.push_back(e);
            }
        }
    }

    // FIFO trim to MAX_ENTRIES (keep newest)
    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}

void PreprintPredictor::add_entry(const PreprintEntry& entry) {
    entries_.push_back(entry);

    // FIFO trim
    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}

std::vector<PreprintEntry> PreprintPredictor::get_entries() const {
    return entries_;
}

bool PreprintPredictor::has_predictions() const {
    return true;
}

std::vector<double> PreprintPredictor::compute_weights() const {
    if (entries_.empty())
        return {};

    std::vector<double> weights(entries_.size());
    // ln(10)/10 ≈ 0.23 — oldest of 10 entries gets ~10% relative weight
    constexpr double lambda = 0.23;
    double total = 0.0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        weights[i] = std::exp(lambda * static_cast<double>(i));
        total += weights[i];
    }
    for (auto& w : weights) {
        w /= total;
    }
    return weights;
}

std::map<int, int> PreprintPredictor::default_phase_durations() {
    return {
        {static_cast<int>(PrintStartPhase::HOMING), 20},
        {static_cast<int>(PrintStartPhase::BED_MESH), 90},
        {static_cast<int>(PrintStartPhase::QGL), 60},
        {static_cast<int>(PrintStartPhase::Z_TILT), 45},
        {static_cast<int>(PrintStartPhase::CLEANING), 15},
        {static_cast<int>(PrintStartPhase::PURGING), 10},
    };
}

std::map<int, int> PreprintPredictor::predicted_phases() const {
    if (entries_.empty()) {
        return default_phase_durations();
    }

    // Collect all phases that appear in any entry
    std::set<int> all_phases;
    for (const auto& entry : entries_) {
        for (const auto& [phase, _] : entry.phase_durations) {
            all_phases.insert(phase);
        }
    }

    auto weights = compute_weights();

    std::map<int, int> result;
    for (int phase : all_phases) {
        // Step 1: Collect durations for entries that have this phase
        std::vector<std::pair<size_t, double>> phase_entries; // (index, duration)
        for (size_t i = 0; i < entries_.size(); ++i) {
            auto it = entries_[i].phase_durations.find(phase);
            if (it != entries_[i].phase_durations.end()) {
                phase_entries.emplace_back(i, static_cast<double>(it->second));
            }
        }

        if (phase_entries.empty())
            continue;

        // Step 2: MAD anomaly rejection
        // Compute median
        std::vector<double> durations;
        durations.reserve(phase_entries.size());
        for (const auto& [_, dur] : phase_entries) {
            durations.push_back(dur);
        }
        std::sort(durations.begin(), durations.end());

        double median;
        size_t n = durations.size();
        if (n % 2 == 0) {
            median = (durations[n / 2 - 1] + durations[n / 2]) / 2.0;
        } else {
            median = durations[n / 2];
        }

        // Compute MAD = median of |each - median|
        std::vector<double> abs_devs;
        abs_devs.reserve(n);
        for (double d : durations) {
            abs_devs.push_back(std::abs(d - median));
        }
        std::sort(abs_devs.begin(), abs_devs.end());

        double mad;
        if (n % 2 == 0) {
            mad = (abs_devs[n / 2 - 1] + abs_devs[n / 2]) / 2.0;
        } else {
            mad = abs_devs[n / 2];
        }

        // Step 3: Weighted average of non-anomalous entries
        double total_weight = 0.0;
        double weighted_sum = 0.0;
        for (const auto& [idx, dur] : phase_entries) {
            // Reject if MAD > 0 and deviation exceeds 3*MAD
            if (mad > 0.0 && std::abs(dur - median) > 3.0 * mad) {
                continue;
            }
            total_weight += weights[idx];
            weighted_sum += weights[idx] * dur;
        }

        if (total_weight > 0.0) {
            result[phase] = static_cast<int>(std::round(weighted_sum / total_weight));
        }
    }

    return result;
}

int PreprintPredictor::predicted_total() const {
    auto phases = predicted_phases();
    int total = 0;
    for (const auto& [_, duration] : phases) {
        total += duration;
    }
    return total;
}

int PreprintPredictor::remaining_seconds(const std::set<int>& completed_phases, int current_phase,
                                         int elapsed_in_current_phase_seconds) const {
    auto phases = predicted_phases();
    int remaining = 0;

    for (const auto& [phase, predicted_duration] : phases) {
        if (completed_phases.count(phase)) {
            // Already done, actual time was spent (not predicted)
            continue;
        }

        if (phase == current_phase && current_phase != 0) {
            // Currently in this phase - subtract elapsed
            remaining += std::max(0, predicted_duration - elapsed_in_current_phase_seconds);
        } else if (!completed_phases.count(phase) && phase != current_phase) {
            // Future phase
            remaining += predicted_duration;
        }
    }

    return remaining;
}

std::vector<PreprintEntry> PreprintPredictor::load_entries_from_config() {
    auto* cfg = Config::get_instance();
    if (!cfg) {
        return {};
    }

    try {
        auto entries_json =
            cfg->get<nlohmann::json>("/print_start_history/entries", nlohmann::json::array());
        if (!entries_json.is_array() || entries_json.empty()) {
            return {};
        }

        std::vector<PreprintEntry> entries;
        for (const auto& ej : entries_json) {
            PreprintEntry entry;
            entry.total_seconds = ej.value("total", 0);
            entry.timestamp = ej.value("timestamp", static_cast<int64_t>(0));
            entry.temp_bucket = ej.value("temp_bucket", 0);
            if (ej.contains("phases") && ej["phases"].is_object()) {
                for (auto& [key, val] : ej["phases"].items()) {
                    entry.phase_durations[std::stoi(key)] = val.get<int>();
                }
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    } catch (...) {
        return {};
    }
}

int PreprintPredictor::predicted_total_from_config() {
    // Cache result for 60s to avoid re-parsing config on every file in a list
    static std::atomic<int> cached_value{-1};
    static std::atomic<int64_t> cached_at{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    if (cached_value.load() >= 0 && (now_sec - cached_at.load()) < 60) {
        return cached_value.load();
    }

    auto entries = load_entries_from_config();
    int result = 0;
    if (!entries.empty()) {
        PreprintPredictor predictor;
        predictor.load_entries(entries);
        result = predictor.predicted_total();
    }

    cached_value.store(result);
    cached_at.store(now_sec);
    return result;
}

} // namespace helix
