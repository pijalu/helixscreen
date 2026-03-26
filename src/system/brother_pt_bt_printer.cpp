// SPDX-License-Identifier: GPL-3.0-or-later

#include "brother_pt_bt_printer.h"
#include "bluetooth_loader.h"
#include "brother_pt_protocol.h"
#include "bt_print_utils.h"
#include "label_renderer.h"
#include "spoolman_types.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <thread>

namespace helix::label {

void BrotherPTBluetoothPrinter::set_device(const std::string& mac, int channel) {
    mac_ = mac;
    channel_ = channel;
}

std::string BrotherPTBluetoothPrinter::name() const {
    return "Brother PT (Bluetooth)";
}

std::vector<LabelSize> BrotherPTBluetoothPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> BrotherPTBluetoothPrinter::supported_sizes_static() {
    std::vector<LabelSize> sizes;
    for (int w : {4, 6, 9, 12, 18, 24}) {
        auto s = brother_pt_label_size_for_tape(w);
        if (s) sizes.push_back(*s);
    }
    return sizes;
}

void BrotherPTBluetoothPrinter::print(const LabelBitmap& bitmap,
                                        const LabelSize& size,
                                        PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("[Brother PT BT] Bluetooth not available");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth not available");
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("[Brother PT BT] No device configured");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth device not configured");
        });
        return;
    }

    auto commands = brother_pt_build_raster(bitmap, size.width_mm);
    if (commands.empty()) {
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Unsupported tape width");
        });
        return;
    }

    spdlog::info("[Brother PT BT] Sending {} bytes to {} ch{}",
                 commands.size(), mac_, channel_);

    std::string mac = mac_;
    int channel = channel_;

    std::thread([mac, channel, commands = std::move(commands), callback]() {
        auto result = helix::bluetooth::rfcomm_send(mac, channel, commands, "Brother PT BT");
        helix::ui::queue_update([callback, result]() {
            if (callback) callback(result.success, result.error);
        });
    }).detach();
}

void BrotherPTBluetoothPrinter::print_spool(const SpoolInfo& spool,
                                              LabelPreset preset,
                                              PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("[Brother PT BT] Bluetooth not available");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth not available");
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("[Brother PT BT] No device configured");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth device not configured");
        });
        return;
    }

    std::string mac = mac_;
    int channel = channel_;

    std::thread([mac, channel, spool, preset, callback]() {
        // Step 1: Query status to detect loaded tape
        auto status_cmd = brother_pt_build_status_request();
        auto status_result = helix::bluetooth::rfcomm_send_receive(
            mac, channel, status_cmd, 32, "Brother PT BT status");

        if (!status_result.success) {
            helix::ui::queue_update([callback, err = status_result.error]() {
                if (callback) callback(false, "Status query failed: " + err);
            });
            return;
        }

        auto media = brother_pt_parse_status(status_result.response.data(),
                                              status_result.response.size());
        if (!media.valid) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "Invalid status response from printer");
            });
            return;
        }

        auto error = brother_pt_error_string(media);
        if (!error.empty()) {
            helix::ui::queue_update([callback, error]() {
                if (callback) callback(false, error);
            });
            return;
        }

        if (media.width_mm == 0) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "No tape detected in printer");
            });
            return;
        }

        // Step 2: Build label size from detected tape
        auto label_size = brother_pt_label_size_for_tape(media.width_mm);
        if (!label_size) {
            helix::ui::queue_update([callback, w = media.width_mm]() {
                if (callback)
                    callback(false, "Unsupported tape width: " + std::to_string(w) + "mm");
            });
            return;
        }

        spdlog::info("[Brother PT BT] Detected {}mm tape ({} printable pixels)",
                     label_size->name, label_size->width_px);

        // Step 3: Render label at detected tape dimensions
        auto actual_preset = preset;
        if (label_size->width_px <= 50) {
            actual_preset = LabelPreset::MINIMAL;
        }

        auto bitmap = helix::LabelRenderer::render(spool, actual_preset, *label_size);
        if (bitmap.empty()) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "Failed to render label");
            });
            return;
        }

        // Step 4: Build raster and send
        auto commands = brother_pt_build_raster(bitmap, media.width_mm);
        if (commands.empty()) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "Failed to build raster data");
            });
            return;
        }

        spdlog::info("[Brother PT BT] Sending {} bytes to {} ch{}",
                     commands.size(), mac, channel);

        auto result = helix::bluetooth::rfcomm_send(mac, channel, commands, "Brother PT BT");
        helix::ui::queue_update([callback, result]() {
            if (callback) callback(result.success, result.error);
        });
    }).detach();
}

}  // namespace helix::label
