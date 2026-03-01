// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"

#include <lvgl.h>

#include <vector>

/**
 * @brief Windows 95-style Starfield screensaver
 *
 * Stars fly outward from the center of the screen. Each star starts small
 * and dim near the center, growing larger and brighter as it approaches
 * the edges. Canvas-based rendering at ~30fps.
 */
class StarfieldScreensaver : public Screensaver {
  public:
    StarfieldScreensaver() = default;
    ~StarfieldScreensaver() override = default;

    void start() override;
    void stop() override;
    bool is_active() const override { return active_; }
    ScreensaverType type() const override { return ScreensaverType::STARFIELD; }

  private:
    struct Star {
        float x;         // normalized position (-1..1)
        float y;         // normalized position (-1..1)
        float z;         // depth (0..1, 1=far, approaches 0)
        float speed;     // z decrement per frame
        uint8_t tint_r;  // color tint (assigned at birth, visible when close)
        uint8_t tint_g;
        uint8_t tint_b;
    };

    void init_stars();
    void recycle_star(Star& star);

    static void frame_timer_cb(lv_timer_t* timer);
    void render_frame();

    bool active_ = false;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    // Draw buffer owned by the canvas
    uint8_t* draw_buf_ = nullptr;

    std::vector<Star> stars_;

    // Cached screen dimensions
    int screen_w_ = 0;
    int screen_h_ = 0;
};

#endif // HELIX_ENABLE_SCREENSAVER
