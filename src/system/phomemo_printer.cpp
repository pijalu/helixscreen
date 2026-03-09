// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "phomemo_printer.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <thread>

namespace helix {

// Phomemo M110 defaults
static constexpr uint8_t DEFAULT_SPEED = 0x03;
static constexpr uint8_t DEFAULT_DENSITY = 0x0A;

PhomemoPrinter::PhomemoPrinter() = default;
PhomemoPrinter::~PhomemoPrinter() = default;

std::string PhomemoPrinter::name() const {
    return "Phomemo M110";
}

void PhomemoPrinter::set_device(uint16_t vid, uint16_t pid, const std::string& serial) {
    vid_ = vid;
    pid_ = pid;
    serial_ = serial;
}

std::vector<LabelSize> PhomemoPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> PhomemoPrinter::supported_sizes_static() {
    // All sizes at 203 DPI, gap/die-cut media (0x0A)
    return {
        {"40x30mm",  319, 240, 203, 0x0A, 40, 30},
        {"40x20mm",  319, 160, 203, 0x0A, 40, 20},
        {"50x30mm",  400, 240, 203, 0x0A, 50, 30},
        {"50x50mm",  400, 400, 203, 0x0A, 50, 50},
        {"50x80mm",  400, 640, 203, 0x0A, 50, 80},
        {"30x20mm",  240, 160, 203, 0x0A, 30, 20},
        {"25x10mm",  200,  80, 203, 0x0A, 25, 10},
        {"40x60mm",  319, 480, 203, 0x0A, 40, 60},
    };
}

std::vector<uint8_t> PhomemoPrinter::build_raster_commands(const LabelBitmap& bitmap,
                                                            const LabelSize& size) {
    int bytes_per_line = (bitmap.width() + 7) / 8;
    int num_lines = bitmap.height();

    std::vector<uint8_t> cmd;
    cmd.reserve(19 + static_cast<size_t>(bytes_per_line) * num_lines + 8);

    // === Header (11 bytes) ===

    // Print speed: ESC N 0D <speed>
    cmd.push_back(0x1B);
    cmd.push_back(0x4E);
    cmd.push_back(0x0D);
    cmd.push_back(DEFAULT_SPEED);

    // Print density: ESC N 04 <density>
    cmd.push_back(0x1B);
    cmd.push_back(0x4E);
    cmd.push_back(0x04);
    cmd.push_back(DEFAULT_DENSITY);

    // Media type: 1F 11 <type>
    cmd.push_back(0x1F);
    cmd.push_back(0x11);
    cmd.push_back(size.media_type);

    // === Image: GS v 0 raster block ===

    // GS v 0 command
    cmd.push_back(0x1D);
    cmd.push_back(0x76);
    cmd.push_back(0x30);
    cmd.push_back(0x00); // normal mode

    // bytes_per_line (16-bit LE)
    cmd.push_back(static_cast<uint8_t>(bytes_per_line & 0xFF));
    cmd.push_back(static_cast<uint8_t>((bytes_per_line >> 8) & 0xFF));

    // num_lines (16-bit LE)
    cmd.push_back(static_cast<uint8_t>(num_lines & 0xFF));
    cmd.push_back(static_cast<uint8_t>((num_lines >> 8) & 0xFF));

    // Raster data
    for (int y = 0; y < num_lines; y++) {
        const uint8_t* row = bitmap.row_data(y);
        cmd.insert(cmd.end(), row, row + bytes_per_line);
    }

    // === Footer (8 bytes) ===

    // Finalize: 1F F0 05 00
    cmd.push_back(0x1F);
    cmd.push_back(0xF0);
    cmd.push_back(0x05);
    cmd.push_back(0x00);

    // Feed to gap: 1F F0 03 00
    cmd.push_back(0x1F);
    cmd.push_back(0xF0);
    cmd.push_back(0x03);
    cmd.push_back(0x00);

    return cmd;
}

// Find the /dev/usb/lp* device node for a given VID:PID by checking sysfs
static std::string find_usblp_device(uint16_t vid, uint16_t pid) {
    // Scan /dev/usb/lp* and match against sysfs VID:PID
    namespace fs = std::filesystem;

    for (int i = 0; i < 8; i++) {
        std::string dev_path = fmt::format("/dev/usb/lp{}", i);
        std::string sysfs_path = fmt::format("/sys/class/usbmisc/lp{}/device/../", i);

        // Read VID/PID from sysfs
        auto read_hex = [](const std::string& path) -> uint16_t {
            std::ifstream f(path);
            if (!f.is_open()) return 0;
            std::string val;
            f >> val;
            return static_cast<uint16_t>(std::stoul(val, nullptr, 16));
        };

        uint16_t dev_vid = read_hex(sysfs_path + "idVendor");
        uint16_t dev_pid = read_hex(sysfs_path + "idProduct");

        if (dev_vid == vid && dev_pid == pid) {
            spdlog::debug("Phomemo: matched {} to {:04x}:{:04x}", dev_path, vid, pid);
            return dev_path;
        }
    }
    return {};
}

void PhomemoPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                            PrintCallback callback) {
    if (vid_ == 0 || pid_ == 0) {
        spdlog::error("Phomemo: USB device not configured (vid/pid not set)");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "USB device not configured");
        });
        return;
    }

    auto commands = build_raster_commands(bitmap, size);
    spdlog::info("Phomemo: sending {} bytes to USB {:04x}:{:04x}", commands.size(), vid_, pid_);

    uint16_t vid = vid_;
    uint16_t pid = pid_;

    std::thread([vid, pid, commands = std::move(commands), callback]() {
        bool success = false;
        std::string error;

        // Find the usblp device node for this VID:PID
        std::string dev_path = find_usblp_device(vid, pid);
        if (dev_path.empty()) {
            error = fmt::format("No USB printer device found for {:04x}:{:04x}. "
                                "Is the printer turned on?", vid, pid);
            spdlog::error("Phomemo: {}", error);
        } else {
            std::ofstream f(dev_path, std::ios::binary);
            if (!f.is_open()) {
                error = fmt::format("Cannot open {} (check permissions)", dev_path);
                spdlog::error("Phomemo: {}", error);
            } else {
                f.write(reinterpret_cast<const char*>(commands.data()),
                        static_cast<std::streamsize>(commands.size()));
                f.flush();
                if (f.good()) {
                    success = true;
                    spdlog::info("Phomemo: sent {} bytes via {}", commands.size(), dev_path);
                } else {
                    error = fmt::format("Write to {} failed", dev_path);
                    spdlog::error("Phomemo: {}", error);
                }
            }
        }

        helix::ui::queue_update([callback, success, error]() {
            callback(success, error);
        });
    }).detach();
}

} // namespace helix
