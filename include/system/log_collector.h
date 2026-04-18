// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

/// Unified log-tail collection for crash reporter and debug bundle.
///
/// Three sources, tried in order:
///   1. File-based logs at XDG / /var/log / /tmp paths (desktop + systems with file sink)
///   2. Syslog at /var/log/messages or /var/log/syslog (embedded: AD5M, AD5X, K1)
///   3. systemd journal via `journalctl SYSLOG_IDENTIFIER=helix-screen` (pi, pi32)
///
/// HelixScreen's logging pipeline writes to exactly one of these per device, so
/// the cascade converges quickly. Callers that know which source they want can
/// invoke one of the individual tail_* functions.
namespace helix::logs {

/// Default file-log search paths in resolution order, matching
/// logging_init.cpp's `resolve_log_file_path()`. Includes env-dependent entries
/// (XDG_DATA_HOME, HOME) resolved at call time.
std::vector<std::string> default_file_paths();

/// Read the last `num_lines` lines from the first readable path in `paths`.
/// Returns empty string if none exist, are empty, or are unreadable.
std::string tail_file(const std::vector<std::string>& paths, int num_lines);

/// Read the last `num_lines` helix-screen entries from /var/log/messages or
/// /var/log/syslog. Filters for "helix-screen", "helix-watchdog", and
/// "helix-splash" identifiers. Returns empty if neither file exists.
std::string tail_syslog(int num_lines);

/// Testable overload: read the last `num_lines` helix-screen entries from the
/// supplied `paths` list instead of /var/log. Same filter as `tail_syslog`.
/// Production code should call `tail_syslog()` — this exists so tests can
/// feed fake syslog files without touching /var/log.
std::string tail_syslog_from(const std::vector<std::string>& paths, int num_lines);

/// Run `journalctl SYSLOG_IDENTIFIER=helix-screen -n NUM --no-pager -b 0` and
/// return its output. Returns empty if journalctl isn't installed, the journal
/// is empty for this identifier, or the call fails. The `-b 0` flag restricts
/// to the current boot so we don't drag in old noise.
std::string tail_journal(int num_lines);

/// Cascade: file logs, then syslog, then journal. First source with content
/// wins. Pass an empty `paths` vector to use `default_file_paths()`.
std::string tail_best(int num_lines, const std::vector<std::string>& paths = {});

} // namespace helix::logs
