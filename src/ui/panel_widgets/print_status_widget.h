// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_job_queue_modal.h"
#include "ui_observer_guard.h"
#include "ui_runout_guidance_modal.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#include "print_history_manager.h"

#include <string>
#include <unordered_set>

namespace helix {

class PrinterState;
enum class PrintJobState;

class PrintStatusWidget : public PanelWidget {
  public:
    PrintStatusWidget();
    ~PrintStatusWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "print_status";
    }

    // Configuration
    void set_config(const nlohmann::json& config) override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    /// Re-check runout condition after wizard completion
    void trigger_idle_runout_check();

    /// XML event callback — opens print status panel or file browser
    static void print_card_clicked_cb(lv_event_t* e);

    /// Library row callbacks
    static void library_files_cb(lv_event_t* e);
    static void library_last_cb(lv_event_t* e);
    static void library_recent_cb(lv_event_t* e);
    static void library_queue_cb(lv_event_t* e);

    /// Configure picker callback
    static void print_status_picker_backdrop_cb(lv_event_t* e);

    /// Registry of live (attached) widget instances for use-after-free prevention
    static std::unordered_set<PrintStatusWidget*>& live_instances();

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Cached widget references (looked up after XML creation)
    lv_obj_t* print_card_thumb_ = nullptr;        // Idle state thumbnail
    lv_obj_t* print_card_active_thumb_ = nullptr; // Active print thumbnail
    lv_obj_t* print_card_layout_ = nullptr;       // Row/column layout container
    lv_obj_t* print_card_thumb_wrap_ = nullptr;   // Thumbnail wrapper
    lv_obj_t* print_card_info_ = nullptr;         // Info section (filename/progress)

    // Library idle state widgets
    lv_obj_t* print_card_idle_ = nullptr;          // Full library idle card
    lv_obj_t* print_card_idle_compact_ = nullptr;  // Compact idle card (1x2)
    lv_obj_t* print_card_thumb_compact_ = nullptr; // Compact thumbnail
    lv_obj_t* library_row_last_ = nullptr;         // Print Last row (for graying out)
    lv_obj_t* compact_row_last_ = nullptr;         // Compact Print Last row (for graying out)
    lv_obj_t* library_row_queue_ = nullptr;

    // Size-dependent subject for XML bindings (1 = column/2x2 mode, 0 = row/wide)
    static inline lv_subject_t column_mode_subject_;
    static inline bool column_mode_subject_initialized_ = false;

    // Compact mode and state tracking
    bool is_compact_ = false;
    bool last_print_available_ = false;

    // PrinterState reference for subject access
    PrinterState& printer_state_;

    // Observers (RAII cleanup via ObserverGuard)
    ObserverGuard print_state_observer_;
    ObserverGuard print_thumbnail_path_observer_;
    ObserverGuard filament_runout_observer_;
    ObserverGuard job_queue_count_observer_;

    // Guards async thumbnail callbacks and history observer from use-after-free
    helix::AsyncLifetimeGuard lifetime_;

    // History observer for updating idle thumbnail when history loads
    helix::HistoryChangedCallback history_changed_cb_;

    // Filament runout modal
    RunoutGuidanceModal runout_modal_;
    bool runout_modal_shown_ = false;

    // Job queue
    helix::JobQueueModal job_queue_modal_;

    // Print card update methods
    [[nodiscard]] std::string get_last_print_thumbnail_path() const;
    void handle_print_card_clicked();
    void on_print_state_changed(PrintJobState state);
    void on_print_thumbnail_path_changed(const char* path);
    void reset_print_card_to_idle();
    void update_idle_compact_mode();
    void update_last_print_availability();

    // Library action handlers
    void handle_library_files();
    void handle_library_last();
    void handle_library_recent();
    void handle_library_queue();
    void update_job_queue_row_visibility();

    // Filament runout handling
    void check_and_show_idle_runout_modal();
    void show_idle_runout_modal();

    // Configuration state
    nlohmann::json config_;
    bool show_title_ = true;
    bool show_print_files_ = true;
    bool show_reprint_last_ = true;
    bool show_recent_prints_ = true;
    bool show_job_queue_ = true;

    // Configure picker
    lv_obj_t* picker_backdrop_ = nullptr;
    void show_configure_picker();
    void dismiss_configure_picker();
    void apply_visibility_config();
    void apply_picker_state();

    static PrintStatusWidget* s_active_picker_;
};

} // namespace helix
