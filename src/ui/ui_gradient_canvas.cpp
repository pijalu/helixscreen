// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_gradient_canvas.h"

#include "ui_error_reporting.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "memory_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace {

// Default gradient colors (matching original thumbnail-gradient-bg.png)
// Diagonal gradient: bright at top-right, dark at bottom-left
// Lightened ~50% from original (80,0) to (123,43) for better visibility
constexpr uint8_t DEFAULT_START_GRAY = 123; // Top-right - brighter
constexpr uint8_t DEFAULT_END_GRAY = 43;    // Bottom-left - darker

// Light theme gradient colors
constexpr uint8_t LIGHT_START_GRAY = 235; // Top-right - brighter
constexpr uint8_t LIGHT_END_GRAY = 188;   // Bottom-left - darker

// Pre-rendered gradient buffer size
// 256x256 on normal devices, 128x128 on constrained (saves 192KB ARGB8888)
static int32_t gradient_buffer_size() {
    static const int32_t size = helix::get_system_memory_info().is_constrained_device() ? 128 : 256;
    return size;
}

// 4x4 Bayer dither matrix (normalized to 0-15)
constexpr uint8_t BAYER_4X4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// User data for gradient configuration
struct GradientData {
    uint8_t start_r, start_g, start_b;
    uint8_t end_r, end_g, end_b;
    bool dither;
    lv_draw_buf_t* draw_buf; // Pre-rendered gradient buffer
};

/**
 * @brief Extract RGB from LVGL color string
 */
static void parse_color_to_rgb(const char* color_str, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!color_str) {
        r = g = b = 0;
        return;
    }
    if (color_str[0] == '#')
        color_str++;
    lv_color_t color = lv_xml_to_color(color_str);
    lv_color32_t c32 = lv_color_to_32(color, LV_OPA_COVER);
    r = c32.red;
    g = c32.green;
    b = c32.blue;
}

/**
 * @brief Apply Bayer dithering threshold tuned for RGB565
 *
 * RGB565 has 5-6-5 bits per channel, meaning:
 * - Red/Blue: 32 levels (step = 8 in 8-bit)
 * - Green: 64 levels (step = 4 in 8-bit)
 *
 * We use ±12 range for visible effect without excessive noise.
 */
static inline int16_t bayer_threshold(int32_t x, int32_t y) {
    // Bayer value 0-15, map to -12..+11 for RGB565 quantization dithering
    int16_t bayer_val = BAYER_4X4[y & 3][x & 3];
    return (bayer_val * 24 / 16) - 12; // Scale to ±12 range
}

/**
 * @brief Render diagonal gradient into an ARGB8888 draw buffer
 *
 * Renders bright at top-right, dark at bottom-left.
 * Uses ordered dithering for smooth appearance on 16-bit displays.
 * Supports arbitrary (non-square) buffer dimensions.
 */
static void render_gradient_to_buf(lv_draw_buf_t* buf, uint8_t start_r, uint8_t start_g,
                                   uint8_t start_b, uint8_t end_r, uint8_t end_g, uint8_t end_b,
                                   bool dither) {
    if (!buf || !buf->data)
        return;

    uint8_t* buf_data = buf->data;
    uint32_t stride = buf->header.stride;
    int32_t w = buf->header.w;
    int32_t h = buf->header.h;

    // For diagonal gradient (top-right to bottom-left), max distance is (w-1)+(h-1)
    float max_dist = static_cast<float>((w - 1) + (h - 1));
    if (max_dist < 1.0f)
        max_dist = 1.0f;

    int16_t dr = end_r - start_r;
    int16_t dg = end_g - start_g;
    int16_t db = end_b - start_b;

    for (int32_t y = 0; y < h; y++) {
        lv_color32_t* row =
            reinterpret_cast<lv_color32_t*>(buf_data + static_cast<uint32_t>(y) * stride);

        for (int32_t x = 0; x < w; x++) {
            // Diagonal interpolation: top-right (bright) to bottom-left (dark)
            float t = static_cast<float>((w - 1 - x) + y) / max_dist;

            int16_t r = start_r + static_cast<int16_t>(t * dr);
            int16_t g = start_g + static_cast<int16_t>(t * dg);
            int16_t b = start_b + static_cast<int16_t>(t * db);

            if (dither) {
                int16_t threshold = bayer_threshold(x, y);
                r = std::clamp<int16_t>(r + threshold, 0, 255);
                g = std::clamp<int16_t>(g + threshold, 0, 255);
                b = std::clamp<int16_t>(b + threshold, 0, 255);
            }

            row[x].red = static_cast<uint8_t>(r);
            row[x].green = static_cast<uint8_t>(g);
            row[x].blue = static_cast<uint8_t>(b);
            row[x].alpha = 255;
        }
    }
}

/// Convenience wrapper for GradientData (used by the XML widget)
static void render_gradient_buffer(GradientData* data) {
    if (!data || !data->draw_buf)
        return;
    render_gradient_to_buf(data->draw_buf, data->start_r, data->start_g, data->start_b, data->end_r,
                           data->end_g, data->end_b, data->dither);
}

/**
 * @brief Cleanup handler - free gradient data and buffer on delete
 */
static void gradient_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    std::unique_ptr<GradientData> data(static_cast<GradientData*>(lv_obj_get_user_data(obj)));
    lv_obj_set_user_data(obj, nullptr);
    if (data) {
        if (data->draw_buf) {
            lv_draw_buf_destroy(data->draw_buf);
            data->draw_buf = nullptr;
        }
        // data automatically freed
    }
}

/**
 * @brief XML create handler - creates image widget with pre-rendered gradient
 */
static void* ui_gradient_canvas_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* img = lv_image_create(static_cast<lv_obj_t*>(parent));

    if (!img) {
        LOG_ERROR_INTERNAL("[GradientCanvas] Failed to create image object");
        return nullptr;
    }

    // Initialize gradient data with defaults
    auto data_ptr = std::make_unique<GradientData>();
    data_ptr->start_r = DEFAULT_START_GRAY;
    data_ptr->start_g = DEFAULT_START_GRAY;
    data_ptr->start_b = DEFAULT_START_GRAY;
    data_ptr->end_r = DEFAULT_END_GRAY;
    data_ptr->end_g = DEFAULT_END_GRAY;
    data_ptr->end_b = DEFAULT_END_GRAY;
    data_ptr->dither = true;
    data_ptr->draw_buf = nullptr;

    // Create draw buffer for gradient
    // Must stay ARGB8888 — render_gradient_buffer() writes lv_color32_t pixels directly
    data_ptr->draw_buf = lv_draw_buf_create(gradient_buffer_size(), gradient_buffer_size(),
                                            LV_COLOR_FORMAT_ARGB8888, 0);

    if (!data_ptr->draw_buf) {
        LOG_ERROR_INTERNAL("[GradientCanvas] Failed to create draw buffer");
        helix::ui::safe_delete(img);
        return nullptr;
    }

    // Render initial gradient (must be done before release)
    render_gradient_buffer(data_ptr.get());

    // Set image source to our buffer
    lv_image_set_src(img, data_ptr->draw_buf);

    lv_obj_set_user_data(img, data_ptr.release());

    // Configure image to fill widget area (scales to cover, clips overflow)
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);

    // Remove default styling
    lv_obj_set_style_border_width(img, 0, 0);
    lv_obj_set_style_pad_all(img, 0, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);

    // Cleanup handler
    lv_obj_add_event_cb(img, gradient_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::trace("[GradientCanvas] Created gradient ({}x{} buffer)", gradient_buffer_size(),
                  gradient_buffer_size());
    return static_cast<void*>(img);
}

/**
 * @brief XML apply handler - processes custom attributes
 */
static void ui_gradient_canvas_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);

    if (!obj) {
        LOG_ERROR_INTERNAL("[GradientCanvas] NULL object in xml_apply");
        return;
    }

    GradientData* data = static_cast<GradientData*>(lv_obj_get_user_data(obj));
    bool colors_changed = false;

    // Parse custom attributes
    for (int i = 0; attrs[i] && attrs[i + 1]; i += 2) {
        if (strcmp(attrs[i], "start_color") == 0) {
            if (data) {
                parse_color_to_rgb(attrs[i + 1], data->start_r, data->start_g, data->start_b);
                colors_changed = true;
            }
        } else if (strcmp(attrs[i], "end_color") == 0) {
            if (data) {
                parse_color_to_rgb(attrs[i + 1], data->end_r, data->end_g, data->end_b);
                colors_changed = true;
            }
        } else if (strcmp(attrs[i], "dither") == 0) {
            if (data) {
                data->dither = (strcmp(attrs[i + 1], "true") == 0);
                colors_changed = true;
            }
        }
    }

    // Apply standard lv_obj properties (size, position, etc.)
    lv_xml_obj_apply(state, attrs);

    // Re-render gradient if colors changed
    if (colors_changed && data) {
        render_gradient_buffer(data);
        // Note: No explicit invalidate needed - LVGL will redraw when buffer is accessed
        // Calling lv_obj_invalidate() here can trigger assertion if called during render

        spdlog::trace("[GradientCanvas] Applied (start=#{:02X}{:02X}{:02X}, "
                      "end=#{:02X}{:02X}{:02X}, dither={})",
                      data->start_r, data->start_g, data->start_b, data->end_r, data->end_g,
                      data->end_b, data->dither);
    }
}

} // anonymous namespace

void ui_gradient_canvas_register(void) {
    lv_xml_register_widget("ui_gradient_canvas", ui_gradient_canvas_xml_create,
                           ui_gradient_canvas_xml_apply);
    spdlog::trace("[GradientCanvas] Registered <ui_gradient_canvas> widget");
}

void ui_gradient_canvas_redraw(lv_obj_t* obj) {
    if (!obj)
        return;
    GradientData* data = static_cast<GradientData*>(lv_obj_get_user_data(obj));
    if (data) {
        render_gradient_buffer(data);
        // Defer invalidation to avoid calling during render phase
        helix::ui::async_call(
            obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
    }
}

void ui_gradient_canvas_set_dither(lv_obj_t* obj, bool enable) {
    if (!obj)
        return;
    GradientData* data = static_cast<GradientData*>(lv_obj_get_user_data(obj));
    if (data && data->dither != enable) {
        data->dither = enable;
        render_gradient_buffer(data);
        // Defer invalidation to avoid calling during render phase
        helix::ui::async_call(
            obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
    }
}

/**
 * @brief Apply rounded corner alpha mask to an ARGB8888 draw buffer
 *
 * Sets alpha to 0 for pixels outside the rounded rectangle, with 1px
 * anti-aliased fringe for smooth edges. Workaround for LVGL's style_clip_corner
 * not reliably clipping children on SDL/GLES backends.
 */
static void apply_corner_radius_mask(lv_draw_buf_t* buf, int32_t radius) {
    if (!buf || !buf->data || radius <= 0)
        return;

    uint8_t* buf_data = buf->data;
    uint32_t stride = buf->header.stride;
    int32_t w = buf->header.w;
    int32_t h = buf->header.h;

    radius = std::min(radius, std::min(w, h) / 2);

    for (int32_t cy = 0; cy < radius; cy++) {
        auto* top_row =
            reinterpret_cast<lv_color32_t*>(buf_data + static_cast<uint32_t>(cy) * stride);
        auto* bot_row =
            reinterpret_cast<lv_color32_t*>(buf_data + static_cast<uint32_t>(h - 1 - cy) * stride);

        for (int32_t cx = 0; cx < radius; cx++) {
            float dx = static_cast<float>(radius - cx) - 0.5f;
            float dy = static_cast<float>(radius - cy) - 0.5f;
            float dist = sqrtf(dx * dx + dy * dy);
            float r = static_cast<float>(radius);

            if (dist > r) {
                top_row[cx].alpha = 0;
                top_row[w - 1 - cx].alpha = 0;
                bot_row[cx].alpha = 0;
                bot_row[w - 1 - cx].alpha = 0;
            } else if (dist > r - 1.0f) {
                uint8_t aa = static_cast<uint8_t>((r - dist) * 255.0f);
                top_row[cx].alpha = std::min(top_row[cx].alpha, aa);
                top_row[w - 1 - cx].alpha = std::min(top_row[w - 1 - cx].alpha, aa);
                bot_row[cx].alpha = std::min(bot_row[cx].alpha, aa);
                bot_row[w - 1 - cx].alpha = std::min(bot_row[w - 1 - cx].alpha, aa);
            }
        }
    }
}

lv_draw_buf_t* ui_gradient_canvas_create_buf(int32_t width, int32_t height, bool dark_mode,
                                             int32_t radius) {
    if (width <= 0 || height <= 0)
        return nullptr;

    lv_draw_buf_t* buf = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!buf) {
        spdlog::error("[GradientCanvas] Failed to allocate {}x{} draw buffer", width, height);
        return nullptr;
    }

    uint8_t start_gray = dark_mode ? DEFAULT_START_GRAY : LIGHT_START_GRAY;
    uint8_t end_gray = dark_mode ? DEFAULT_END_GRAY : LIGHT_END_GRAY;

    render_gradient_to_buf(buf, start_gray, start_gray, start_gray, end_gray, end_gray, end_gray,
                           true);

    if (radius > 0) {
        apply_corner_radius_mask(buf, radius);
    }

    spdlog::debug("[GradientCanvas] Created shared {}x{} gradient buffer ({}, radius={})", width,
                  height, dark_mode ? "dark" : "light", radius);
    return buf;
}
