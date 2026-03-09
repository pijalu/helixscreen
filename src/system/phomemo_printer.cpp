// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "phomemo_printer.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#ifdef HELIX_HAS_LIBUSB
#include <libusb-1.0/libusb.h>
#endif

#include <thread>

namespace helix {

// Phomemo M110 defaults
static constexpr uint8_t DEFAULT_SPEED = 0x03;
static constexpr uint8_t DEFAULT_DENSITY = 0x08;

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
    // Pre-allocate: 11 (header) + 8 (GS v 0) + raster data + 8 (footer)
    cmd.reserve(11 + 8 + static_cast<size_t>(bytes_per_line) * num_lines + 8);

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

    // === Image: GS v 0 raster block (8 bytes + data) ===

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

    // Raster data — direct copy, no flip needed for Phomemo
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

void PhomemoPrinter::print(const LabelBitmap& bitmap, const LabelSize& size,
                            PrintCallback callback) {
#ifndef HELIX_HAS_LIBUSB
    (void)bitmap;
    (void)size;
    helix::ui::queue_update([callback]() {
        if (callback) callback(false, "USB printing not available on this platform");
    });
#else
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

        libusb_context* ctx = nullptr;
        int rc = libusb_init(&ctx);
        if (rc < 0) {
            error = fmt::format("libusb_init failed: {}", libusb_strerror(static_cast<libusb_error>(rc)));
            spdlog::error("Phomemo: {}", error);
        } else {
            libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, vid, pid);
            if (!handle) {
                error = fmt::format("USB device {:04x}:{:04x} not found", vid, pid);
                spdlog::error("Phomemo: {}", error);
            } else {
                // Detach kernel driver if active
                if (libusb_kernel_driver_active(handle, 0) == 1) {
                    libusb_detach_kernel_driver(handle, 0);
                }

                rc = libusb_claim_interface(handle, 0);
                if (rc < 0) {
                    error = fmt::format("claim interface failed: {}",
                                        libusb_strerror(static_cast<libusb_error>(rc)));
                    spdlog::error("Phomemo: {}", error);
                } else {
                    // Find bulk OUT endpoint
                    uint8_t endpoint = 0;
                    libusb_device* dev = libusb_get_device(handle);
                    struct libusb_config_descriptor* config = nullptr;
                    if (libusb_get_active_config_descriptor(dev, &config) == 0 && config) {
                        for (int i = 0; i < config->bNumInterfaces && endpoint == 0; i++) {
                            auto& iface = config->interface[i];
                            for (int j = 0; j < iface.num_altsetting && endpoint == 0; j++) {
                                auto& alt = iface.altsetting[j];
                                for (int k = 0; k < alt.bNumEndpoints; k++) {
                                    auto& ep = alt.endpoint[k];
                                    if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                                            LIBUSB_TRANSFER_TYPE_BULK &&
                                        (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                                            LIBUSB_ENDPOINT_OUT) {
                                        endpoint = ep.bEndpointAddress;
                                        break;
                                    }
                                }
                            }
                        }
                        libusb_free_config_descriptor(config);
                    }

                    if (endpoint == 0) {
                        error = "no bulk OUT endpoint found";
                        spdlog::error("Phomemo: {}", error);
                    } else {
                        int transferred = 0;
                        rc = libusb_bulk_transfer(handle, endpoint,
                                                  const_cast<uint8_t*>(commands.data()),
                                                  static_cast<int>(commands.size()),
                                                  &transferred, 10000);
                        bool success_transfer = (rc == 0 && transferred == static_cast<int>(commands.size()));
                        if (!success_transfer) {
                            error = fmt::format("USB transfer failed: {} (sent {}/{})",
                                                libusb_strerror(static_cast<libusb_error>(rc)),
                                                transferred, commands.size());
                            spdlog::error("Phomemo: {}", error);
                        } else {
                            success = true;
                            spdlog::info("Phomemo: sent {} bytes successfully", transferred);
                        }
                    }

                    libusb_release_interface(handle, 0);
                }
                libusb_close(handle);
            }
            libusb_exit(ctx);
        }

        // Dispatch callback to UI thread
        helix::ui::queue_update([callback, success, error]() {
            callback(success, error);
        });
    }).detach();
#endif // HELIX_HAS_LIBUSB
}

} // namespace helix
