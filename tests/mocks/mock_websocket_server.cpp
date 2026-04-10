// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mock_websocket_server.h"

#include "hv/WebSocketServer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

MockWebSocketServer::MockWebSocketServer()
    : ws_service_(std::make_unique<hv::WebSocketService>()),
      server_(std::make_unique<hv::WebSocketServer>(ws_service_.get())) {
    setup_callbacks();
}

MockWebSocketServer::~MockWebSocketServer() {
    stop();
}

void MockWebSocketServer::setup_callbacks() {
    ws_service_->onopen = [this](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
        spdlog::debug("[MockWS] Client connected from {}", req->Path());
        connection_count_++;

        std::lock_guard<std::mutex> lock(channels_mutex_);
        channels_.push_back(channel);
    };

    ws_service_->onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
        spdlog::debug("[MockWS] Received: {}", msg.substr(0, 200));
        request_count_++;
        handle_message(channel, msg);
    };

    ws_service_->onclose = [this](const WebSocketChannelPtr& channel) {
        spdlog::debug("[MockWS] Client disconnected");
        connection_count_--;

        std::lock_guard<std::mutex> lock(channels_mutex_);
        channels_.erase(std::remove(channels_.begin(), channels_.end(), channel), channels_.end());
    };
}

int MockWebSocketServer::start(int port) {
    if (running_.load()) {
        spdlog::warn("[MockWS] Server already running");
        return port_.load();
    }

    server_->port = port;
    int result = server_->start();

    if (result == 0) {
        running_.store(true);

        // When using ephemeral port (0), get actual port via getsockname
        // libhv's start() is async, so wait for socket to be ready
        int actual_port = server_->port;
        spdlog::debug("[MockWS] server_->port={}, listenfd[0]={}", server_->port,
                      server_->listenfd[0]);

        if (actual_port == 0) {
            // Wait up to 5 seconds for libhv's async start() to open the listen socket
            for (int i = 0; i < 500 && server_->listenfd[0] < 0; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            spdlog::debug("[MockWS] After wait: listenfd[0]={}", server_->listenfd[0]);

            if (server_->listenfd[0] >= 0) {
                struct sockaddr_in addr;
                socklen_t addr_len = sizeof(addr);
                int gsn_result =
                    getsockname(server_->listenfd[0], (struct sockaddr*)&addr, &addr_len);
                spdlog::debug("[MockWS] getsockname returned {}, port={}", gsn_result,
                              gsn_result == 0 ? ntohs(addr.sin_port) : -1);
                if (gsn_result == 0) {
                    actual_port = ntohs(addr.sin_port);
                }
            }

            // If we still couldn't get a valid port, fail the start.
            // IMPORTANT: On macOS, libhv's partially-initialized server hangs in
            // http_server_stop → pthread_join when the listen socket never became
            // ready (kqueue issue). Also the ~WebSocketServer destructor will
            // try to stop/join the same way. So we release() the unique_ptr and
            // intentionally leak the partially-started libhv server. running_ stays
            // false so our own stop()/destructor short-circuits.
            if (actual_port <= 0) {
                spdlog::error("[MockWS] Timed out waiting for ephemeral port assignment — "
                              "leaking partially-started libhv server to avoid hang");
                running_.store(false);
                [[maybe_unused]] auto* leaked = server_.release();
                return -1;
            }
        }

        port_.store(actual_port);
        spdlog::info("[MockWS] Server started on port {}", port_.load());
        return port_.load();
    } else {
        spdlog::error("[MockWS] Failed to start server: {}", result);
        return -1;
    }
}

void MockWebSocketServer::stop() {
    if (!running_.load()) {
        return;
    }

    spdlog::debug("[MockWS] Stopping server");
    running_.store(false);

    // Disconnect all clients first
    disconnect_all();

    // Stop the server
    server_->stop();

    spdlog::info("[MockWS] Server stopped");
}

std::string MockWebSocketServer::url() const {
    // MoonrakerClient expects /websocket path, libhv accepts any path
    return "ws://127.0.0.1:" + std::to_string(port_.load()) + "/websocket";
}

void MockWebSocketServer::on_method(const std::string& method, Handler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_[method] = std::move(handler);
}

void MockWebSocketServer::on_method_error(const std::string& method, ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    error_handlers_[method] = std::move(handler);
}

void MockWebSocketServer::on_any_method(Handler handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    fallback_handler_ = std::move(handler);
}

void MockWebSocketServer::clear_handlers() {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    handlers_.clear();
    error_handlers_.clear();
    fallback_handler_ = nullptr;
}

void MockWebSocketServer::handle_message(const WebSocketChannelPtr& channel,
                                         const std::string& msg) {
    json request;
    try {
        request = json::parse(msg);
    } catch (const json::parse_error& e) {
        spdlog::warn("[MockWS] Invalid JSON: {}", e.what());
        send_error(channel, 0, -32700, "Parse error");
        return;
    }

    // Validate JSON-RPC structure
    if (!request.contains("method") || !request["method"].is_string()) {
        send_error(channel, request.value("id", 0), -32600, "Invalid Request: missing method");
        return;
    }

    std::string method = request["method"].get<std::string>();
    json params = request.value("params", json::object());
    uint64_t id = request.value("id", 0);

    // Track received methods
    {
        std::lock_guard<std::mutex> lock(methods_mutex_);
        received_methods_.push_back(method);
    }

    // Apply response delay if configured
    int delay = response_delay_ms_.load();
    if (delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    // Look up handler
    Handler handler;
    ErrorHandler error_handler;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);

        // Check for error handler first
        auto error_it = error_handlers_.find(method);
        if (error_it != error_handlers_.end()) {
            error_handler = error_it->second;
        }

        // Check for success handler
        auto it = handlers_.find(method);
        if (it != handlers_.end()) {
            handler = it->second;
        } else if (fallback_handler_) {
            handler = fallback_handler_;
        }
    }

    // Invoke error handler if present
    if (error_handler) {
        try {
            auto [code, message] = error_handler(params);
            send_error(channel, id, code, message);
            return;
        } catch (const std::exception& e) {
            spdlog::error("[MockWS] Error handler threw: {}", e.what());
            send_error(channel, id, -32603, "Internal error");
            return;
        }
    }

    // Invoke success handler
    if (handler) {
        try {
            json result = handler(params);
            send_response(channel, id, result);
        } catch (const std::exception& e) {
            spdlog::error("[MockWS] Handler threw: {}", e.what());
            send_error(channel, id, -32603, std::string("Internal error: ") + e.what());
        }
    } else {
        // No handler - return empty result (like Moonraker does for some methods)
        spdlog::debug("[MockWS] No handler for method '{}', returning empty result", method);
        send_response(channel, id, json::object());
    }
}

void MockWebSocketServer::send_response(const WebSocketChannelPtr& channel, uint64_t id,
                                        const json& result) {
    json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};

    std::string msg = response.dump();
    spdlog::debug("[MockWS] Sending response: {}", msg.substr(0, 200));
    channel->send(msg);
}

void MockWebSocketServer::send_error(const WebSocketChannelPtr& channel, uint64_t id, int code,
                                     const std::string& message) {
    json response = {
        {"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};

    std::string msg = response.dump();
    spdlog::debug("[MockWS] Sending error: {}", msg);
    channel->send(msg);
}

void MockWebSocketServer::send_notification(const std::string& method, const json& params) {
    json notification = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};

    std::string msg = notification.dump();
    spdlog::debug("[MockWS] Broadcasting notification: {}", method);

    std::lock_guard<std::mutex> lock(channels_mutex_);
    for (auto& channel : channels_) {
        if (channel->isConnected()) {
            channel->send(msg);
        }
    }
}

void MockWebSocketServer::send_notification_to(int channel_id, const std::string& method,
                                               const json& params) {
    json notification = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};

    std::string msg = notification.dump();

    std::lock_guard<std::mutex> lock(channels_mutex_);
    if (channel_id >= 0 && channel_id < static_cast<int>(channels_.size())) {
        if (channels_[channel_id]->isConnected()) {
            channels_[channel_id]->send(msg);
        }
    }
}

void MockWebSocketServer::disconnect_all() {
    std::lock_guard<std::mutex> lock(channels_mutex_);
    for (auto& channel : channels_) {
        if (channel->isConnected()) {
            channel->close();
        }
    }
    channels_.clear();
}

std::vector<std::string> MockWebSocketServer::received_methods() const {
    std::lock_guard<std::mutex> lock(methods_mutex_);
    return received_methods_;
}

void MockWebSocketServer::reset_stats() {
    request_count_.store(0);
    std::lock_guard<std::mutex> lock(methods_mutex_);
    received_methods_.clear();
}
