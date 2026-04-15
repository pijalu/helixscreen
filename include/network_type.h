// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix {

/// Network connection types used by HomePanel and NetworkWidget.
/// `Unknown` is the pre-detection sentinel so widgets can distinguish a
/// first activation (no prior state) from re-activations where the last
/// known state should be reused to avoid provisional-state flicker.
enum class NetworkType { Unknown, Wifi, Ethernet, Disconnected };

} // namespace helix
