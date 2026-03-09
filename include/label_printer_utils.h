// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "label_printer.h"
#include "mdns_discovery.h"
#include "spoolman_types.h"

#include <algorithm>
#include <string>

namespace helix {

/// Print a spool label using the configured printer backend.
/// Handles backend selection, USB device scanning with fallback, and error reporting.
/// Callback fires on UI thread.
void print_spool_label(const SpoolInfo& spool, PrintCallback callback);

/**
 * Score how likely a discovered printer is to be a label printer.
 * Higher scores = more likely. 0 = definitely not a label printer.
 */
inline int label_printer_score(const DiscoveredPrinter& printer) {
    auto lower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(), ::tolower);
        return out;
    };
    std::string name = lower(printer.name);
    std::string host = lower(printer.hostname);

    // Strong signals — these are definitely label printers
    if (name.find("ql-") != std::string::npos || name.find("ql ") != std::string::npos)
        return 100;
    if (name.find("td-") != std::string::npos || name.find("td ") != std::string::npos)
        return 100;
    if (name.find("labelwriter") != std::string::npos || name.find("dymo") != std::string::npos)
        return 90;
    if (name.find("zebra") != std::string::npos)
        return 80;
    if (name.find("rollo") != std::string::npos || name.find("munbyn") != std::string::npos)
        return 80;
    if (name.find("label") != std::string::npos)
        return 70;

    // Moderate signals from hostname (BRW prefix = Brother wireless)
    if (host.find("brw") != std::string::npos)
        return 50;

    // Negative signals — definitely NOT label printers
    if (name.find("laserjet") != std::string::npos || name.find("officejet") != std::string::npos)
        return 0;
    if (name.find("inkjet") != std::string::npos || name.find("pixma") != std::string::npos)
        return 0;
    if (name.find("ecotank") != std::string::npos || name.find("envy") != std::string::npos)
        return 0;
    if (name.find("epson") != std::string::npos && name.find("et-") != std::string::npos)
        return 0;

    // Unknown — could be anything, low score but still show
    return 10;
}

} // namespace helix
