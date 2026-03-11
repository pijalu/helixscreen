// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_scanner_monitor.h"

#include "qr_decoder.h"

#include <spdlog/spdlog.h>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#endif

namespace helix {

UsbScannerMonitor::~UsbScannerMonitor()
{
    stop();
}

char UsbScannerMonitor::keycode_to_char(int keycode, bool shift)
{
    // Number row: KEY_1(2)..KEY_9(10), KEY_0(11)
    if (keycode >= 2 && keycode <= 10) {
        if (shift) {
            // Shifted number row: !@#$%^&*(
            static const char shifted_nums[] = "!@#$%^&*(";
            return shifted_nums[keycode - 2];
        }
        return '0' + (keycode - 1);  // KEY_1=2 -> '1', KEY_2=3 -> '2', etc.
    }
    if (keycode == 11) {  // KEY_0
        return shift ? ')' : '0';
    }

    // KEY_MINUS(12), KEY_EQUAL(13)
    if (keycode == 12) return shift ? '_' : '-';
    if (keycode == 13) return shift ? '+' : '=';

    // Letter keys - mapped by keyboard physical layout (not alphabetical)
    // Row 1: Q(16) W(17) E(18) R(19) T(20) Y(21) U(22) I(23) O(24) P(25)
    // Row 2: A(30) S(31) D(32) F(33) G(34) H(35) J(36) K(37) L(38)
    // Row 3: Z(44) X(45) C(46) V(47) B(48) N(49) M(50)
    static const struct {
        int keycode;
        char lower;
    } letter_map[] = {
        {16, 'q'}, {17, 'w'}, {18, 'e'}, {19, 'r'}, {20, 't'},
        {21, 'y'}, {22, 'u'}, {23, 'i'}, {24, 'o'}, {25, 'p'},
        {30, 'a'}, {31, 's'}, {32, 'd'}, {33, 'f'}, {34, 'g'},
        {35, 'h'}, {36, 'j'}, {37, 'k'}, {38, 'l'},
        {44, 'z'}, {45, 'x'}, {46, 'c'}, {47, 'v'}, {48, 'b'},
        {49, 'n'}, {50, 'm'},
    };

    for (const auto& entry : letter_map) {
        if (entry.keycode == keycode) {
            return shift ? static_cast<char>(std::toupper(entry.lower)) : entry.lower;
        }
    }

    // Punctuation keys
    if (keycode == 39) return shift ? ':' : ';';   // KEY_SEMICOLON
    if (keycode == 51) return shift ? '<' : ',';   // KEY_COMMA
    if (keycode == 52) return shift ? '>' : '.';   // KEY_DOT
    if (keycode == 53) return shift ? '?' : '/';   // KEY_SLASH

    return 0;  // Unmapped
}

int UsbScannerMonitor::check_spoolman_pattern(const std::string& input)
{
    return QrDecoder::parse_spoolman_id(input);
}

#ifdef __linux__

std::vector<std::string> UsbScannerMonitor::find_scanner_devices()
{
    std::vector<std::string> devices;

    // Strategy 1: Check /dev/input/by-id/ for devices with barcode/scanner in name
    DIR* dir = opendir("/dev/input/by-id/");
    if (dir) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            // Case-insensitive check for barcode or scanner
            std::string lower_name = name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower_name.find("barcode") != std::string::npos ||
                lower_name.find("scanner") != std::string::npos) {
                std::string path = "/dev/input/by-id/" + name;
                devices.push_back(path);
                spdlog::info("UsbScannerMonitor: found scanner device by-id: {}", path);
            }
        }
        closedir(dir);
    }

    // Strategy 2: Check all /dev/input/event* devices via EVIOCGNAME
    if (devices.empty()) {
        dir = opendir("/dev/input/");
        if (dir) {
            struct dirent* entry = nullptr;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name(entry->d_name);
                if (name.find("event") != 0) continue;

                std::string path = "/dev/input/" + name;
                int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
                if (fd < 0) continue;

                char dev_name[256] = {0};
                if (ioctl(fd, EVIOCGNAME(sizeof(dev_name)), dev_name) >= 0) {
                    std::string dev_name_str(dev_name);
                    std::string lower_name = dev_name_str;
                    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (lower_name.find("barcode") != std::string::npos ||
                        lower_name.find("scanner") != std::string::npos) {
                        devices.push_back(path);
                        spdlog::info("UsbScannerMonitor: found scanner device by name '{}': {}",
                                     dev_name_str, path);
                    }
                }
                close(fd);
            }
            closedir(dir);
        }
    }

    return devices;
}

void UsbScannerMonitor::start(ScanCallback on_scan)
{
    if (running_.load()) {
        spdlog::warn("UsbScannerMonitor: already running");
        return;
    }

    callback_ = std::move(on_scan);

    if (pipe(stop_pipe_) != 0) {
        spdlog::error("UsbScannerMonitor: failed to create pipe: {}", strerror(errno));
        return;
    }

    running_.store(true);
    monitor_thread_ = std::thread(&UsbScannerMonitor::monitor_thread_func, this);
    spdlog::info("UsbScannerMonitor: started");
}

void UsbScannerMonitor::stop()
{
    if (!running_.load()) return;

    running_.store(false);

    // Wake up poll() by writing to stop pipe
    if (stop_pipe_[1] >= 0) {
        char c = 'x';
        (void)write(stop_pipe_[1], &c, 1);
    }

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    if (stop_pipe_[0] >= 0) {
        close(stop_pipe_[0]);
        stop_pipe_[0] = -1;
    }
    if (stop_pipe_[1] >= 0) {
        close(stop_pipe_[1]);
        stop_pipe_[1] = -1;
    }

    spdlog::info("UsbScannerMonitor: stopped");
}

void UsbScannerMonitor::monitor_thread_func()
{
    spdlog::debug("UsbScannerMonitor: monitor thread started");

    std::vector<int> device_fds;
    std::string accumulator;
    bool shift_held = false;

    auto open_devices = [&]() {
        // Close any previously open devices
        for (int fd : device_fds) {
            close(fd);
        }
        device_fds.clear();

        auto paths = find_scanner_devices();
        for (const auto& path : paths) {
            int fd = open(path.c_str(), O_RDONLY);
            if (fd < 0) {
                spdlog::warn("UsbScannerMonitor: failed to open {}: {}", path, strerror(errno));
                continue;
            }
            // Try to grab exclusively so keystrokes don't go to console
            if (ioctl(fd, EVIOCGRAB, 1) != 0) {
                spdlog::debug("UsbScannerMonitor: EVIOCGRAB failed for {} (non-fatal): {}",
                              path, strerror(errno));
            }
            device_fds.push_back(fd);
        }
    };

    open_devices();

    while (running_.load()) {
        // Build poll fd list: stop_pipe + device fds
        std::vector<struct pollfd> pfds;
        pfds.push_back({stop_pipe_[0], POLLIN, 0});

        if (device_fds.empty()) {
            // No devices found - poll on just the stop pipe with 5s timeout for re-scan
            int ret = poll(pfds.data(), pfds.size(), 5000);
            if (ret > 0 && (pfds[0].revents & POLLIN)) {
                break;  // Stop signal
            }
            // Re-scan for hot-plugged devices
            open_devices();
            continue;
        }

        for (int fd : device_fds) {
            pfds.push_back({fd, POLLIN, 0});
        }

        int ret = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 5000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            spdlog::error("UsbScannerMonitor: poll error: {}", strerror(errno));
            break;
        }

        // Check stop pipe
        if (pfds[0].revents & POLLIN) {
            break;
        }

        // Check device fds
        bool device_error = false;
        for (size_t i = 1; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;

            struct input_event ev {};
            ssize_t n = read(pfds[i].fd, &ev, sizeof(ev));
            if (n < static_cast<ssize_t>(sizeof(ev))) {
                if (n < 0 && errno != EAGAIN) {
                    spdlog::warn("UsbScannerMonitor: device read error, will re-scan");
                    device_error = true;
                }
                continue;
            }

            if (ev.type != EV_KEY) continue;

            // Track shift state
            if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                shift_held = (ev.value != 0);  // 1=press, 0=release, 2=repeat
                continue;
            }

            // Only process key-down events (value == 1)
            if (ev.value != 1) continue;

            if (ev.code == KEY_ENTER || ev.code == KEY_KPENTER) {
                // End of scan - check pattern
                if (!accumulator.empty()) {
                    spdlog::debug("UsbScannerMonitor: scanned text: '{}'", accumulator);
                    int spool_id = check_spoolman_pattern(accumulator);
                    if (spool_id >= 0 && callback_) {
                        spdlog::info("UsbScannerMonitor: detected Spoolman spool ID: {}",
                                     spool_id);
                        callback_(spool_id);
                    } else if (spool_id < 0) {
                        spdlog::debug("UsbScannerMonitor: text '{}' is not a Spoolman QR code",
                                      accumulator);
                    }
                    accumulator.clear();
                }
                continue;
            }

            char ch = keycode_to_char(static_cast<int>(ev.code), shift_held);
            if (ch != 0) {
                accumulator += ch;
            }
        }

        if (device_error) {
            // Re-scan for devices (one may have been unplugged)
            open_devices();
        }
    }

    // Clean up device fds
    for (int fd : device_fds) {
        close(fd);
    }

    spdlog::debug("UsbScannerMonitor: monitor thread exiting");
}

#else  // !__linux__

std::vector<std::string> UsbScannerMonitor::find_scanner_devices()
{
    return {};
}

void UsbScannerMonitor::start(ScanCallback /*on_scan*/)
{
    spdlog::warn("UsbScannerMonitor: USB barcode scanner monitoring is only supported on Linux");
}

void UsbScannerMonitor::stop()
{
    // No-op on non-Linux
}

void UsbScannerMonitor::monitor_thread_func()
{
    // No-op on non-Linux
}

#endif  // __linux__

} // namespace helix
