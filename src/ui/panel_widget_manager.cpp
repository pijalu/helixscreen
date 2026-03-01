// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_manager.h"

#include "ui_ams_mini_status.h"
#include "ui_notification.h"

#include "config.h"
#include "grid_layout.h"
#include "observer_factory.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace helix {

PanelWidgetManager& PanelWidgetManager::instance() {
    static PanelWidgetManager instance;
    return instance;
}

void PanelWidgetManager::clear_shared_resources() {
    shared_resources_.clear();
}

void PanelWidgetManager::init_widget_subjects() {
    if (widget_subjects_initialized_) {
        return;
    }

    // Register all widget factories explicitly (avoids SIOF from file-scope statics)
    init_widget_registrations();

    for (const auto& def : get_all_widget_defs()) {
        if (def.init_subjects) {
            spdlog::debug("[PanelWidgetManager] Initializing subjects for widget '{}'", def.id);
            def.init_subjects();
        }
    }

    widget_subjects_initialized_ = true;
    spdlog::debug("[PanelWidgetManager] Widget subjects initialized");
}

void PanelWidgetManager::register_rebuild_callback(const std::string& panel_id,
                                                   RebuildCallback cb) {
    rebuild_callbacks_[panel_id] = std::move(cb);
}

void PanelWidgetManager::unregister_rebuild_callback(const std::string& panel_id) {
    rebuild_callbacks_.erase(panel_id);
}

void PanelWidgetManager::notify_config_changed(const std::string& panel_id) {
    auto it = rebuild_callbacks_.find(panel_id);
    if (it != rebuild_callbacks_.end()) {
        it->second();
    }
}

static PanelWidgetConfig& get_widget_config_impl(const std::string& panel_id) {
    // Per-panel config instances cached by panel ID
    static std::unordered_map<std::string, PanelWidgetConfig> configs;
    auto it = configs.find(panel_id);
    if (it == configs.end()) {
        it = configs.emplace(panel_id, PanelWidgetConfig(panel_id, *Config::get_instance())).first;
    }
    // Always reload to pick up changes from settings overlay
    it->second.load();
    return it->second;
}

std::vector<std::unique_ptr<PanelWidget>>
PanelWidgetManager::populate_widgets(const std::string& panel_id, lv_obj_t* container) {
    if (!container) {
        spdlog::debug("[PanelWidgetManager] populate_widgets: null container for '{}'", panel_id);
        return {};
    }

    if (populating_) {
        spdlog::debug(
            "[PanelWidgetManager] populate_widgets: already in progress for '{}', skipping",
            panel_id);
        return {};
    }
    populating_ = true;

    // Clear existing children (for repopulation)
    lv_obj_clean(container);

    auto& widget_config = get_widget_config_impl(panel_id);

    // Resolved widget slot: holds the widget ID, resolved XML component name,
    // per-widget config, and optionally a pre-created PanelWidget instance.
    struct WidgetSlot {
        std::string widget_id;
        std::string component_name;
        nlohmann::json config;
        std::unique_ptr<PanelWidget> instance; // nullptr for pure-XML widgets
    };

    // Collect enabled + hardware-available widgets
    std::vector<WidgetSlot> enabled_widgets;
    for (const auto& entry : widget_config.entries()) {
        if (!entry.enabled) {
            continue;
        }

        // Check hardware gate — skip widgets whose hardware isn't present.
        // Gates are defined in PanelWidgetDef::hardware_gate_subject and checked
        // here instead of XML bind_flag_if_eq to avoid orphaned dividers.
        const auto* def = find_widget_def(entry.id);
        if (def && def->hardware_gate_subject) {
            lv_subject_t* gate = lv_xml_get_subject(nullptr, def->hardware_gate_subject);
            if (gate && lv_subject_get_int(gate) == 0) {
                continue;
            }
        }

        WidgetSlot slot;
        slot.widget_id = entry.id;
        slot.config = entry.config;

        // If this widget has a factory, create the instance early so it can
        // resolve the XML component name (e.g. carousel vs stack mode).
        if (def && def->factory) {
            slot.instance = def->factory();
            if (slot.instance) {
                slot.instance->set_config(entry.config);
                slot.component_name = slot.instance->get_component_name();
            } else {
                slot.component_name = "panel_widget_" + entry.id;
            }
        } else {
            slot.component_name = "panel_widget_" + entry.id;
        }

        enabled_widgets.push_back(std::move(slot));
    }

    // If firmware_restart is NOT already in the list (user disabled it),
    // conditionally inject it as the LAST widget when Klipper is NOT READY.
    // This ensures the restart button is always reachable during shutdown, error,
    // or startup (e.g., stuck trying to connect to an MCU).
    bool has_firmware_restart = false;
    for (const auto& slot : enabled_widgets) {
        if (slot.widget_id == "firmware_restart") {
            has_firmware_restart = true;
            break;
        }
    }
    if (!has_firmware_restart) {
        lv_subject_t* klippy = lv_xml_get_subject(nullptr, "klippy_state");
        if (klippy) {
            int state = lv_subject_get_int(klippy);
            if (state != static_cast<int>(KlippyState::READY)) {
                const char* state_names[] = {"READY", "STARTUP", "SHUTDOWN", "ERROR"};
                const char* name = (state >= 0 && state <= 3) ? state_names[state] : "UNKNOWN";
                WidgetSlot slot;
                slot.widget_id = "firmware_restart";
                slot.component_name = "panel_widget_firmware_restart";
                enabled_widgets.push_back(std::move(slot));
                spdlog::debug("[PanelWidgetManager] Injected firmware_restart (Klipper {})", name);
            }
        }
    }

    if (enabled_widgets.empty()) {
        populating_ = false;
        return {};
    }

    // --- Grid layout: compute placements first, then build minimal grid ---

    // Get current breakpoint for column count
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    int breakpoint = bp_subj ? lv_subject_get_int(bp_subj) : 2; // Default to MEDIUM

    // Build grid placement tracker to compute positions
    GridLayout grid(breakpoint);

    // Correlate widget entries with config entries to get grid positions
    const auto& entries = widget_config.entries();

    // First pass: place widgets with explicit grid positions (anchors + user-positioned)
    struct PlacedSlot {
        size_t slot_index; // Index into enabled_widgets
        int col, row, colspan, rowspan;
    };
    std::vector<PlacedSlot> placed;
    std::vector<size_t> auto_place_indices; // Widgets needing dynamic placement

    for (size_t i = 0; i < enabled_widgets.size(); ++i) {
        auto& slot = enabled_widgets[i];

        auto entry_it =
            std::find_if(entries.begin(), entries.end(),
                         [&](const PanelWidgetEntry& e) { return e.id == slot.widget_id; });

        if (entry_it != entries.end() && entry_it->has_grid_position()) {
            int col = entry_it->col;
            int row = entry_it->row;
            int colspan = entry_it->colspan;
            int rowspan = entry_it->rowspan;

            // Clamp: if widget overflows the grid, push it to fit
            if (row + rowspan > grid.rows()) {
                row = std::max(0, grid.rows() - rowspan);
            }
            if (col + colspan > grid.cols()) {
                col = std::max(0, grid.cols() - colspan);
            }

            // Pin print_status to bottom row on first layout (no user edit yet).
            // Skip pinning if the grid edit mode is active — user is positioning manually.
            // We detect user-positioned widgets by checking if the row would differ;
            // during initial layout (auto-placed), the row will be -1 and get_grid_position
            // won't match, so this only fires for the default layout.
            // TODO: replace with explicit "user_positioned" flag in config

            if (grid.place({slot.widget_id, col, row, colspan, rowspan})) {
                placed.push_back({i, col, row, colspan, rowspan});
            } else {
                spdlog::warn("[PanelWidgetManager] Cannot place widget '{}' at ({},{} {}x{})",
                             slot.widget_id, col, row, colspan, rowspan);
                auto_place_indices.push_back(i); // Fall back to auto-place
            }
        } else {
            auto_place_indices.push_back(i);
        }
    }

    // Second pass: auto-place widgets without explicit positions.
    // Place multi-cell widgets first (they need contiguous space), then pack
    // 1×1 widgets into remaining cells bottom-right first.
    std::vector<size_t> multi_cell_indices;
    std::vector<size_t> single_cell_indices;
    for (size_t idx : auto_place_indices) {
        const auto* def = find_widget_def(enabled_widgets[idx].widget_id);
        int cs = def ? def->colspan : 1;
        int rs = def ? def->rowspan : 1;
        if (cs > 1 || rs > 1) {
            multi_cell_indices.push_back(idx);
        } else {
            single_cell_indices.push_back(idx);
        }
    }

    // Place multi-cell widgets first, scanning bottom-to-top
    for (size_t slot_idx : multi_cell_indices) {
        auto& slot = enabled_widgets[slot_idx];
        const auto* def = find_widget_def(slot.widget_id);
        int colspan = def ? def->colspan : 1;
        int rowspan = def ? def->rowspan : 1;

        auto pos = grid.find_available_bottom(colspan, rowspan);
        if (pos && grid.place({slot.widget_id, pos->first, pos->second, colspan, rowspan})) {
            placed.push_back({slot_idx, pos->first, pos->second, colspan, rowspan});
        } else {
            // Grid is full — disable the widget so it goes back to the catalog
            // as an available widget. User can re-add it after freeing space.
            auto& mut_entries = widget_config.mutable_entries();
            auto cfg_it =
                std::find_if(mut_entries.begin(), mut_entries.end(),
                             [&](const PanelWidgetEntry& e) { return e.id == slot.widget_id; });
            if (cfg_it != mut_entries.end()) {
                cfg_it->enabled = false;
                cfg_it->col = -1;
                cfg_it->row = -1;
            }
            spdlog::info("[PanelWidgetManager] Disabled widget '{}' — no grid space",
                         slot.widget_id);
            const auto* def = find_widget_def(slot.widget_id);
            const char* name = def ? def->display_name : slot.widget_id.c_str();
            ui_notification_warning(fmt::format("'{}' removed — grid full", name).c_str());
        }
    }

    // Pack 1×1 widgets into remaining free cells, bottom-right first
    {
        int grid_cols = GridLayout::get_cols(breakpoint);
        int grid_rows = GridLayout::get_rows(breakpoint);

        std::vector<std::pair<int, int>> free_cells;
        for (int r = grid_rows - 1; r >= 0; --r) {
            for (int c = grid_cols - 1; c >= 0; --c) {
                if (!grid.is_occupied(c, r)) {
                    free_cells.push_back({c, r});
                }
            }
        }

        // Map: last widget → bottom-right cell, first → top-left of the block
        size_t n_single = single_cell_indices.size();
        size_t n_cells = free_cells.size();
        for (size_t i = 0; i < n_single; ++i) {
            size_t slot_idx = single_cell_indices[i];
            auto& slot = enabled_widgets[slot_idx];

            size_t cell_idx = n_single - 1 - i;
            if (cell_idx < n_cells) {
                auto [col, row] = free_cells[cell_idx];
                if (grid.place({slot.widget_id, col, row, 1, 1})) {
                    placed.push_back({slot_idx, col, row, 1, 1});
                    continue;
                }
            }

            // Fallback
            auto pos = grid.find_available_bottom(1, 1);
            if (pos && grid.place({slot.widget_id, pos->first, pos->second, 1, 1})) {
                placed.push_back({slot_idx, pos->first, pos->second, 1, 1});
            } else {
                auto& mut_entries = widget_config.mutable_entries();
                auto cfg_it =
                    std::find_if(mut_entries.begin(), mut_entries.end(),
                                 [&](const PanelWidgetEntry& e) { return e.id == slot.widget_id; });
                if (cfg_it != mut_entries.end()) {
                    cfg_it->enabled = false;
                    cfg_it->col = -1;
                    cfg_it->row = -1;
                }
                spdlog::info("[PanelWidgetManager] Disabled widget '{}' — no grid space",
                             slot.widget_id);
                const auto* def = find_widget_def(slot.widget_id);
                const char* name = def ? def->display_name : slot.widget_id.c_str();
                ui_notification_warning(fmt::format("'{}' removed — grid full", name).c_str());
            }
        }
    }

    // Write computed positions back to config entries and persist to disk.
    // This ensures auto-placed positions survive the next load() call
    // (get_widget_config_impl always reloads from the JSON store).
    {
        auto& mut_entries = widget_config.mutable_entries();
        bool any_written = false;
        for (const auto& p : placed) {
            auto& slot = enabled_widgets[p.slot_index];
            auto entry_it =
                std::find_if(mut_entries.begin(), mut_entries.end(),
                             [&](const PanelWidgetEntry& e) { return e.id == slot.widget_id; });
            if (entry_it != mut_entries.end()) {
                if (entry_it->col != p.col || entry_it->row != p.row) {
                    any_written = true;
                }
                entry_it->col = p.col;
                entry_it->row = p.row;
                entry_it->colspan = p.colspan;
                entry_it->rowspan = p.rowspan;
            }
        }
        if (any_written) {
            widget_config.save();
        }
    }

    // Compute the actual number of rows used (not the full breakpoint row count)
    int max_row_used = 0;
    for (const auto& p : placed) {
        int bottom = p.row + p.rowspan;
        if (bottom > max_row_used) {
            max_row_used = bottom;
        }
    }
    if (max_row_used == 0) {
        max_row_used = 1; // At least 1 row if any widgets placed
    }

    // Generate grid descriptors sized to actual content
    // Columns: use breakpoint column count (fills available width)
    // Rows: only create rows that are actually occupied (avoids empty rows stealing space)
    auto& dsc = grid_descriptors_[panel_id];
    dsc.col_dsc = GridLayout::make_col_dsc(breakpoint);
    dsc.row_dsc.clear();
    for (int r = 0; r < max_row_used; ++r) {
        dsc.row_dsc.push_back(LV_GRID_FR(1));
    }
    dsc.row_dsc.push_back(LV_GRID_TEMPLATE_LAST);

    // Set up grid on container
    lv_obj_set_layout(container, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(container, dsc.col_dsc.data(), dsc.row_dsc.data());
    lv_obj_set_style_pad_column(container, theme_manager_get_spacing("space_xs"), 0);
    lv_obj_set_style_pad_row(container, theme_manager_get_spacing("space_xs"), 0);

    spdlog::debug("[PanelWidgetManager] Grid layout: {}cols x {}rows (bp={}) for '{}'",
                  GridLayout::get_cols(breakpoint), max_row_used, breakpoint, panel_id);

    // Create merged card backgrounds behind adjacent 1x1 widgets.
    // BFS flood-fill finds connected components of 1x1 cells, then a single
    // card object spans each component's bounding rectangle.
    {
        // Collect all placed 1x1 cells into a set for O(1) lookup
        struct CellHash {
            size_t operator()(const std::pair<int, int>& p) const {
                return std::hash<int>()(p.first) ^ (std::hash<int>()(p.second) << 16);
            }
        };
        std::unordered_set<std::pair<int, int>, CellHash> single_cells;
        for (const auto& p : placed) {
            if (p.colspan == 1 && p.rowspan == 1) {
                single_cells.insert({p.col, p.row});
            }
        }

        // BFS flood-fill to find connected components (4-directional adjacency)
        std::unordered_set<std::pair<int, int>, CellHash> visited;
        for (const auto& cell : single_cells) {
            if (visited.count(cell)) {
                continue;
            }

            // BFS from this cell
            std::queue<std::pair<int, int>> q;
            q.push(cell);
            visited.insert(cell);

            int min_col = cell.first, max_col = cell.first;
            int min_row = cell.second, max_row_card = cell.second;

            while (!q.empty()) {
                auto [c, r] = q.front();
                q.pop();
                min_col = std::min(min_col, c);
                max_col = std::max(max_col, c);
                min_row = std::min(min_row, r);
                max_row_card = std::max(max_row_card, r);

                const std::pair<int, int> neighbors[] = {{c - 1, r}, {c + 1, r}, {c, r - 1}, {c, r + 1}};
                for (const auto& n : neighbors) {
                    if (single_cells.count(n) && !visited.count(n)) {
                        visited.insert(n);
                        q.push(n);
                    }
                }
            }

            int card_colspan = max_col - min_col + 1;
            int card_rowspan = max_row_card - min_row + 1;

            // Create a plain lv_obj with Card styling as the background
            lv_obj_t* card_bg = lv_obj_create(container);
            lv_obj_remove_style(card_bg, nullptr, LV_PART_MAIN);
            lv_obj_add_style(card_bg, ThemeManager::instance().get_style(StyleRole::Card), LV_PART_MAIN);
            lv_obj_set_style_pad_all(card_bg, 0, 0);
            lv_obj_remove_flag(card_bg, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(card_bg, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_grid_cell(card_bg, LV_GRID_ALIGN_STRETCH, min_col, card_colspan,
                                 LV_GRID_ALIGN_STRETCH, min_row, card_rowspan);

            spdlog::debug("[PanelWidgetManager] Card background at ({},{} {}x{})",
                          min_col, min_row, card_colspan, card_rowspan);
        }
    }

    // Second pass: create XML components and place in grid cells
    std::vector<std::unique_ptr<PanelWidget>> result;

    for (const auto& p : placed) {
        try {
            auto& slot = enabled_widgets[p.slot_index];

            // Create XML component
            auto* widget = static_cast<lv_obj_t*>(
                lv_xml_create(container, slot.component_name.c_str(), nullptr));
            if (!widget) {
                spdlog::warn("[PanelWidgetManager] Failed to create widget: {} (component: {})",
                             slot.widget_id, slot.component_name);
                continue;
            }

            // Place in grid cell
            lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, p.col, p.colspan,
                                 LV_GRID_ALIGN_STRETCH, p.row, p.rowspan);

            // Tag widget with its config ID so GridEditMode can identify it
            lv_obj_set_name(widget, slot.widget_id.c_str());

            spdlog::debug("[PanelWidgetManager] Placed widget '{}' at ({},{} {}x{})", slot.widget_id,
                          p.col, p.row, p.colspan, p.rowspan);

            // Attach the pre-created PanelWidget instance if present
            if (slot.instance) {
                slot.instance->attach(widget, lv_scr_act());

                // Notify widget of its grid allocation and approximate pixel size
                int cols = GridLayout::get_cols(breakpoint);
                int rows = GridLayout::get_rows(breakpoint);
                int container_w = lv_obj_get_content_width(container);
                int container_h = lv_obj_get_content_height(container);
                int cell_w = (cols > 0) ? container_w / cols : 0;
                int cell_h = (rows > 0) ? container_h / rows : 0;
                slot.instance->on_size_changed(p.colspan, p.rowspan, cell_w * p.colspan,
                                               cell_h * p.rowspan);

                result.push_back(std::move(slot.instance));
            }

            // Propagate width to AMS mini status (pure XML widget, no PanelWidget)
            if (slot.widget_id == "ams") {
                lv_obj_t* ams_child = lv_obj_get_child(widget, 0);
                if (ams_child && ui_ams_mini_status_is_valid(ams_child)) {
                    int ams_w = lv_obj_get_content_width(container);
                    int cols = GridLayout::get_cols(breakpoint);
                    int cell_w = (cols > 0) ? ams_w / cols : 0;
                    ui_ams_mini_status_set_width(ams_child, cell_w * p.colspan);
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("[PanelWidgetManager] Widget '{}' creation failed: {}",
                          enabled_widgets[p.slot_index].widget_id, e.what());
        }
    }

    spdlog::debug("[PanelWidgetManager] Populated {} widgets ({} with factories) via grid for '{}'",
                  placed.size(), result.size(), panel_id);

    populating_ = false;
    return result;
}

void PanelWidgetManager::setup_gate_observers(const std::string& panel_id,
                                              RebuildCallback rebuild_cb) {
    using helix::ui::observe_int_sync;

    // Observers must be destroyed BEFORE timers — observer callbacks
    // capture &timer references into rebuild_timers_
    gate_observers_.erase(panel_id);
    rebuild_timers_.erase(panel_id);
    auto& observers = gate_observers_[panel_id];
    // 200ms coalesce window: during startup, multiple gate subjects fire in rapid
    // succession as hardware is discovered (power, LED, filament, humidity, etc.).
    // A 1ms window only coalesces within a single LVGL tick, but discovery events
    // arrive across multiple ticks (~30ms spread in mock, potentially wider on real
    // hardware with WebSocket latency). 200ms batches all discovery into one rebuild.
    auto& timer = rebuild_timers_.emplace(panel_id, ui::CoalescedTimer(300)).first->second;

    // Collect unique gate subject names from the widget registry
    std::vector<const char*> gate_names;
    for (const auto& def : get_all_widget_defs()) {
        if (def.hardware_gate_subject) {
            // Avoid duplicates
            bool found = false;
            for (const auto* existing : gate_names) {
                if (std::strcmp(existing, def.hardware_gate_subject) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                gate_names.push_back(def.hardware_gate_subject);
            }
        }
    }

    // Also observe klippy_state for firmware_restart conditional injection
    gate_names.push_back("klippy_state");

    for (const auto* name : gate_names) {
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        if (!subject) {
            spdlog::trace("[PanelWidgetManager] Gate subject '{}' not registered yet", name);
            continue;
        }

        // Use observe_int_sync with PanelWidgetManager as the class template parameter.
        // The callback ignores the value and schedules a coalesced rebuild.
        // Multiple gate subjects changing in the same LVGL tick (common during
        // startup discovery) coalesce into a single rebuild instead of one each.
        observers.push_back(observe_int_sync<PanelWidgetManager>(
            subject, this, [&timer, rebuild_cb](PanelWidgetManager* /*self*/, int /*value*/) {
                timer.schedule(rebuild_cb);
            }));

        spdlog::trace("[PanelWidgetManager] Observing gate subject '{}' for panel '{}'", name,
                      panel_id);
    }

    spdlog::debug("[PanelWidgetManager] Set up {} gate observers for panel '{}'", observers.size(),
                  panel_id);
}

void PanelWidgetManager::clear_gate_observers(const std::string& panel_id) {
    auto it = gate_observers_.find(panel_id);
    if (it != gate_observers_.end()) {
        spdlog::debug("[PanelWidgetManager] Clearing {} gate observers for panel '{}'",
                      it->second.size(), panel_id);
        // Observers must be destroyed BEFORE timers — observer callbacks
        // capture &timer references into rebuild_timers_
        gate_observers_.erase(it);
    }
    rebuild_timers_.erase(panel_id);
}

PanelWidgetConfig& PanelWidgetManager::get_widget_config(const std::string& panel_id) {
    return get_widget_config_impl(panel_id);
}

} // namespace helix
