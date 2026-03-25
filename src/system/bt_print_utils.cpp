// SPDX-License-Identifier: GPL-3.0-or-later

#include "bt_print_utils.h"
#include "bluetooth_loader.h"

#include <spdlog/spdlog.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mutex>

namespace helix::bluetooth {

// Single mutex for ALL Bluetooth RFCOMM prints — only one connection at a time
static std::mutex s_rfcomm_mutex;

RfcommSendResult rfcomm_send(const std::string& mac, int channel,
                              const std::vector<uint8_t>& data,
                              const std::string& log_tag) {
    std::lock_guard<std::mutex> lock(s_rfcomm_mutex);

    RfcommSendResult result;
    auto& loader = BluetoothLoader::instance();

    auto* ctx = loader.init();
    if (!ctx) {
        result.error = "Failed to initialize Bluetooth context";
        spdlog::error("{}: {}", log_tag, result.error);
        return result;
    }

    int fd = loader.connect_rfcomm(ctx, mac.c_str(), channel);
    if (fd < 0) {
        const char* err = loader.last_error ? loader.last_error(ctx) : "unknown error";
        result.error = fmt::format("RFCOMM connect failed: {}", err);
        spdlog::error("{}: {}", log_tag, result.error);
        loader.deinit(ctx);
        return result;
    }

    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = ::write(fd, data.data() + total_sent,
                               data.size() - total_sent);
        if (sent < 0) {
            result.error = fmt::format("Write failed: {}", strerror(errno));
            spdlog::error("{}: {}", log_tag, result.error);
            break;
        }
        total_sent += static_cast<size_t>(sent);
    }

    if (total_sent == data.size()) {
        result.success = true;
        spdlog::info("{}: sent {} bytes via RFCOMM, draining...", log_tag, total_sent);
        // RFCOMM write() returns when data is copied to the kernel buffer,
        // not when it's delivered over the air. Wait for transmission.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    loader.disconnect(ctx, fd);
    loader.deinit(ctx);
    return result;
}

RfcommSendResult rfcomm_send_receive(const std::string& mac, int channel,
                                      const std::vector<uint8_t>& data,
                                      size_t response_len,
                                      const std::string& log_tag) {
    std::lock_guard<std::mutex> lock(s_rfcomm_mutex);

    RfcommSendResult result;
    auto& loader = BluetoothLoader::instance();

    auto* ctx = loader.init();
    if (!ctx) {
        result.error = "Failed to initialize Bluetooth context";
        spdlog::error("{}: {}", log_tag, result.error);
        return result;
    }

    int fd = loader.connect_rfcomm(ctx, mac.c_str(), channel);
    if (fd < 0) {
        const char* err = loader.last_error ? loader.last_error(ctx) : "unknown error";
        result.error = fmt::format("RFCOMM connect failed: {}", err);
        spdlog::error("{}: {}", log_tag, result.error);
        loader.deinit(ctx);
        return result;
    }

    // Send request
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = ::write(fd, data.data() + total_sent,
                               data.size() - total_sent);
        if (sent < 0) {
            result.error = fmt::format("Write failed: {}", strerror(errno));
            spdlog::error("{}: {}", log_tag, result.error);
            loader.disconnect(ctx, fd);
            loader.deinit(ctx);
            return result;
        }
        total_sent += static_cast<size_t>(sent);
    }

    // Wait for printer to process the command before reading
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Read response with poll + timeout
    result.response.resize(response_len);
    size_t total_read = 0;

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    // Poll for up to 3 seconds total
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (total_read < response_len) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
            break;
        int ready = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ready <= 0)
            break;
        ssize_t n = ::read(fd, result.response.data() + total_read,
                           response_len - total_read);
        if (n <= 0)
            break;
        total_read += static_cast<size_t>(n);
    }

    result.response.resize(total_read);
    result.success = (total_read >= response_len);

    spdlog::debug("{}: sent {} bytes, received {} bytes", log_tag, total_sent, total_read);

    loader.disconnect(ctx, fd);
    loader.deinit(ctx);
    return result;
}

}  // namespace helix::bluetooth
