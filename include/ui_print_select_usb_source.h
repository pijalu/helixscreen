// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "usb_backend.h"

#include <functional>
#include <lvgl.h>
#include <string>
#include <vector>

// Forward declarations
struct PrintFileData;
class UsbManager;

/**
 * @brief File source for print select panel
 *
 * Defined here (not in ui_panel_print_select.h) to avoid circular dependencies.
 */
enum class FileSource {
    PRINTER = 0, ///< Files from Moonraker (printer storage)
    USB = 1      ///< Files from USB drive
};

namespace helix::ui {

/**
 * @file ui_print_select_usb_source.h
 * @brief USB file source manager for print selection panel
 *
 * Handles USB drive detection, G-code file scanning, and source switching
 * between Printer (Moonraker) and USB sources.
 *
 * ## Key Features:
 * - USB drive insertion/removal detection
 * - G-code file scanning from USB drives
 * - Source button state management (Printer/USB toggle)
 * - Conversion of USB files to PrintFileData format
 *
 * ## Usage:
 * @code
 * PrintSelectUsbSource usb_source;
 * usb_source.setup(panel, printer_btn, usb_btn);
 * usb_source.set_usb_manager(manager);
 * usb_source.set_on_files_ready([](auto& files) { ... });
 * usb_source.set_on_source_changed([](FileSource source) { ... });
 *
 * // On USB button click:
 * usb_source.select_usb_source();
 *
 * // On USB drive events:
 * usb_source.on_drive_inserted();
 * usb_source.on_drive_removed();
 * @endcode
 */

/**
 * @brief Callback when files are ready from USB source
 * @param files Vector of PrintFileData from USB drive
 */
using UsbFilesReadyCallback = std::function<void(std::vector<PrintFileData>&& files)>;

/**
 * @brief Callback when source changes
 * @param source New file source (PRINTER or USB)
 */
using SourceChangedCallback = std::function<void(FileSource source)>;

/**
 * @brief USB file source manager
 */
class PrintSelectUsbSource {
  public:
    PrintSelectUsbSource() = default;
    ~PrintSelectUsbSource() = default;

    // Non-copyable, movable
    PrintSelectUsbSource(const PrintSelectUsbSource&) = delete;
    PrintSelectUsbSource& operator=(const PrintSelectUsbSource&) = delete;
    PrintSelectUsbSource(PrintSelectUsbSource&&) noexcept = default;
    PrintSelectUsbSource& operator=(PrintSelectUsbSource&&) noexcept = default;

    // === Setup ===

    /**
     * @brief Initialize with source selector buttons
     *
     * Finds buttons by name and sets up initial state.
     * Both source buttons are hidden by default until a USB drive is inserted.
     *
     * @param panel Root panel widget (for button lookup)
     * @return true if buttons found successfully
     */
    bool setup(lv_obj_t* panel);

    /**
     * @brief Set UsbManager dependency
     */
    void set_usb_manager(UsbManager* manager);

    // === Callbacks ===

    /**
     * @brief Set callback for when USB files are ready
     */
    void set_on_files_ready(UsbFilesReadyCallback callback) {
        on_files_ready_ = std::move(callback);
    }

    /**
     * @brief Set callback for source changes
     */
    void set_on_source_changed(SourceChangedCallback callback) {
        on_source_changed_ = std::move(callback);
    }

    // === Source Selection ===

    /**
     * @brief Switch to Printer (Moonraker) source
     *
     * Updates button states and invokes source changed callback.
     */
    void select_printer_source();

    /**
     * @brief Switch to USB source
     *
     * Updates button states, scans USB drive, and invokes callbacks.
     */
    void select_usb_source();

    /**
     * @brief Get current file source
     */
    [[nodiscard]] FileSource get_current_source() const {
        return current_source_;
    }

    /**
     * @brief Check if USB source is currently active
     */
    [[nodiscard]] bool is_usb_active() const {
        return current_source_ == FileSource::USB;
    }

    // === USB Drive Events ===

    /**
     * @brief Handle USB drive insertion
     *
     * Shows both source selector buttons, unless Moonraker has
     * symlink access to USB files (same-host setup).
     */
    void on_drive_inserted();

    /**
     * @brief Handle USB drive removal
     *
     * Hides both source selector buttons. If USB source is active,
     * switches to Printer and invokes source changed callback.
     */
    void on_drive_removed();

    /**
     * @brief Set whether Moonraker has direct access to USB files via symlink
     *
     * When true, the USB tab is hidden permanently since files are accessible
     * via the Printer source. This is the case when Klipper's mod creates
     * a symlink like gcodes/usb -> /media/sda1.
     *
     * @param has_access true if Moonraker can see USB files via symlink
     */
    void set_moonraker_has_usb_access(bool has_access);

    /**
     * @brief Check if Moonraker has symlink access to USB files
     */
    [[nodiscard]] bool moonraker_has_usb_access() const {
        return moonraker_has_usb_access_;
    }

    // === File Operations ===

    /**
     * @brief Refresh USB file list
     *
     * Scans connected USB drives for G-code files.
     * Invokes on_files_ready callback with results.
     */
    void refresh_files();

    /**
     * @brief Clear cached USB files
     */
    void clear_files() {
        usb_files_.clear();
    }

    // === Button References (for external event handling) ===

    [[nodiscard]] lv_obj_t* get_printer_button() const {
        return source_printer_btn_;
    }
    [[nodiscard]] lv_obj_t* get_usb_button() const {
        return source_usb_btn_;
    }

  private:
    // === Dependencies ===
    UsbManager* usb_manager_ = nullptr;

    // === Widget References ===
    lv_obj_t* source_printer_btn_ = nullptr;
    lv_obj_t* source_usb_btn_ = nullptr;

    // === State ===
    FileSource current_source_ = FileSource::PRINTER;
    std::vector<UsbGcodeFile> usb_files_;
    bool moonraker_has_usb_access_ = false; ///< True if Moonraker has symlink to USB files

    // === Callbacks ===
    UsbFilesReadyCallback on_files_ready_;
    SourceChangedCallback on_source_changed_;

    // === Internal Methods ===

    /**
     * @brief Update button visual states based on current source
     */
    void update_button_states();

    /**
     * @brief Convert USB files to PrintFileData format
     */
    [[nodiscard]] std::vector<PrintFileData> convert_to_print_file_data() const;
};

} // namespace helix::ui
