// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_printer.h"

namespace helix {

const char* label_preset_name(LabelPreset preset) {
    switch (preset) {
    case LabelPreset::STANDARD: return "Standard";
    case LabelPreset::COMPACT:  return "Compact";
    case LabelPreset::MINIMAL:  return "QR Only";
    }
    return "Standard";
}

const char* label_preset_options() {
    return "Standard\nCompact\nQR Only";
}

} // namespace helix
