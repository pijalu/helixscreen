// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_consumption_tracker.h"

#include "ams_state.h"
#include "app_globals.h"
#include "filament_database.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <cmath>
#include <spdlog/spdlog.h>

namespace helix {

FilamentConsumptionTracker& FilamentConsumptionTracker::instance() {
    static FilamentConsumptionTracker inst;
    return inst;
}

void FilamentConsumptionTracker::start() {
    PrinterState& printer = get_printer_state();

    print_state_obs_ = helix::ui::observe_int_sync<FilamentConsumptionTracker>(
        printer.get_print_state_enum_subject(), this,
        [](FilamentConsumptionTracker* self, int state) {
            self->on_print_state_changed(state);
        });

    // filament_used observer wired up here for Tasks 5-8; still a stub this task.
    filament_used_obs_ = helix::ui::observe_int_sync<FilamentConsumptionTracker>(
        printer.get_print_filament_used_subject(), this,
        [](FilamentConsumptionTracker* self, int mm) {
            self->on_filament_used_changed(mm);
        });
}

void FilamentConsumptionTracker::stop() {
    print_state_obs_.reset();
    filament_used_obs_.reset();
    active_ = false;
}

void FilamentConsumptionTracker::on_print_state_changed(int job_state) {
    auto state = static_cast<PrintJobState>(job_state);
    switch (state) {
        case PrintJobState::PRINTING:
            if (!active_) {
                snapshot();
            }
            break;
        case PrintJobState::COMPLETE:
        case PrintJobState::CANCELLED:
        case PrintJobState::ERROR:
            if (active_) {
                persist();
                spdlog::info(
                    "[FilamentTracker] Print ended in state {}; persisted final weight",
                    job_state);
            }
            active_ = false;
            break;
        case PrintJobState::PAUSED:
            if (active_) {
                persist(); // crash-safety snapshot
            }
            break;
        default:
            break;
    }
}

void FilamentConsumptionTracker::on_filament_used_changed(int filament_mm) {
    if (!active_) {
        return;
    }

    auto info_opt = AmsState::instance().get_external_spool_info();
    if (!info_opt.has_value()) {
        return;
    }
    SlotInfo info = *info_opt;

    // External-write detection: someone other than us updated remaining_weight_g.
    // Treat as authoritative and rebase our snapshot from it.
    if (std::abs(info.remaining_weight_g - last_written_weight_g_) > 0.5f) {
        spdlog::info(
            "[FilamentTracker] External write detected (was {} g, now {} g); re-snapshotting",
            last_written_weight_g_, info.remaining_weight_g);
        snapshot_mm_ = static_cast<float>(filament_mm);
        snapshot_weight_g_ = info.remaining_weight_g;
        last_written_weight_g_ = info.remaining_weight_g;
        return;
    }

    float current_mm = static_cast<float>(filament_mm);
    float consumed_mm = current_mm - snapshot_mm_;
    if (consumed_mm < 0.0f) {
        // filament_used was reset under us (e.g. new print). Rebase.
        snapshot_mm_ = current_mm;
        snapshot_weight_g_ = info.remaining_weight_g;
        last_written_weight_g_ = info.remaining_weight_g;
        return;
    }

    float consumed_g = filament::length_to_weight_g(consumed_mm, density_g_cm3_, diameter_mm_);
    float new_remaining_g = snapshot_weight_g_ - consumed_g;
    if (new_remaining_g < 0.0f) {
        new_remaining_g = 0.0f;
    }

    // Avoid noise writes for sub-gram changes that the UI can't show anyway.
    if (std::abs(new_remaining_g - info.remaining_weight_g) < 0.05f) {
        return;
    }

    info.remaining_weight_g = new_remaining_g;
    AmsState::instance().set_external_spool_info_in_memory(info);

    // Throttled disk persist (crash-safety).
    if (lv_tick_elaps(last_persist_tick_ms_) >= persist_interval_ms()) {
        AmsState::instance().set_external_spool_info(info);
        last_persist_tick_ms_ = lv_tick_get();
    }

    last_written_weight_g_ = new_remaining_g;
}

void FilamentConsumptionTracker::snapshot() {
    active_ = false;
    auto info_opt = AmsState::instance().get_external_spool_info();
    if (!info_opt.has_value()) {
        spdlog::debug(
            "[FilamentTracker] No external spool assigned; consumption tracking disabled");
        return;
    }
    const auto& info = *info_opt;
    if (info.remaining_weight_g <= 0.0f) {
        spdlog::debug(
            "[FilamentTracker] External spool has no known remaining weight; skipping");
        return;
    }

    auto material = filament::find_material(info.material);
    if (!material.has_value() || material->density_g_cm3 <= 0.0f) {
        spdlog::warn(
            "[FilamentTracker] Cannot resolve density for material '{}'; "
            "consumption tracking disabled for this print. Set a known material on the "
            "external spool to enable tracking.",
            info.material);
        return;
    }

    density_g_cm3_ = material->density_g_cm3;
    diameter_mm_ = 1.75f;
    snapshot_mm_ = static_cast<float>(
        lv_subject_get_int(get_printer_state().get_print_filament_used_subject()));
    snapshot_weight_g_ = info.remaining_weight_g;
    last_written_weight_g_ = info.remaining_weight_g;
    last_persist_tick_ms_ = lv_tick_get();
    active_ = true;
    spdlog::info(
        "[FilamentTracker] Snapshot: material={}, density={} g/cm3, weight={} g, "
        "filament_used_mm={}",
        info.material, density_g_cm3_, snapshot_weight_g_, snapshot_mm_);
}

void FilamentConsumptionTracker::persist() {
    auto info_opt = AmsState::instance().get_external_spool_info();
    if (!info_opt.has_value()) {
        return;
    }
    // Full write updates settings.json via SettingsManager and re-fires the subject.
    AmsState::instance().set_external_spool_info(*info_opt);
    last_written_weight_g_ = info_opt->remaining_weight_g;
    last_persist_tick_ms_ = lv_tick_get();
}

} // namespace helix
