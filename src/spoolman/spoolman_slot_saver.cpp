// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_slot_saver.h"

#include "moonraker_api.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

#include "hv/json.hpp"

namespace helix {

SpoolmanSlotSaver::SpoolmanSlotSaver(MoonrakerAPI* api) : api_(api) {}

ChangeSet SpoolmanSlotSaver::detect_changes(const SlotInfo& original, const SlotInfo& edited) {
    ChangeSet changes;

    // Filament-level: brand, material, color_rgb
    if (original.brand != edited.brand || original.material != edited.material ||
        original.color_rgb != edited.color_rgb) {
        changes.filament_level = true;
    }

    // Spool-level: remaining_weight_g (float comparison with threshold)
    if (std::abs(original.remaining_weight_g - edited.remaining_weight_g) > WEIGHT_THRESHOLD) {
        changes.spool_level = true;
    }

    return changes;
}

std::string SpoolmanSlotSaver::color_to_hex(uint32_t rgb) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06X", rgb & 0xFFFFFF);
    return std::string(buf);
}

void SpoolmanSlotSaver::save(const SlotInfo& original, const SlotInfo& edited,
                             CompletionCallback on_complete) {
    // No-op for non-Spoolman slots
    if (!edited.spoolman_id) {
        spdlog::debug("[SpoolmanSlotSaver] No spoolman_id, skipping save");
        if (on_complete)
            on_complete(true);
        return;
    }

    auto changes = detect_changes(original, edited);

    // No changes detected
    if (!changes.any()) {
        spdlog::debug("[SpoolmanSlotSaver] No changes detected for spool {}", edited.spoolman_id);
        if (on_complete)
            on_complete(true);
        return;
    }

    const int spool_id = edited.spoolman_id;

    // Only spool-level (weight) change
    if (!changes.filament_level && changes.spool_level) {
        spdlog::info("[SpoolmanSlotSaver] Updating weight for spool {} to {:.1f}g", spool_id,
                     edited.remaining_weight_g);
        update_weight(spool_id, edited.remaining_weight_g, on_complete);
        return;
    }

    // Filament-level change (possibly also weight)
    if (changes.filament_level) {
        const int filament_id = edited.spoolman_filament_id;
        spdlog::info("[SpoolmanSlotSaver] Filament-level change for spool {} "
                     "(filament_id={}, brand={}, material={}, color={:#08x})",
                     spool_id, filament_id, edited.brand, edited.material, edited.color_rgb);

        if (original.brand != edited.brand && edited.spoolman_vendor_id == 0) {
            spdlog::warn("[SpoolmanSlotSaver] Vendor changed to '{}' but no vendor_id available, "
                         "vendor will not be updated in Spoolman",
                         edited.brand);
        }

        if (!filament_id) {
            spdlog::error("[SpoolmanSlotSaver] No filament_id for spool {}, cannot update",
                          spool_id);
            if (on_complete)
                on_complete(false);
            return;
        }

        if (changes.spool_level) {
            // Chain: update filament first, then update weight
            auto weight = edited.remaining_weight_g;
            update_filament(filament_id, edited,
                            [this, spool_id, weight, on_complete](bool success) {
                                if (!success) {
                                    if (on_complete)
                                        on_complete(false);
                                    return;
                                }
                                update_weight(spool_id, weight, on_complete);
                            });
        } else {
            // Only filament update
            update_filament(filament_id, edited, on_complete);
        }
    }
}

void SpoolmanSlotSaver::update_weight(int spool_id, float weight_g,
                                      CompletionCallback on_complete) {
    api_->spoolman().update_spoolman_spool_weight(
        spool_id, static_cast<double>(weight_g),
        [on_complete]() {
            spdlog::debug("[SpoolmanSlotSaver] Weight update succeeded");
            if (on_complete)
                on_complete(true);
        },
        [on_complete](const MoonrakerError& err) {
            spdlog::error("[SpoolmanSlotSaver] Weight update failed: {}", err.message);
            if (on_complete)
                on_complete(false);
        });
}

void SpoolmanSlotSaver::update_filament(int filament_id, const SlotInfo& edited,
                                        CompletionCallback on_complete) {
    nlohmann::json filament_data;
    filament_data["material"] = edited.material;
    filament_data["color_hex"] = color_to_hex(edited.color_rgb);
    if (edited.spoolman_vendor_id > 0) {
        filament_data["vendor_id"] = edited.spoolman_vendor_id;
    }

    spdlog::info("[SpoolmanSlotSaver] PATCHing filament {} (material={}, color={}, vendor_id={})",
                 filament_id, edited.material, filament_data["color_hex"].get<std::string>(),
                 edited.spoolman_vendor_id);

    api_->spoolman().update_spoolman_filament(
        filament_id, filament_data,
        [on_complete, filament_id]() {
            spdlog::debug("[SpoolmanSlotSaver] Filament {} updated", filament_id);
            if (on_complete)
                on_complete(true);
        },
        [on_complete](const MoonrakerError& err) {
            spdlog::error("[SpoolmanSlotSaver] Filament update failed: {}", err.message);
            if (on_complete)
                on_complete(false);
        });
}

} // namespace helix
