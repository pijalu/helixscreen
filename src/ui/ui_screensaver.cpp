// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>

// Sprite asset paths
static constexpr const char* TOASTER_FRAMES[] = {
    "A:assets/images/screensaver/toaster_0.png",
    "A:assets/images/screensaver/toaster_1.png",
    "A:assets/images/screensaver/toaster_2.png",
    "A:assets/images/screensaver/toaster_3.png",
};
static constexpr const char* TOAST_IMG = "A:assets/images/screensaver/toast.png";
static constexpr int NUM_TOASTER_FRAMES =
    sizeof(TOASTER_FRAMES) / sizeof(TOASTER_FRAMES[0]);

// CSS reference: translate(-1600px, 1600px) — fixed travel distance
static constexpr int FLIGHT_DISTANCE = 1600;

// CSS reference: flap .2s steps(4) infinite alternate
// 200ms / 4 steps = 50ms per frame
static constexpr uint32_t FLAP_PERIOD_MS = 50;

// Object definition matching the exact CSS classes and positions.
// CSS uses right/top percentages. Negative right = off-screen to the right.
// Negative top = off-screen above.
//
// Timing classes from CSS:
//   t1: 10s,  0s delay, alternate
//   t2: 16s,  0s delay, alternate-reverse
//   t3: 24s,  0s delay, alternate
//   t4: 10s,  5s delay, alternate
//   t5: 24s,  4s delay, alternate-reverse
//   t6: 24s,  8s delay, alternate
//   t7: 24s, 12s delay, alternate-reverse
//   t8: 24s, 16s delay, alternate
//   t9: 24s, 20s delay, alternate-reverse
//   tst1: 10s, 0s delay (toast)
//   tst2: 16s, 0s delay (toast)
//   tst3: 24s, 0s delay (toast)
//   tst4: 24s, 12s delay (toast)

struct ObjectDef {
    float right_pct;  // CSS right percentage (negative = off-screen right)
    float top_pct;    // CSS top percentage (negative = off-screen above)
    bool is_toaster;
    bool reverse_flap; // alternate-reverse direction
    int fly_ms;        // flight animation duration
    int delay_ms;      // flight start delay
};

// Exact objects from the CSS HTML, in order
static constexpr ObjectDef OBJECTS[] = {
    // First group
    {-0.02f, -0.17f, true,  false, 10000, 0},      // t1 p6
    { 0.10f, -0.19f, true,  false, 24000, 0},      // t3 p7
    { 0.20f, -0.18f, false, false, 10000, 0},      // tst1 p8
    { 0.30f, -0.20f, true,  false, 24000, 0},      // t3 p9
    { 0.50f, -0.18f, true,  false, 10000, 0},      // t1 p11
    { 0.60f, -0.20f, true,  false, 24000, 0},      // t3 p12
    {-0.17f,  0.10f, true,  true,  16000, 0},      // t2 p13
    {-0.19f,  0.20f, false, false, 24000, 0},      // tst3 p14
    {-0.23f,  0.50f, false, false, 16000, 0},      // tst2 p16
    {-0.25f,  0.70f, true,  false, 10000, 0},      // t1 p17
    { 0.10f, -0.20f, false, false, 16000, 0},      // tst2 p19
    { 0.20f, -0.36f, false, false, 24000, 0},      // tst3 p20
    { 0.30f, -0.24f, true,  true,  16000, 0},      // t2 p21
    {-0.26f,  0.10f, false, false, 10000, 0},      // tst1 p24
    { 0.40f, -0.33f, true,  false, 10000, 0},      // t1 p22
    {-0.29f,  0.50f, false, false, 16000, 0},      // tst2 p26
    { 0.10f, -0.56f, true,  false, 10000, 0},      // t1 p28
    { 0.30f, -0.60f, false, false, 16000, 0},      // tst2 p30
    {-0.46f,  0.10f, true,  true,  16000, 0},      // t2 p31
    {-0.56f,  0.20f, true,  false, 10000, 0},      // t1 p32
    {-0.49f,  0.30f, false, false, 24000, 0},      // tst3 p33

    // Wave 1: t4 (fast delayed) — 10s, 5s delay
    { 0.00f, -0.46f, true,  false, 10000, 5000},   // t4 p27
    { 0.40f, -0.21f, true,  false, 10000, 5000},   // t4 p10
    {-0.36f,  0.30f, true,  false, 10000, 5000},   // t4 p25
    { 0.20f, -0.49f, true,  false, 10000, 5000},   // t4 p29

    // Wave 2: t5 — 24s, 4s delay, alternate-reverse
    {-0.21f,  0.30f, true,  true,  24000, 4000},   // t5 p15
    { 0.00f, -0.26f, true,  true,  24000, 4000},   // t5 p18
    { 0.40f, -0.33f, true,  true,  24000, 4000},   // t5 p22

    // Wave 3: t6 — 24s, 8s delay, alternate
    {-0.02f, -0.17f, true,  false, 24000, 8000},   // t6 p6
    { 0.50f, -0.18f, true,  false, 24000, 8000},   // t6 p11
    {-0.21f,  0.30f, true,  false, 24000, 8000},   // t6 p15
    { 0.10f, -0.20f, true,  false, 24000, 8000},   // t6 p19
    { 0.60f, -0.40f, true,  false, 24000, 8000},   // t6 p23

    // Delayed toast: tst4 — 24s, 12s delay
    { 0.40f, -0.21f, false, false, 24000, 12000},  // tst4 p10
    { 0.60f, -0.40f, false, false, 24000, 12000},  // tst4 p23
    {-0.21f,  0.30f, false, false, 24000, 12000},  // tst4 p15

    // Wave 4: t7 — 24s, 12s delay, alternate-reverse
    { 0.10f, -0.19f, true,  true,  24000, 12000},  // t7 p7
    { 0.60f, -0.20f, true,  true,  24000, 12000},  // t7 p12
    {-0.23f,  0.50f, true,  true,  24000, 12000},  // t7 p16
    { 0.20f, -0.36f, true,  true,  24000, 12000},  // t7 p20
    {-0.26f,  0.10f, true,  true,  24000, 12000},  // t7 p24

    // Wave 5: t8 — 24s, 16s delay, alternate
    { 0.20f, -0.18f, true,  false, 24000, 16000},  // t8 p8
    {-0.17f,  0.10f, true,  false, 24000, 16000},  // t8 p13
    {-0.25f,  0.70f, true,  false, 24000, 16000},  // t8 p17
    {-0.36f,  0.30f, true,  false, 24000, 16000},  // t8 p25

    // Wave 6: t9 — 24s, 20s delay, alternate-reverse
    {-0.19f,  0.20f, true,  true,  24000, 20000},  // t9 p14
    { 0.00f, -0.26f, true,  true,  24000, 20000},  // t9 p18
    { 0.30f, -0.24f, true,  true,  24000, 20000},  // t9 p21
    {-0.29f,  0.50f, true,  true,  24000, 20000},  // t9 p26
};
static constexpr int NUM_OBJECTS = sizeof(OBJECTS) / sizeof(OBJECTS[0]);

int FlyingToasterScreensaver::get_scale_factor() const {
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return 256; // 1x in LVGL 256 = 100%
    int w = lv_display_get_horizontal_resolution(disp);
    return (w > 800) ? 512 : 256; // 2x on larger displays
}

void FlyingToasterScreensaver::start() {
    if (m_active) {
        spdlog::debug("[Screensaver] Already active, ignoring start()");
        return;
    }

    spdlog::info("[Screensaver] Starting flying toasters");

    create_overlay();
    spawn_objects();

    // Start wing flap timer (CSS: flap .2s steps(4) = 50ms per frame)
    m_flap_timer = lv_timer_create(flap_timer_cb, FLAP_PERIOD_MS, this);

    m_active = true;
}

void FlyingToasterScreensaver::stop() {
    if (!m_active) {
        return;
    }

    spdlog::info("[Screensaver] Stopping flying toasters");

    // Stop flap timer
    if (m_flap_timer) {
        lv_timer_delete(m_flap_timer);
        m_flap_timer = nullptr;
    }

    // Clear object list (LVGL objects are children of overlay, deleted with it)
    m_objects.clear();

    // Delete overlay (deletes all children too)
    if (m_overlay) {
        lv_obj_delete(m_overlay);
    }
    m_overlay = nullptr;

    m_active = false;
}

void FlyingToasterScreensaver::create_overlay() {
    m_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(m_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(m_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m_overlay, 0, 0);
    lv_obj_set_style_pad_all(m_overlay, 0, 0);
    lv_obj_set_style_radius(m_overlay, 0, 0);
    // Clickable to absorb wake touch (prevents it from triggering underlying UI)
    // LVGL still registers the activity for inactivity tracking
    lv_obj_add_flag(m_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m_overlay, LV_OBJ_FLAG_SCROLLABLE);
}

void FlyingToasterScreensaver::spawn_objects() {
    lv_display_t* disp = lv_display_get_default();
    if (!disp) return;

    int screen_w = lv_display_get_horizontal_resolution(disp);
    int screen_h = lv_display_get_vertical_resolution(disp);
    int obj_size = 64;
    int scale = get_scale_factor();
    if (scale != 256) {
        obj_size = obj_size * scale / 256;
    }

    m_objects.reserve(NUM_OBJECTS);

    for (int i = 0; i < NUM_OBJECTS; i++) {
        const auto& def = OBJECTS[i];

        // Convert CSS right/top percentages to LVGL left/top coordinates.
        // CSS: right: R% means element's right edge is R% from container right.
        //   Negative right = off-screen to the right.
        //   left = screen_w * (1.0 - right_pct) - obj_size
        // CSS: top: T% means element's top edge is T% from container top.
        //   Negative top = above the viewport.
        //   y = screen_h * top_pct
        int start_x = static_cast<int>(screen_w * (1.0f - def.right_pct)) - obj_size;
        int start_y = static_cast<int>(screen_h * def.top_pct);

        create_flying_object(start_x, start_y, def.is_toaster,
                             def.reverse_flap, def.fly_ms, def.delay_ms);
    }

    spdlog::debug("[Screensaver] Spawned {} flying objects ({}x{} screen, {}px sprites)",
                  m_objects.size(), screen_w, screen_h, obj_size);
}

void FlyingToasterScreensaver::create_flying_object(
    int start_x, int start_y, bool is_toaster,
    bool reverse_flap, int speed_ms, int delay_ms) {

    if (!m_overlay) return;

    lv_obj_t* img = lv_image_create(m_overlay);

    // Set initial image
    if (is_toaster) {
        lv_image_set_src(img, TOASTER_FRAMES[reverse_flap ? 2 : 0]);
    } else {
        lv_image_set_src(img, TOAST_IMG);
    }

    // Scale on larger displays
    int scale = get_scale_factor();
    if (scale != 256) {
        lv_image_set_scale(img, scale);
    }

    // Position absolutely (floating, out of any layout)
    lv_obj_add_flag(img, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(img, start_x, start_y);

    uint8_t initial_frame = reverse_flap ? 2 : 0;
    FlyingObject obj{img, is_toaster, reverse_flap, speed_ms, 0, initial_frame, true};
    m_objects.push_back(obj);

    // Pass explicit start coords — lv_obj_get_x/y returns 0 before first layout pass
    animate_flight(m_objects.back(), start_x, start_y, speed_ms, delay_ms);
}

void FlyingToasterScreensaver::animate_flight(FlyingObject& obj,
                                               int start_x, int start_y,
                                               int speed_ms, int delay_ms) {
    // CSS: translate(-1600px, 1600px) — fixed distance, diagonal top-right to bottom-left
    int end_x = start_x - FLIGHT_DISTANCE;
    int end_y = start_y + FLIGHT_DISTANCE;

    // X animation: right to left
    lv_anim_t anim_x;
    lv_anim_init(&anim_x);
    lv_anim_set_var(&anim_x, obj.img);
    lv_anim_set_values(&anim_x, start_x, end_x);
    lv_anim_set_duration(&anim_x, speed_ms);
    lv_anim_set_delay(&anim_x, delay_ms);
    lv_anim_set_path_cb(&anim_x, lv_anim_path_linear);
    lv_anim_set_repeat_count(&anim_x, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim_x, [](void* var, int32_t val) {
        lv_obj_set_x(static_cast<lv_obj_t*>(var), val);
    });
    lv_anim_start(&anim_x);

    // Y animation: top to bottom (diagonal flight)
    lv_anim_t anim_y;
    lv_anim_init(&anim_y);
    lv_anim_set_var(&anim_y, obj.img);
    lv_anim_set_values(&anim_y, start_y, end_y);
    lv_anim_set_duration(&anim_y, speed_ms);
    lv_anim_set_delay(&anim_y, delay_ms);
    lv_anim_set_path_cb(&anim_y, lv_anim_path_linear);
    lv_anim_set_repeat_count(&anim_y, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim_y, [](void* var, int32_t val) {
        lv_obj_set_y(static_cast<lv_obj_t*>(var), val);
    });
    lv_anim_start(&anim_y);
}

void FlyingToasterScreensaver::flap_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<FlyingToasterScreensaver*>(lv_timer_get_user_data(timer));
    if (!self || !self->m_active) return;

    // Per-object flap rate: slower flight = slower wing flap.
    // 10s flight → every tick (50ms/frame), 16s → every 2 ticks, 24s → every 3 ticks.
    for (auto& obj : self->m_objects) {
        if (!obj.is_toaster || !obj.img) continue;

        int ticks_per_frame = std::max(1, obj.fly_ms / 10000);
        obj.flap_counter++;
        if (obj.flap_counter < ticks_per_frame) continue;
        obj.flap_counter = 0;

        // Advance this toaster's own frame: 0→1→2→3→2→1→0 (ping-pong)
        if (obj.flap_forward) {
            obj.flap_frame++;
            if (obj.flap_frame >= NUM_TOASTER_FRAMES - 1) {
                obj.flap_forward = false;
            }
        } else {
            if (obj.flap_frame == 0) {
                obj.flap_forward = true;
            } else {
                obj.flap_frame--;
            }
        }

        lv_image_set_src(obj.img, TOASTER_FRAMES[obj.flap_frame]);
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
