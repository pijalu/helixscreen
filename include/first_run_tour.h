// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix::tour {

/// Current tour version. Bump when tour content materially changes.
constexpr int kTourVersion = 1;

class FirstRunTour {
  public:
    // Singleton used by runtime methods (Task 5); gate helpers below are static.
    static FirstRunTour& instance();

    /// Gate check: true iff tour should auto-start on home activate.
    /// Checks: tour not already completed at current version, wizards complete.
    static bool should_auto_start();

    /// Writes tour.completed=true and tour.last_seen_version=kTourVersion.
    /// Persists via Config::save(). Called on both skip and finish.
    static void mark_completed();

    // Runtime API (implemented in later tasks):
    void maybe_start(); // Auto-trigger entry point
    void start();       // Replay entry point (bypasses gate)
    void advance();     // Next step
    void skip();        // Skip button
    bool is_running() const { return running_; }

  private:
    FirstRunTour() = default;
    bool running_ = false;
};

} // namespace helix::tour
