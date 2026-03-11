// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace helix {

class UsbScannerMonitor {
public:
    // Callback fires on the monitor background thread.
    // Callers must use ui_queue_update() if touching LVGL objects.
    using ScanCallback = std::function<void(int spool_id)>;

    UsbScannerMonitor() = default;
    ~UsbScannerMonitor();

    // Non-copyable
    UsbScannerMonitor(const UsbScannerMonitor&) = delete;
    UsbScannerMonitor& operator=(const UsbScannerMonitor&) = delete;

    void start(ScanCallback on_scan);
    void stop();
    bool is_running() const { return running_.load(); }

    // Public for testing: convert evdev keycode to ASCII character
    static char keycode_to_char(int keycode, bool shift);

    // Public for testing: check if accumulated string is a Spoolman QR code
    // and extract the spool ID. Returns -1 if not a match.
    static int check_spoolman_pattern(const std::string& input);

private:
    void monitor_thread_func();
    std::vector<std::string> find_scanner_devices();

    std::thread monitor_thread_;
    std::atomic<bool> running_{false};
    int stop_pipe_[2] = {-1, -1};  // For waking up poll()
    ScanCallback callback_;
};

} // namespace helix
