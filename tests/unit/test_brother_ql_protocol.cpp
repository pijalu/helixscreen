// SPDX-License-Identifier: GPL-3.0-or-later

#include "../catch_amalgamated.hpp"
#include "brother_ql_protocol.h"

using namespace helix;
using namespace helix::label;

TEST_CASE("Brother QL protocol - invalidation header", "[label][brother]") {
    LabelBitmap bitmap(696, 200);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    auto data = brother_ql_build_raster(bitmap, size);

    // First 200 bytes must be 0x00 (invalidation)
    REQUIRE(data.size() > 200);
    for (int i = 0; i < 200; i++) {
        REQUIRE(data[i] == 0x00);
    }
}

TEST_CASE("Brother QL protocol - init sequence", "[label][brother]") {
    LabelBitmap bitmap(696, 200);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    auto data = brother_ql_build_raster(bitmap, size);

    // After 200 bytes of invalidation: ESC @ (initialize)
    REQUIRE(data[200] == 0x1B);
    REQUIRE(data[201] == 0x40);
}

TEST_CASE("Brother QL protocol - raster mode command", "[label][brother]") {
    LabelBitmap bitmap(696, 200);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    auto data = brother_ql_build_raster(bitmap, size);

    // ESC i a 01 (raster mode) follows ESC @
    REQUIRE(data[202] == 0x1B);
    REQUIRE(data[203] == 0x69);
    REQUIRE(data[204] == 0x61);
    REQUIRE(data[205] == 0x01);
}

TEST_CASE("Brother QL protocol - media info", "[label][brother]") {
    LabelBitmap bitmap(696, 200);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    auto data = brother_ql_build_raster(bitmap, size);

    // ESC i z at offset 206
    REQUIRE(data[206] == 0x1B);
    REQUIRE(data[207] == 0x69);
    REQUIRE(data[208] == 0x7A);
    REQUIRE(data[209] == 0x86);  // validity flags
    REQUIRE(data[210] == 0x0A);  // media type
    REQUIRE(data[211] == 62);    // width mm
    REQUIRE(data[212] == 0);     // length mm (continuous)
}

TEST_CASE("Brother QL protocol - print command at end", "[label][brother]") {
    LabelBitmap bitmap(696, 200);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    auto data = brother_ql_build_raster(bitmap, size);

    // Last byte is 0x1A (print command)
    REQUIRE(data.back() == 0x1A);
}

TEST_CASE("Brother QL protocol - horizontal flip verification", "[label][brother]") {
    LabelBitmap bitmap(696, 1);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    // Set pixel at x=0, y=0 (leftmost)
    bitmap.set_pixel(0, 0, true);

    auto data = brother_ql_build_raster(bitmap, size);

    // Find the raster data row (0x67 0x00 0x5A marker)
    // Since we have one non-blank row, look for 0x67
    bool found_raster = false;
    size_t raster_start = 0;
    for (size_t i = 0; i + 2 < data.size(); i++) {
        if (data[i] == 0x67 && data[i + 1] == 0x00 &&
            data[i + 2] == static_cast<uint8_t>(BROTHER_QL_RASTER_ROW_BYTES)) {
            found_raster = true;
            raster_start = i + 3; // skip header, data starts here
            break;
        }
    }
    REQUIRE(found_raster);

    // After horizontal flip, pixel at x=0 should appear at x=695 (width-1)
    // x=695 is in byte 695/8 = 86, bit 7-(695%8) = 7-7 = 0
    // But with left padding: label is 87 bytes, left pad = 90-87 = 3
    int label_byte_width = (696 + 7) / 8; // 87
    int left_pad = BROTHER_QL_RASTER_ROW_BYTES - label_byte_width; // 3

    // The flipped pixel at dst_x=695 is in the flipped row at byte 695/8=86, bit 0
    // In the output buffer: offset = raster_start + left_pad + 86
    size_t pixel_byte_offset = raster_start + left_pad + 86;
    REQUIRE(pixel_byte_offset < data.size());
    // Bit 0 (7 - (695%8) = 7-7 = 0) should be set
    REQUIRE((data[pixel_byte_offset] & 0x01) != 0);

    // Original position x=0 should NOT be set in the flipped output
    // x=0 in output is at byte 0 of flipped row, bit 7
    size_t orig_byte_offset = raster_start + left_pad + 0;
    // bit 7 should NOT be set (the pixel was flipped away)
    REQUIRE((data[orig_byte_offset] & 0x80) == 0);
}

TEST_CASE("Brother QL protocol - empty bitmap", "[label][brother]") {
    LabelBitmap bitmap(696, 0);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    auto data = brother_ql_build_raster(bitmap, size);

    // Should still have header + print command, just no raster rows
    REQUIRE(data.size() > 200);
    REQUIRE(data.back() == 0x1A);
}

TEST_CASE("Brother QL protocol - deterministic output", "[label][brother]") {
    LabelBitmap bitmap(696, 100);
    LabelSize size{"62mm", 696, 0, 300, 0x0A, 62, 0};

    auto data1 = brother_ql_build_raster(bitmap, size);
    auto data2 = brother_ql_build_raster(bitmap, size);

    REQUIRE(data1 == data2);
}
