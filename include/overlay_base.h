// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file overlay_base.h
 * @brief Abstract base class for overlay panels with lifecycle hooks
 *
 * @pattern Two-phase init (init_subjects -> XML -> callbacks); lifecycle hooks via
 * NavigationManager
 * @threading Main thread only
 *
 * OverlayBase provides lifecycle management for overlay panels:
 * - on_activate() called when overlay becomes visible (slide-in complete)
 * - on_deactivate() called when overlay is being hidden (before slide-out)
 *
 * ## Lifecycle Flow
 *
 * ### Overlay pushed:
 * 1. If first overlay: main panel's on_deactivate() is called
 * 2. If nested: previous overlay's on_deactivate() is called
 * 3. Overlay shows with slide-in animation
 * 4. Overlay's on_activate() is called
 *
 * ### Overlay popped (go_back):
 * 1. Overlay's on_deactivate() is called
 * 2. Slide-out animation plays
 * 3. If returning to main panel: main panel's on_activate() is called
 * 4. If returning to previous overlay: previous overlay's on_activate() is called
 *
 * ## Usage Pattern:
 *
 * @code
 * class MyOverlay : public OverlayBase {
 * public:
 *     void init_subjects() override {
 *         // Register LVGL subjects for XML binding
 *         subjects_initialized_ = true;
 *     }
 *
 *     void register_callbacks() override {
 *         // Register event callbacks with lv_xml_register_event_cb()
 *     }
 *
 *     lv_obj_t* create(lv_obj_t* parent) override {
 *         overlay_root_ = lv_xml_create(parent, "my_overlay", nullptr);
 *         return overlay_root_;
 *     }
 *
 *     const char* get_name() const override { return "My Overlay"; }
 *
 *     void on_activate() override {
 *         // Start scanning, refresh data, etc.
 *         visible_ = true;
 *     }
 *
 *     void on_deactivate() override {
 *         // Stop scanning, cancel pending operations, etc.
 *         visible_ = false;
 *     }
 * };
 * @endcode
 *
 * @see NetworkSettingsOverlay for reference implementation
 */

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"

#include <spdlog/spdlog.h>

// Include for SubjectManager (needed for deinit_subjects_base)
#include "subject_managed_panel.h"

/**
 * @class OverlayBase
 * @brief Abstract base class for overlay panels with lifecycle management
 *
 * Provides shared infrastructure for overlay panels including:
 * - Lifecycle hooks (on_activate/on_deactivate) called by NavigationManager
 * - Two-phase initialization (init_subjects -> create -> register_callbacks)
 * - Async-safe cleanup pattern
 *
 * @implements IPanelLifecycle for NavigationManager dispatch
 */
class OverlayBase : public IPanelLifecycle {
  public:
    /**
     * @brief Virtual destructor for proper cleanup
     */
    virtual ~OverlayBase();

    // Non-copyable (unique overlay instances)
    OverlayBase(const OverlayBase&) = delete;
    OverlayBase& operator=(const OverlayBase&) = delete;

    //
    // === Core Interface (must implement) ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * MUST be called BEFORE create() to ensure bindings work.
     * Implementations should set subjects_initialized_ = true.
     */
    virtual void init_subjects() = 0;

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     *
     * Implementations should store result in overlay_root_.
     */
    virtual lv_obj_t* create(lv_obj_t* parent) = 0;

    /**
     * @brief Get human-readable overlay name
     *
     * Used in logging and debugging.
     *
     * @return Overlay name (e.g., "Network Settings")
     */
    const char* get_name() const override = 0;

    //
    // === Optional Hooks (override as needed) ===
    //

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Called after create() to register XML event callbacks.
     * Default implementation does nothing.
     */
    virtual void register_callbacks() {}

    /**
     * @brief Called when overlay becomes visible
     *
     * Override to start scanning, refresh data, begin animations, etc.
     * Called by NavigationManager after slide-in animation starts.
     * Default implementation sets visible_ = true.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Override to stop scanning, cancel pending operations, pause timers.
     * Called by NavigationManager before slide-out animation starts.
     * Default implementation sets visible_ = false.
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     *
     * Call this before destroying the overlay to handle any pending
     * async callbacks safely. Sets cleanup_called_ flag.
     */
    virtual void cleanup();

    //
    // === State Queries ===
    //

    /**
     * @brief Check if overlay is currently visible
     * @return true if overlay is visible
     */
    bool is_visible() const {
        return visible_;
    }

    /**
     * @brief Check if cleanup has been called
     * @return true if cleanup() was called
     */
    bool cleanup_called() const {
        return cleanup_called_;
    }

    /**
     * @brief Get root overlay widget
     * @return Root widget, or nullptr if not created
     */
    lv_obj_t* get_root() const {
        return overlay_root_;
    }

    /**
     * @brief Destroy overlay widget tree to free memory
     *
     * Called by close callbacks registered via lazy_create_and_push_overlay
     * with destroy_on_close=true. Performs:
     * 1. Drains UpdateQueue (process pending deferred callbacks)
     * 2. Unregisters close callback from NavigationManager
     * 3. Unregisters overlay instance from NavigationManager
     * 4. Deletes the widget tree via safe_delete()
     * 5. Calls on_ui_destroyed() to null derived class widget pointers
     *
     * The overlay object (subjects, state) survives — only the widget tree
     * is destroyed. Next open triggers re-creation via lazy_create_and_push_overlay.
     *
     * @param cached_panel Reference to the caller's cached lv_obj_t* pointer
     *                     (the same reference passed to lazy_create_and_push_overlay)
     */
    void destroy_overlay_ui(lv_obj_t*& cached_panel);

    /**
     * @brief Check if subjects have been initialized
     * @return true if init_subjects() was called
     */
    bool are_subjects_initialized() const {
        return subjects_initialized_;
    }

  protected:
    /**
     * @brief Default constructor (protected - use derived classes)
     */
    OverlayBase() = default;

    /**
     * @brief Create overlay from XML with standard setup
     *
     * Helper method that consolidates common overlay creation boilerplate:
     * - Sets parent_screen_ and resets cleanup_called_
     * - Creates overlay from XML using lv_xml_create()
     * - Applies standard overlay setup (header, content padding)
     * - Hides overlay initially
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @param component_name XML component name to create (e.g., "console_panel")
     * @return Root object of overlay, or nullptr on failure
     *
     * @code
     * lv_obj_t* MyOverlay::create(lv_obj_t* parent) {
     *     if (!create_overlay_from_xml(parent, "my_overlay")) {
     *         return nullptr;
     *     }
     *     // Find additional widgets and setup panel-specific state...
     *     return overlay_root_;
     * }
     * @endcode
     */
    lv_obj_t* create_overlay_from_xml(lv_obj_t* parent, const char* component_name);

    //
    // === Protected State ===
    //

    lv_obj_t* overlay_root_ = nullptr;  ///< Root widget of overlay UI
    lv_obj_t* parent_screen_ = nullptr; ///< Parent screen (for overlay setup)
    bool subjects_initialized_ = false; ///< True after init_subjects() called
    bool visible_ = false;              ///< True when overlay is visible
    bool cleanup_called_ = false;       ///< True after cleanup() called

    /// Async callback safety. Automatically invalidated on cleanup()/on_deactivate().
    /// Subclasses use lifetime_.defer(...) or lifetime_.token() for
    /// bg-thread callbacks that need to touch UI.
    helix::AsyncLifetimeGuard lifetime_;

    /**
     * @brief Called after widget tree is destroyed by destroy_overlay_ui()
     *
     * Override to null derived-class widget pointers so that create()
     * works correctly when re-invoked. The base overlay_root_ is already
     * nulled before this is called.
     *
     * Default implementation does nothing.
     */
    virtual void on_ui_destroyed() {}

    //
    // === Subject Init/Deinit Guards ===
    //

    /**
     * @brief Execute init function with guard against double initialization
     *
     * Wraps the actual subject initialization code with a guard that prevents
     * double initialization and logs appropriately.
     *
     * @tparam Func Callable type (typically lambda)
     * @param init_func Function to execute if not already initialized
     * @return true if initialization was performed, false if already initialized
     *
     * Example:
     * @code
     * void MyOverlay::init_subjects() {
     *     init_subjects_guarded([this]() {
     *         UI_MANAGED_SUBJECT_INT(my_subject_, 0, "my_subject", subjects_);
     *     });
     * }
     * @endcode
     */
    template <typename Func> bool init_subjects_guarded(Func&& init_func) {
        if (subjects_initialized_) {
            spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
            return false;
        }
        init_func();
        subjects_initialized_ = true;
        spdlog::debug("[{}] Subjects initialized", get_name());
        return true;
    }

    /**
     * @brief Deinitialize subjects via SubjectManager with guard
     *
     * Checks subjects_initialized_ flag before deinitializing.
     * Resets the flag after cleanup.
     *
     * @param subjects Reference to the overlay's SubjectManager
     *
     * Example:
     * @code
     * void MyOverlay::deinit_subjects() {
     *     deinit_subjects_base(subjects_);
     * }
     * @endcode
     */
    void deinit_subjects_base(SubjectManager& subjects) {
        if (!subjects_initialized_) {
            return;
        }
        subjects.deinit_all();
        subjects_initialized_ = false;
        spdlog::trace("[{}] Subjects deinitialized", get_name());
    }
};
