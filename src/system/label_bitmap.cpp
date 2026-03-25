// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_bitmap.h"

#include <spdlog/spdlog.h>

#include <algorithm>

extern "C" {
#include "lvgl/src/libs/qrcode/qrcodegen.h"
}

namespace helix {

void LabelBitmap::blit(const LabelBitmap& src, int dst_x, int dst_y) {
    for (int sy = 0; sy < src.height(); sy++) {
        int dy = dst_y + sy;
        if (dy < 0)
            continue;
        if (dy >= height_)
            break;
        for (int sx = 0; sx < src.width(); sx++) {
            int dx = dst_x + sx;
            if (dx < 0)
                continue;
            if (dx >= width_)
                break;
            if (src.get_pixel(sx, sy)) {
                set_pixel(dx, dy, true);
            }
        }
    }
}

LabelBitmap LabelBitmap::rotate_90_cw() const {
    // 90° CW: src(x, y) → dst(height-1-y, x)
    // New dimensions: width=height_, height=width_
    LabelBitmap dst(height_, width_);
    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            if (get_pixel(x, y)) {
                dst.set_pixel(height_ - 1 - y, x, true);
            }
        }
    }
    return dst;
}

LabelBitmap generate_qr_bitmap(const std::string& data, int target_size_px) {
    if (data.empty() || target_size_px <= 0) {
        spdlog::warn("[LabelBitmap] generate_qr_bitmap: invalid parameters");
        return {};
    }

    // Encode QR code with HIGH error correction (30% recovery for logo overlay)
    uint8_t qr_buf[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp_buf[qrcodegen_BUFFER_LEN_MAX];

    bool ok = qrcodegen_encodeText(data.c_str(), tmp_buf, qr_buf, qrcodegen_Ecc_HIGH, 1,
                                   qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    if (!ok) {
        spdlog::error("[LabelBitmap] QR encoding failed for '{}'", data);
        return {};
    }

    int qr_size = qrcodegen_getSize(qr_buf);
    if (qr_size <= 0) {
        spdlog::error("[LabelBitmap] QR code has invalid size");
        return {};
    }

    // Calculate module size — round up to fill the target. The bitmap may be
    // slightly larger than target_size_px; the caller's blit() clips to label bounds.
    int module_px = (target_size_px + qr_size - 1) / qr_size;
    if (module_px < 1)
        module_px = 1;

    int bitmap_size = qr_size * module_px;
    auto bitmap = LabelBitmap::create(bitmap_size, bitmap_size);

    // Render QR modules into bitmap
    for (int qy = 0; qy < qr_size; qy++) {
        for (int qx = 0; qx < qr_size; qx++) {
            if (qrcodegen_getModule(qr_buf, qx, qy)) {
                // Fill module_px × module_px block
                int px_x = qx * module_px;
                int px_y = qy * module_px;
                for (int dy = 0; dy < module_px; dy++) {
                    for (int dx = 0; dx < module_px; dx++) {
                        bitmap.set_pixel(px_x + dx, px_y + dy, true);
                    }
                }
            }
        }
    }

    spdlog::debug("[LabelBitmap] Generated QR bitmap {}x{} (qr_size={}, module={}px) for '{}'",
                  bitmap_size, bitmap_size, qr_size, module_px, data);
    return bitmap;
}

} // namespace helix
