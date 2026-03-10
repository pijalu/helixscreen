// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <cstdint>
#include <vector>

namespace helix::label {

/// Raster row width for Brother QL-800/810W/820NWB: 90 bytes = 720 pixels
static constexpr int BROTHER_QL_RASTER_ROW_BYTES = 90;

/// Build Brother QL raster protocol bytes from a bitmap.
/// Pure function — no I/O. Output can be sent over TCP, RFCOMM, or any transport.
std::vector<uint8_t> brother_ql_build_raster(const LabelBitmap& bitmap,
                                              const LabelSize& size);

}  // namespace helix::label
