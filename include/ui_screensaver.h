// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"

#include <lvgl.h>

#include <cstdint>
#include <vector>

/**
 * @brief Flying Toasters screensaver (After Dark, 1989)
 *
 * Replaces the dim phase when enabled: after inactivity timeout, toasters
 * and toast fly diagonally across a black screen. Touch wakes back to UI.
 *
 * Lifecycle:
 *   start()  — Create black overlay on lv_layer_top(), spawn objects, start animations
 *   stop()   — Delete everything, clean shutdown
 *   is_active() — Check if screensaver is currently running
 *
 * All animation (flight + wing flap) is driven by a single lv_timer at ~30fps
 * to minimize CPU usage. Positions are computed from elapsed time rather than
 * using per-object LVGL animations.
 */
class FlyingToasterScreensaver : public Screensaver {
  public:
    FlyingToasterScreensaver() = default;
    ~FlyingToasterScreensaver() override = default;

    FlyingToasterScreensaver(const FlyingToasterScreensaver&) = delete;
    FlyingToasterScreensaver& operator=(const FlyingToasterScreensaver&) = delete;

    /** @brief Start the screensaver (creates overlay, spawns objects, starts animations) */
    void start() override;

    /** @brief Stop the screensaver (clean shutdown, deletes everything) */
    void stop() override;

    /** @brief Check if screensaver is currently active */
    bool is_active() const override { return m_active; }

    /** @brief Return FLYING_TOASTERS type */
    ScreensaverType type() const override { return ScreensaverType::FLYING_TOASTERS; }

  private:

    struct FlyingObject {
        lv_obj_t* img;
        bool is_toaster;
        int16_t start_x;
        int16_t start_y;
        int fly_ms;
        int delay_ms;
        // Flap state (toasters only)
        uint8_t flap_frame;
        bool flap_forward;
        int8_t flap_counter;
        int8_t ticks_per_flap; // pre-computed: ticks between frame changes
    };

    /** @brief Create the full-screen black overlay */
    void create_overlay();

    /** @brief Spawn all flying objects with staggered positions and delays */
    void spawn_objects();

    /** @brief Create a single flying object */
    void create_flying_object(int start_x, int start_y, bool is_toaster,
                              bool reverse_flap, int speed_ms, int delay_ms);

    /** @brief Single timer callback driving all animation (flight + flap) */
    static void tick_cb(lv_timer_t* timer);

    /** @brief Get image scale factor based on screen width */
    int get_scale_factor() const;

    bool m_active = false;
    lv_obj_t* m_overlay = nullptr;
    std::vector<FlyingObject> m_objects;
    lv_timer_t* m_tick_timer = nullptr;
    uint32_t m_elapsed_ms = 0;
};

#endif // HELIX_ENABLE_SCREENSAVER
