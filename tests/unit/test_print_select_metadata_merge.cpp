// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_metadata_merge.cpp
 * @brief Tests for the print-select panel's file-list merge carry-forward decision.
 *
 * Regression: after Moonraker transiently failed to extract metadata for a newly
 * uploaded file (JSON-RPC -32601 "Metadata not available"), the file's card in the
 * print-select grid showed a placeholder thumbnail even after Moonraker recovered.
 *
 * Root cause: fetch_metadata_range() marks PrintFileData::metadata_fetched=true
 * optimistically before dispatching the fetch. When the fetch and its metascan
 * fallback both returned empty metadata, thumbnail_path stayed empty but
 * metadata_fetched stayed true. The on_files_ready merge loop then carried the
 * stale entry forward on every polling refresh (size unchanged), and nothing
 * else ever reset metadata_fetched. The panel never retried even on revisit.
 *
 * Fix: on panel activation, on_activate() sets retry_missing_thumbnails_on_refresh_
 * so the merge drops cached entries with empty thumbnail_path, giving each one a
 * one-shot retry this visit. should_carry_forward_print_file_metadata() is the
 * pure decision function.
 */

#include "print_file_data.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Build a cached entry as it would look after a successful fetch.
PrintFileData make_cached_entry(const std::string& filename, size_t size,
                                const std::string& thumbnail_path) {
    PrintFileData f;
    f.filename = filename;
    f.file_size_bytes = size;
    f.thumbnail_path = thumbnail_path;
    f.metadata_fetched = true;
    return f;
}

} // namespace

// ============================================================================
// Basic carry-forward
// ============================================================================

TEST_CASE("Unchanged file with thumbnail carries forward", "[print_select][merge]") {
    auto old = make_cached_entry("print.gcode", 1024, "A:helix_thumbs/abc.bin");
    REQUIRE(should_carry_forward_print_file_metadata(old, 1024, false) == true);
    // Retry flag does not affect entries that already have a thumbnail — the
    // retry logic is specifically scoped to the empty-thumbnail recovery case.
    REQUIRE(should_carry_forward_print_file_metadata(old, 1024, true) == true);
}

TEST_CASE("Size change (re-slice) drops cached metadata", "[print_select][merge]") {
    auto old = make_cached_entry("print.gcode", 1024, "A:helix_thumbs/abc.bin");
    REQUIRE(should_carry_forward_print_file_metadata(old, 2048, false) == false);
    REQUIRE(should_carry_forward_print_file_metadata(old, 2048, true) == false);
}

TEST_CASE("Entry without metadata_fetched is never carried forward", "[print_select][merge]") {
    // The merge loop only consults this function for entries that were placed in
    // old_state because they claimed to have metadata. But the function must still
    // reject a bogus caller that passes an unfetched entry.
    PrintFileData old;
    old.filename = "print.gcode";
    old.file_size_bytes = 1024;
    old.metadata_fetched = false;
    old.thumbnail_path = "A:helix_thumbs/abc.bin";
    REQUIRE(should_carry_forward_print_file_metadata(old, 1024, false) == false);
    REQUIRE(should_carry_forward_print_file_metadata(old, 1024, true) == false);
}

// ============================================================================
// Retry-missing-thumbnail path (the actual bug)
// ============================================================================

TEST_CASE("Empty thumbnail carries forward on polling refresh", "[print_select][merge][retry]") {
    // Polling refresh (not panel activation): retry flag is false. The stale entry
    // must carry forward so we don't spam metadata re-fetches every 5 seconds for
    // files that legitimately have no thumbnail.
    auto old = make_cached_entry("print.gcode", 1024, /*thumbnail_path=*/"");
    REQUIRE(should_carry_forward_print_file_metadata(old, 1024, false) == true);
}

TEST_CASE("Empty thumbnail dropped on panel activation for one-shot retry",
          "[print_select][merge][retry]") {
    // Panel activation: retry flag is true. Entries with empty thumbnail_path get
    // dropped so they re-fetch this visit. This is the self-heal path for files
    // whose upload-time metadata extraction failed transiently in Moonraker.
    auto old = make_cached_entry("print.gcode", 1024, /*thumbnail_path=*/"");
    REQUIRE(should_carry_forward_print_file_metadata(old, 1024, true) == false);
}

TEST_CASE("Size change wins over retry flag for empty-thumbnail entry",
          "[print_select][merge][retry]") {
    // Size changed AND empty thumbnail AND retry flag set — all three rules agree
    // the entry should be dropped. This just checks the function doesn't short-
    // circuit in a way that hides the size-change case.
    auto old = make_cached_entry("print.gcode", 1024, /*thumbnail_path=*/"");
    REQUIRE(should_carry_forward_print_file_metadata(old, 2048, true) == false);
}
