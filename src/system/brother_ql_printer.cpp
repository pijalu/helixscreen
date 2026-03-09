// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "brother_ql_printer.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

namespace helix {

// Brother QL raster row width: 90 bytes = 720 pixels for QL-800/810W/820NWB
static constexpr int RASTER_ROW_BYTES = 90;


struct BrotherQLPrinter::Impl {};

BrotherQLPrinter::BrotherQLPrinter() : impl_(std::make_unique<Impl>()) {}

BrotherQLPrinter::BrotherQLPrinter(std::string host, int port)
    : impl_(std::make_unique<Impl>()), host_(std::move(host)), port_(port) {}

BrotherQLPrinter::~BrotherQLPrinter() = default;

std::string BrotherQLPrinter::name() const {
    return "Brother QL";
}

void BrotherQLPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                              PrintCallback callback) {
    print_label(host_, port_, bitmap, size, std::move(callback));
}

std::vector<LabelSize> BrotherQLPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> BrotherQLPrinter::supported_sizes_static() {
    return {
        {"29mm",     306,    0, 300, 0x0A, 29,  0},
        {"38mm",     413,    0, 300, 0x0A, 38,  0},
        {"62mm",     696,    0, 300, 0x0A, 62,  0},
        {"29x90mm",  306,  991, 300, 0x0B, 29, 90},
        {"62x29mm",  696,  271, 300, 0x0B, 62, 29},
        {"62x100mm", 696, 1164, 300, 0x0B, 62, 100},
    };
}

std::vector<uint8_t> BrotherQLPrinter::build_raster_commands(const LabelBitmap& bitmap,
                                                              const LabelSize& size) {
    std::vector<uint8_t> cmd;
    // Pre-allocate: header ~220 bytes + per-row worst case (3 + 90) * rows + footer
    cmd.reserve(256 + static_cast<size_t>(bitmap.height()) * (3 + RASTER_ROW_BYTES) + 1);

    // 1. Invalidate — 200 bytes of 0x00
    cmd.insert(cmd.end(), 200, 0x00);

    // 2. Initialize — ESC @
    cmd.push_back(0x1B);
    cmd.push_back(0x40);

    // 3. Switch to raster mode — ESC i a 01
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x61);
    cmd.push_back(0x01);

    // 4. Set media and quality — ESC i z
    int page_length = bitmap.height();
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x7A);
    cmd.push_back(0x86);             // Validity flags: media type + width + length valid
    cmd.push_back(size.media_type);  // Media type
    cmd.push_back(size.width_mm);    // Width in mm
    cmd.push_back(size.length_mm);   // Length in mm
    // Page length as little-endian 32-bit
    cmd.push_back(static_cast<uint8_t>(page_length & 0xFF));
    cmd.push_back(static_cast<uint8_t>((page_length >> 8) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((page_length >> 16) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((page_length >> 24) & 0xFF));
    cmd.push_back(0x00); // Page number = 0
    cmd.push_back(0x00); // Auto-cut flag (set separately)

    // 5. Set auto-cut on — ESC i M
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4D);
    cmd.push_back(0x40);

    // 5a. Set cut-every = 1 label — ESC i A
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x41);
    cmd.push_back(0x01);

    // 6. Expanded mode — ESC i K (auto-cut, no 2-color, 300dpi)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4B);
    cmd.push_back(0x08); // bit 3 = auto-cut

    // 7. Set margins = 0 (no margins for die-cut)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x64);
    cmd.push_back(0x00);
    cmd.push_back(0x00);

    // 8. Disable compression — M 0x00
    cmd.push_back(0x4D);
    cmd.push_back(0x00);

    // 9. Raster data rows
    // Brother QL printers require horizontally flipped image data
    int label_byte_width = (size.width_px + 7) / 8;
    int left_pad = RASTER_ROW_BYTES - label_byte_width;
    if (left_pad < 0) left_pad = 0;

    // Build a horizontally-flipped copy of each row
    std::vector<uint8_t> flipped_row(label_byte_width, 0x00);

    for (int y = 0; y < bitmap.height(); y++) {
        const uint8_t* row = bitmap.row_data(y);
        int row_bytes = bitmap.row_byte_width();

        // Horizontal flip: reverse pixel order within the row
        std::fill(flipped_row.begin(), flipped_row.end(), 0x00);
        for (int x = 0; x < size.width_px && x < bitmap.width(); x++) {
            int src_byte = x / 8;
            int src_bit = 7 - (x % 8);
            if (src_byte < row_bytes && (row[src_byte] & (1 << src_bit))) {
                int dst_x = size.width_px - 1 - x;
                int dst_byte = dst_x / 8;
                int dst_bit = 7 - (dst_x % 8);
                flipped_row[dst_byte] |= (1 << dst_bit);
            }
        }

        // Check if flipped row is all white
        bool all_white = true;
        for (int b = 0; b < label_byte_width; b++) {
            if (flipped_row[b] != 0x00) {
                all_white = false;
                break;
            }
        }

        if (all_white) {
            // Blank line marker
            cmd.push_back(0x5A);
        } else {
            // Raster data: 0x67 0x00 [byte_count] [data...]
            cmd.push_back(0x67);
            cmd.push_back(0x00);
            cmd.push_back(static_cast<uint8_t>(RASTER_ROW_BYTES));

            // Left padding (for narrow labels right-justified in 90-byte row)
            cmd.insert(cmd.end(), left_pad, 0x00);

            // Copy flipped pixel data
            cmd.insert(cmd.end(), flipped_row.begin(), flipped_row.end());
        }
    }

    // 10. Print command
    cmd.push_back(0x1A);

    return cmd;
}

void BrotherQLPrinter::print_label(const std::string& host, int port,
                                    const LabelBitmap& bitmap, const LabelSize& size,
                                    PrintCallback callback) {
    auto commands = build_raster_commands(bitmap, size);
    spdlog::info("Brother QL: sending {} bytes to {}:{}", commands.size(), host, port);

    std::thread([host, port, commands = std::move(commands), callback]() {
        bool success = false;
        std::string error;

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            error = fmt::format("socket() failed: {}", strerror(errno));
            spdlog::error("Brother QL: {}", error);
        } else {
            // Set send timeout
            struct timeval tv{};
            tv.tv_sec = 10;
            setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            // Resolve hostname
            struct addrinfo hints{}, *result = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            int gai_err = getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                                       &hints, &result);
            if (gai_err != 0) {
                error = fmt::format("DNS resolve failed for {}: {}", host,
                                    gai_strerror(gai_err));
                spdlog::error("Brother QL: {}", error);
            } else {
                // Connect
                if (connect(sockfd, result->ai_addr, result->ai_addrlen) < 0) {
                    error = fmt::format("connect to {}:{} failed: {}", host, port,
                                        strerror(errno));
                    spdlog::error("Brother QL: {}", error);
                } else {
                    // Send all data
                    size_t total_sent = 0;
                    while (total_sent < commands.size()) {
                        ssize_t sent = send(sockfd, commands.data() + total_sent,
                                            commands.size() - total_sent, MSG_NOSIGNAL);
                        if (sent < 0) {
                            error = fmt::format("send failed: {}", strerror(errno));
                            spdlog::error("Brother QL: {}", error);
                            break;
                        }
                        total_sent += static_cast<size_t>(sent);
                    }
                    if (total_sent == commands.size()) {
                        success = true;
                        spdlog::info("Brother QL: sent {} bytes successfully", total_sent);
                    }
                }
                freeaddrinfo(result);
            }
            close(sockfd);
        }

        // Dispatch callback to UI thread
        helix::ui::queue_update([callback, success, error]() {
            callback(success, error);
        });
    }).detach();
}

} // namespace helix
