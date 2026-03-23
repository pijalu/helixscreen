// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel_widget_registry.h"
#include <spdlog/spdlog.h>

namespace helix {
void register_temp_graph_widget() {
    spdlog::debug("[TempGraphWidget] Registered");
}
} // namespace helix
