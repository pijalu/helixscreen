// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/pending_startup_warnings.h"

#include <utility>

namespace helix {

PendingStartupWarnings& PendingStartupWarnings::instance() {
    static PendingStartupWarnings s_instance;
    return s_instance;
}

void PendingStartupWarnings::enqueue(Severity severity, std::string message) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.emplace_back(severity, std::move(message));
}

void PendingStartupWarnings::drain(
    const std::function<void(Severity, const std::string&)>& on_warning) {
    // Swap under the lock, then invoke callbacks without holding it. This
    // avoids any risk of the callback re-entering enqueue() (e.g. from a
    // toast implementation that logs something), and keeps the lock hold
    // time minimal.
    std::vector<std::pair<Severity, std::string>> local;
    {
        std::lock_guard<std::mutex> lock(mu_);
        local.swap(pending_);
    }
    for (const auto& entry : local) {
        on_warning(entry.first, entry.second);
    }
}

void PendingStartupWarnings::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.clear();
}

} // namespace helix
