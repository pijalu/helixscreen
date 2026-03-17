// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "action_prompt_manager.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * @file action_prompt_modal.h
 * @brief Modal dialog for displaying Klipper action:prompt messages
 *
 * Displays interactive prompts from Klipper macros with dynamic buttons.
 * Buttons can be styled with different colors and grouped for layout.
 *
 * ## Integration Note
 * The component must be registered in main.cpp before use:
 * @code
 * lv_xml_register_component_from_file("action_prompt_modal",
 *     "ui_xml/action_prompt_modal.xml");
 * @endcode
 *
 * ## Usage:
 * @code
 * helix::ui::ActionPromptModal modal;
 * modal.set_gcode_callback([&api](const std::string& gcode) {
 *     api.send_gcode(gcode);
 * });
 * modal.show_prompt(parent, prompt_data);
 * @endcode
 */

namespace helix::ui {

/**
 * @brief Modal dialog for Klipper action:prompt messages
 *
 * Dynamically creates buttons based on PromptData from the ActionPromptManager.
 * Supports button colors, grouping, and footer buttons.
 */
class ActionPromptModal : public Modal {
  public:
    /**
     * @brief Callback type for button clicks that send gcode
     */
    using GcodeCallback = std::function<void(const std::string& gcode)>;

    ActionPromptModal();
    ~ActionPromptModal() override;

    // Non-copyable
    ActionPromptModal(const ActionPromptModal&) = delete;
    ActionPromptModal& operator=(const ActionPromptModal&) = delete;

    // Movable
    ActionPromptModal(ActionPromptModal&& other) noexcept;
    ActionPromptModal& operator=(ActionPromptModal&& other) noexcept;

    /**
     * @brief Show modal with prompt data
     * @param parent Parent screen for the modal
     * @param data Prompt data from ActionPromptManager
     * @return true if modal was created successfully
     */
    bool show_prompt(lv_obj_t* parent, const PromptData& data);

    /**
     * @brief Set callback for when a button is clicked
     *
     * The callback receives the gcode string associated with the button.
     * After calling the callback, the modal closes automatically.
     *
     * @param callback Function to send gcode to printer
     */
    void set_gcode_callback(GcodeCallback callback);

    // Modal interface
    [[nodiscard]] const char* get_name() const override {
        return "Action Prompt Modal";
    }
    [[nodiscard]] const char* component_name() const override {
        return "action_prompt_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    /**
     * @brief Data passed as user_data to button event callbacks
     *
     * Owns a copy of the gcode string (not a pointer into prompt_data_.buttons)
     * and holds a shared_ptr to the alive flag to keep the control block alive
     * until LVGL delivers (or discards) the button event. Using weak_ptr here
     * caused SIGSEGV in weak_ptr::lock() when the control block was freed
     * before the queued click event fired (crash #437).
     */
    struct ButtonCallbackData {
        ActionPromptModal* modal;
        std::shared_ptr<std::atomic<bool>> alive;
        std::string gcode; // Owned copy, safe from vector reallocation
    };

    // === State ===
    PromptData prompt_data_;
    GcodeCallback gcode_callback_;

    // === Lifetime safety ===
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    // === Dynamic button tracking ===
    std::vector<lv_obj_t*> created_buttons_;
    std::vector<lv_obj_t*> created_text_labels_;
    std::vector<std::unique_ptr<ButtonCallbackData>> button_callback_data_;

    // === Internal Methods ===
    void populate_content();
    void create_text_lines();
    void create_buttons();
    void create_button(const PromptButton& btn, lv_obj_t* container);
    lv_color_t get_button_color(const std::string& color_name);
    void clear_dynamic_content();

    // === Event Handler ===
    void handle_button_click(const std::string& gcode);

    // === Static Callback Registration ===
    static void register_callbacks();
    static bool callbacks_registered_;

    // === Static Callbacks ===
    static void on_button_cb(lv_event_t* e);
};

} // namespace helix::ui
