// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_starfield.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

static constexpr int NUM_STARS = 150;
static constexpr uint32_t FRAME_PERIOD_MS = 33; // ~30fps
static constexpr float COLOR_THRESHOLD = 0.35f; // stars closer than this show color

// Opaque black in ARGB8888 format (alpha=0xFF, RGB=0)
static constexpr uint32_t PIXEL_BLACK = 0xFF000000;

// Star color tints — blue dwarfs, red giants, yellow suns, blue-white hot stars
static const uint8_t STAR_TINTS[][3] = {
    {255, 255, 255}, // white (most common)
    {255, 255, 255}, // white
    {255, 255, 255}, // white
    {255, 200, 150}, // warm yellow
    {255, 160, 120}, // orange
    {255, 120, 100}, // red giant
    {150, 180, 255}, // blue dwarf
    {200, 220, 255}, // blue-white
};
static constexpr int NUM_TINTS = sizeof(STAR_TINTS) / sizeof(STAR_TINTS[0]);

void StarfieldScreensaver::start() {
    if (active_) {
        spdlog::debug("[Screensaver] Starfield already active, ignoring start()");
        return;
    }

    spdlog::info("[Screensaver] Starting starfield");

    lv_display_t* disp = lv_display_get_default();
    if (!disp) {
        spdlog::warn("[Screensaver] No display available, cannot start starfield");
        return;
    }

    screen_w_ = lv_display_get_horizontal_resolution(disp);
    screen_h_ = lv_display_get_vertical_resolution(disp);
    cx_ = static_cast<float>(screen_w_) / 2.0f;
    cy_ = static_cast<float>(screen_h_) / 2.0f;
    focal_ = static_cast<float>(screen_w_) / 3.0f;

    // Create black overlay on lv_layer_top() — absorbs touch input
    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // Create canvas as child of overlay
    canvas_ = lv_canvas_create(overlay_);
    lv_obj_set_size(canvas_, screen_w_, screen_h_);
    lv_obj_set_pos(canvas_, 0, 0);

    // Allocate ARGB8888 draw buffer
    size_t buf_size = static_cast<size_t>(screen_w_) * screen_h_ * 4;
    draw_buf_ = static_cast<uint8_t*>(lv_malloc(buf_size));
    if (!draw_buf_) {
        spdlog::error("[Screensaver] Failed to allocate {}KB draw buffer", buf_size / 1024);
        lv_obj_delete(overlay_);
        overlay_ = nullptr;
        canvas_ = nullptr;
        return;
    }

    lv_canvas_set_buffer(canvas_, draw_buf_, screen_w_, screen_h_, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);

    // Seed RNG and initialize stars
    srand(static_cast<unsigned>(time(nullptr)));
    init_stars();

    // Create render timer
    timer_ = lv_timer_create(frame_timer_cb, FRAME_PERIOD_MS, this);

    active_ = true;
    spdlog::debug("[Screensaver] Starfield started ({}x{}, {} stars)", screen_w_, screen_h_, NUM_STARS);
}

void StarfieldScreensaver::stop() {
    if (!active_) {
        return;
    }

    spdlog::info("[Screensaver] Stopping starfield");

    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    // Free draw buffer BEFORE deleting overlay — the canvas (child of overlay)
    // may access the buffer during deletion
    if (draw_buf_) {
        lv_free(draw_buf_);
        draw_buf_ = nullptr;
    }

    if (overlay_) {
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(overlay_);
        overlay_ = nullptr;
        canvas_ = nullptr; // deleted as child of overlay
    }

    stars_.clear();
    active_ = false;
}

static void assign_tint(uint8_t& r, uint8_t& g, uint8_t& b) {
    int idx = rand() % NUM_TINTS;
    r = STAR_TINTS[idx][0];
    g = STAR_TINTS[idx][1];
    b = STAR_TINTS[idx][2];
}

void StarfieldScreensaver::init_stars() {
    stars_.resize(NUM_STARS);
    for (auto& star : stars_) {
        float angle = (static_cast<float>(rand()) / RAND_MAX) * 2.0f * 3.14159265f;
        float radius = 0.1f + (static_cast<float>(rand()) / RAND_MAX) * 0.9f;
        star.x = radius * std::cos(angle);
        star.y = radius * std::sin(angle);
        star.z = 0.01f + (static_cast<float>(rand()) / RAND_MAX) * 0.99f;
        star.speed = 0.008f + (static_cast<float>(rand()) / RAND_MAX) * 0.017f;
        assign_tint(star.tint_r, star.tint_g, star.tint_b);
        star.prev_sx = 0;
        star.prev_sy = 0;
        star.prev_size = 0;
    }
}

void StarfieldScreensaver::recycle_star(Star& star) {
    // Pick random angle + radius so stars fly uniformly in all directions
    float angle = (static_cast<float>(rand()) / RAND_MAX) * 2.0f * 3.14159265f;
    float radius = 0.3f + (static_cast<float>(rand()) / RAND_MAX) * 0.7f;
    star.x = radius * std::cos(angle);
    star.y = radius * std::sin(angle);
    star.z = 1.0f;
    star.speed = 0.008f + (static_cast<float>(rand()) / RAND_MAX) * 0.017f;
    assign_tint(star.tint_r, star.tint_g, star.tint_b);
}

void StarfieldScreensaver::frame_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<StarfieldScreensaver*>(lv_timer_get_user_data(timer));
    if (!self || !self->active_) return;
    self->render_frame();
}

void StarfieldScreensaver::render_frame() {
    if (!canvas_ || !draw_buf_) return;

    auto* pixels = reinterpret_cast<uint32_t*>(draw_buf_);
    int w = screen_w_;
    int h = screen_h_;

    // Erase previous star positions (incremental clear — avoids full-buffer memset)
    for (auto& star : stars_) {
        if (star.prev_size == 0) continue;
        int sx = star.prev_sx;
        int sy = star.prev_sy;
        int sz = star.prev_size;
        for (int dy = 0; dy < sz; dy++) {
            int py = sy + dy;
            if (py < 0 || py >= h) continue;
            int row = py * w;
            for (int dx = 0; dx < sz; dx++) {
                int px = sx + dx;
                if (px >= 0 && px < w) {
                    pixels[row + px] = PIXEL_BLACK;
                }
            }
        }
        star.prev_size = 0;
    }

    // Update and draw stars via direct pixel writes (no LVGL draw API)
    for (auto& star : stars_) {
        // Move star closer
        star.z -= star.speed;

        if (star.z <= 0.01f) {
            recycle_star(star);
            continue;
        }

        // Project to screen coordinates
        float sx = cx_ + (star.x / star.z) * focal_;
        float sy = cy_ + (star.y / star.z) * focal_;

        // Check bounds
        if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
            recycle_star(star);
            continue;
        }

        int isx = static_cast<int>(sx);
        int isy = static_cast<int>(sy);

        // Size: larger when closer (z near 0)
        int size = std::max(1, static_cast<int>(3.0f * (1.0f - star.z)));

        // Brightness: brighter when closer, with minimum floor
        float bright_f = 80.0f + 175.0f * (1.0f - star.z);

        // Close stars show their color tint; distant stars stay white
        uint8_t r, g, b;
        if (star.z < COLOR_THRESHOLD) {
            float tint_mix = (COLOR_THRESHOLD - star.z) / COLOR_THRESHOLD;
            r = static_cast<uint8_t>(bright_f * (1.0f - tint_mix + tint_mix * star.tint_r / 255.0f));
            g = static_cast<uint8_t>(bright_f * (1.0f - tint_mix + tint_mix * star.tint_g / 255.0f));
            b = static_cast<uint8_t>(bright_f * (1.0f - tint_mix + tint_mix * star.tint_b / 255.0f));
        } else {
            r = g = b = static_cast<uint8_t>(bright_f);
        }

        // Write pixel(s) directly to canvas buffer (ARGB8888)
        uint32_t pixel = PIXEL_BLACK | (static_cast<uint32_t>(r) << 16) |
                         (static_cast<uint32_t>(g) << 8) | b;

        for (int dy = 0; dy < size; dy++) {
            int py = isy + dy;
            if (py < 0 || py >= h) continue;
            int row = py * w;
            for (int dx = 0; dx < size; dx++) {
                int px = isx + dx;
                if (px >= 0 && px < w) {
                    pixels[row + px] = pixel;
                }
            }
        }

        // Remember position for next frame's erase pass
        star.prev_sx = static_cast<int16_t>(isx);
        star.prev_sy = static_cast<int16_t>(isy);
        star.prev_size = static_cast<uint8_t>(size);
    }

    // Tell LVGL the canvas content changed
    lv_obj_invalidate(canvas_);
}

#endif // HELIX_ENABLE_SCREENSAVER
