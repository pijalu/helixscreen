// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <vector>

/// Whether the printer bed was cold or warm at print start
enum class StartCondition { COLD, WARM };

namespace helix {

/**
 * @brief A single recorded pre-print timing entry
 *
 * Captures per-phase durations from one print start sequence.
 * Phase keys are PrintStartPhase enum int values.
 */
struct PreprintEntry {
    int total_seconds;                  ///< Total pre-print duration
    int64_t timestamp;                  ///< Unix timestamp when entry was recorded
    std::map<int, int> phase_durations; ///< phase_enum -> seconds
    int temp_bucket{0}; ///< Nozzle target temp rounded to nearest 25°C (0 = unknown)
};

/**
 * @brief Predicts pre-print duration from historical per-phase timing
 *
 * Tracks last 3 print start timing entries and computes weighted averages
 * to predict future pre-print remaining time. Weighting favors recent entries:
 * - 1 entry: 100%
 * - 2 entries: 60% newest, 40% older
 * - 3 entries: 50% newest, 30% middle, 20% oldest
 *
 * Pure logic class with no LVGL or Config dependencies.
 */
class PreprintPredictor {
  public:
    /// Maximum entries to keep (FIFO)
    static constexpr int MAX_ENTRIES = 10;

    /**
     * @brief Load entries from storage, optionally filtering by start condition
     *
     * Trims to MAX_ENTRIES (FIFO, keeping newest).
     */
    void load_entries(const std::vector<PreprintEntry>& entries,
                      StartCondition condition = StartCondition::COLD);

    /**
     * @brief Load entries with legacy temp_bucket filtering (backward compat)
     *
     * bucket 0 = no filter, bucket 1 = cold, bucket 2 = warm,
     * other values use simplified mapping.
     */
    void load_entries(const std::vector<PreprintEntry>& entries, int temp_bucket);

    /**
     * @brief Add a single entry, enforcing FIFO trim to MAX_ENTRIES
     */
    void add_entry(const PreprintEntry& entry);

    /**
     * @brief Get current entries (for persistence)
     */
    [[nodiscard]] std::vector<PreprintEntry> get_entries() const;

    /**
     * @brief Sum of all weighted phase averages
     */
    [[nodiscard]] int predicted_total() const;

    /**
     * @brief Per-phase predicted durations (phase_enum -> seconds)
     */
    [[nodiscard]] std::map<int, int> predicted_phases() const;

    /**
     * @brief Real-time remaining seconds estimate
     *
     * @param completed_phases Set of phase enum ints already completed
     * @param current_phase Current phase enum int (0=IDLE, no contribution)
     * @param elapsed_in_current_phase_seconds Seconds spent in current phase
     * @return Estimated remaining seconds, 0 if no predictions
     */
    [[nodiscard]] int remaining_seconds(const std::set<int>& completed_phases, int current_phase,
                                        int elapsed_in_current_phase_seconds) const;

    /**
     * @brief Whether any predictions can be made
     */
    [[nodiscard]] bool has_predictions() const;

    /**
     * @brief Default phase durations for when no history is available
     *
     * Returns reasonable defaults for non-heating phases based on
     * typical printer behavior.
     */
    static std::map<int, int> default_phase_durations();

    /**
     * @brief Load entries from Config's print_start_history
     *
     * Single source of truth for Config→PreprintEntry deserialization.
     * Used by both PrintStartCollector and predicted_total_from_config().
     *
     * @return Parsed entries (may be empty)
     */
    [[nodiscard]] static std::vector<PreprintEntry> load_entries_from_config();

    /**
     * @brief Load history from Config and return predicted total seconds
     *
     * Convenience method for UI code that needs the prediction without
     * access to the PrintStartCollector's predictor instance.
     *
     * @return Predicted pre-print seconds, or 0 if no history
     */
    [[nodiscard]] static int predicted_total_from_config();

  private:
    /// Exponential time-decay weights (oldest=low, newest=high), normalized to sum=1
    [[nodiscard]] std::vector<double> compute_weights() const;

    std::vector<PreprintEntry> entries_;
};

} // namespace helix
