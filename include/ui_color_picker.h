// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "subject_managed_panel.h"

#include <cstdint>
#include <functional>
#include <string>

namespace helix {

/**
 * @brief Map hex color value to human-readable name
 *
 * Uses algorithmic color naming (HSL-based) with special names for
 * preset colors that have non-standard names (Gold, Bronze, Wood, etc.)
 *
 * @param rgb Color as RGB packed uint32_t
 * @return Human-readable color name
 */
std::string get_color_name_from_hex(uint32_t rgb);

} // namespace helix

namespace helix::ui {

/**
 * @file ui_color_picker.h
 * @brief Color picker modal for filament and theme color selection
 *
 * Displays preset swatches and HSV picker for custom colors.
 * Extends ModalBase for RAII lifecycle and backdrop handling.
 *
 * ## Usage:
 * @code
 * helix::ui::ColorPicker picker;
 * picker.set_color_callback([](uint32_t rgb, const std::string& name) {
 *     // Handle color selection
 * });
 * picker.show_with_color(parent, initial_color_rgb);
 * @endcode
 */
class ColorPicker : public Modal {
  public:
    /**
     * @brief Callback type for color selection
     * @param color_rgb Selected color as RGB packed uint32_t
     * @param color_name Human-readable color name
     */
    using ColorCallback = std::function<void(uint32_t color_rgb, const std::string& color_name)>;

    ColorPicker();
    ~ColorPicker() override;

    // Non-copyable
    ColorPicker(const ColorPicker&) = delete;
    ColorPicker& operator=(const ColorPicker&) = delete;

    // Movable
    ColorPicker(ColorPicker&& other) noexcept;
    ColorPicker& operator=(ColorPicker&& other) noexcept;

    /**
     * @brief Show color picker with initial color
     * @param parent Parent screen for the modal
     * @param initial_color Initial color to display (RGB packed)
     * @return true if modal was created successfully
     */
    bool show_with_color(lv_obj_t* parent, uint32_t initial_color);

    /**
     * @brief Set callback for when color is selected
     * @param callback Function to call with selected color
     */
    void set_color_callback(ColorCallback callback);

    /**
     * @brief Set callback for when picker is dismissed (any close - select, cancel, or backdrop)
     * @param callback Function to call on dismiss
     */
    void set_dismiss_callback(std::function<void()> callback);

    // Modal interface
    [[nodiscard]] const char* get_name() const override {
        return "Color Picker";
    }
    [[nodiscard]] const char* component_name() const override {
        return "color_picker";
    }

  protected:
    void on_show() override;
    void on_hide() override;
    void on_cancel() override;

  private:
    // === State ===
    uint32_t selected_color_ = 0x808080;
    ColorCallback color_callback_;
    std::function<void()> dismiss_callback_;

    // === Subjects for XML binding ===
    SubjectManager subjects_;
    lv_subject_t name_subject_;
    char hex_buf_[16] = {0};
    char name_buf_[64] = {0};
    bool subjects_initialized_ = false;

    // === Hex input state ===
    bool hex_input_updating_ = false; // Prevent feedback loop

    // === Cached widget pointers (set in on_show, cleared in on_hide) ===
    lv_obj_t* preview_ = nullptr;
    lv_obj_t* preview_tiny_ = nullptr;
    lv_obj_t* hex_input_ = nullptr;
    lv_obj_t* hex_input_tiny_ = nullptr;
    lv_obj_t* hsv_picker_ = nullptr;
    lv_obj_t* hsv_picker_tiny_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* name_label_tiny_ = nullptr;

    // === Compact mode tab switching (MICRO + TINY share the same layout) ===
    bool is_tiny_mode_ = false; // Covers both MICRO and TINY breakpoints
    lv_obj_t* presets_content_ = nullptr;
    lv_obj_t* custom_content_ = nullptr;
    lv_obj_t* btn_tab_presets_ = nullptr;
    lv_obj_t* btn_tab_custom_ = nullptr;

    // === Internal Methods ===
    void init_subjects();
    void deinit_subjects();
    void update_preview(uint32_t color_rgb, bool from_hsv_picker = false,
                        bool from_hex_input = false);
    void switch_tab(bool show_custom);

    // === Event Handlers (called by static callbacks) ===
    void handle_swatch_clicked(lv_obj_t* swatch);
    void handle_select();
    void handle_hex_input_changed();
    void handle_hex_input_defocused();

    // === Static Callback Registration ===
    static void register_callbacks();
    static bool callbacks_registered_;
    static ColorPicker* active_instance_;

    // === Static Callbacks (traverse widget tree to find modal instance) ===
    static void on_close_cb(lv_event_t* e);
    static void on_swatch_cb(lv_event_t* e);
    static void on_cancel_cb(lv_event_t* e);
    static void on_select_cb(lv_event_t* e);
    static void on_hex_input_changed_cb(lv_event_t* e);
    static void on_hex_input_defocused_cb(lv_event_t* e);
    static void on_tab_presets_cb(lv_event_t* e);
    static void on_tab_custom_cb(lv_event_t* e);

    /**
     * @brief Get the currently active ColorPicker instance
     *
     * Returns the static active instance pointer. Only one ColorPicker
     * can be visible at a time.
     * @param e LVGL event (unused, kept for callback signature compatibility)
     * @return ColorPicker pointer, or nullptr if none active
     */
    static ColorPicker* get_instance_from_event(lv_event_t* e);
};

} // namespace helix::ui
