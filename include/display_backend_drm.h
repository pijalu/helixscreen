// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen - Linux DRM/KMS Display Backend
//
// Modern Linux display backend using Direct Rendering Manager (DRM)
// with Kernel Mode Setting (KMS). Preferred for Raspberry Pi.

#pragma once

#ifdef HELIX_DISPLAY_DRM

#include "display_backend.h"
#include "touch_calibration_wrapper.h"

#include <string>

/**
 * @brief Linux DRM/KMS display backend for modern embedded systems
 *
 * Uses LVGL's DRM driver for hardware-accelerated rendering on
 * systems with GPU support (like Raspberry Pi 4/5).
 *
 * Advantages over framebuffer:
 * - Better performance with GPU acceleration
 * - Proper vsync support
 * - Multiple display support
 * - Modern display pipeline
 *
 * Features:
 * - Direct DRM/KMS access via /dev/dri/card0
 * - Touch input via libinput (preferred) or evdev
 * - Automatic display mode detection
 *
 * Requirements:
 * - /dev/dri/card0 must exist and be accessible
 * - User must be in 'video' and 'input' groups
 * - libdrm and libinput libraries
 */
class DisplayBackendDRM : public DisplayBackend {
  public:
    /**
     * @brief Construct DRM backend with default settings
     *
     * Defaults:
     * - DRM device: /dev/dri/card0
     * - Connector: auto-detect first connected
     */
    DisplayBackendDRM();

    /**
     * @brief Construct DRM backend with custom device path
     *
     * @param drm_device Path to DRM device (e.g., "/dev/dri/card0")
     */
    explicit DisplayBackendDRM(const std::string& drm_device);

    ~DisplayBackendDRM() override;

    // Display creation
    lv_display_t* create_display(int width, int height) override;

    // Input device creation
    lv_indev_t* create_input_pointer() override;
    lv_indev_t* create_input_keyboard() override;

    // Display rotation via DRM plane property
    void set_display_rotation(lv_display_rotation_t rot, int phys_w, int phys_h) override;

    /// Check if DRM plane supports hardware rotation for the given angle
    bool supports_hardware_rotation(lv_display_rotation_t rot) const override;

    // Backend info
    DisplayBackendType type() const override {
        return DisplayBackendType::DRM;
    }
    const char* name() const override {
        return "Linux DRM/KMS";
    }
    bool is_available() const override;
    DetectedResolution detect_resolution() const override;

    // Framebuffer operations
    bool clear_framebuffer(uint32_t color) override;

    // Configuration
    void set_drm_device(const std::string& path) {
        drm_device_ = path;
    }

    /// Whether GPU-accelerated rendering (EGL/OpenGL ES) is active
    bool is_gpu_accelerated() const {
        return using_egl_;
    }

    // Touch calibration
    bool set_calibration(const helix::TouchCalibration& cal) override;
    helix::TouchCalibration get_calibration() const override { return calibration_; }
    bool needs_touch_calibration() const override { return needs_calibration_; }
    void disable_affine_calibration() override;
    void enable_affine_calibration() override;

  private:
    /// Switch VT to KD_GRAPHICS so fbcon stops painting over DRM output.
    /// Required on kernel 6.x where sun4i-drm registers DRM fbdev emulation.
    void suppress_console();
    void restore_console();

    std::string drm_device_;
    lv_display_t* display_ = nullptr;
    lv_indev_t* pointer_ = nullptr;
    lv_indev_t* mouse_ = nullptr;
    lv_indev_t* keyboard_ = nullptr;
    int tty_fd_ = -1;           ///< TTY fd for KD_GRAPHICS console suppression
    bool using_egl_ = false;    ///< Track if GPU-accelerated path is active
    helix::TouchCalibration calibration_;
    helix::CalibrationContext calibration_context_;
    bool needs_calibration_ = false;
    int screen_width_ = 0;
    int screen_height_ = 0;

};

#endif // HELIX_DISPLAY_DRM
