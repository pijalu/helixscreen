// SPDX-License-Identifier: GPL-3.0-or-later

#include "bt_bus_thread.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace helix::bluetooth {

BusThread::BusThread(sd_bus* bus) : bus_(bus) {
    if (pipe2(wakeup_fds_, O_CLOEXEC | O_NONBLOCK) != 0) {
        fprintf(stderr, "[bt] BusThread pipe2 failed: %s\n", strerror(errno));
        wakeup_fds_[0] = wakeup_fds_[1] = -1;
    }
}

BusThread::~BusThread() {
    stop();
    if (wakeup_fds_[0] >= 0) close(wakeup_fds_[0]);
    if (wakeup_fds_[1] >= 0) close(wakeup_fds_[1]);
}

void BusThread::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;
    stopping_.store(false);
    thread_ = std::thread([this]{ loop(); });
    thread_id_ = thread_.get_id();  // Assigned by the calling thread before start() returns —
                                    // happens-before any later call to on_thread() via the caller's
                                    // subsequent actions on this object.
}

void BusThread::stop() {
    if (!running_.exchange(false))
        return;
    stopping_.store(true);
    notify();
    if (thread_.joinable())
        thread_.join();

    // Drain remaining queue AFTER join: the worker has exited, so no new items
    // can enter the queue from the worker side, and submit() callers either
    // (a) saw running_ == false and got an immediately-broken-promise future,
    // or (b) pushed before our running_.exchange and we catch them here.
    std::lock_guard<std::mutex> lk(mu_);
    while (!queue_.empty()) {
        queue_.front().second.set_exception(
            std::make_exception_ptr(std::runtime_error("BusThread stopped")));
        queue_.pop_front();
    }
}

std::future<void> BusThread::submit(BusWork work) {
    std::promise<void> p;
    auto fut = p.get_future();

    if (stopping_.load() || !running_.load()) {
        p.set_exception(std::make_exception_ptr(std::runtime_error("BusThread not running")));
        return fut;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.emplace_back(std::move(work), std::move(p));
    }
    notify();
    return fut;
}

void BusThread::run_sync(BusWork work) {
    if (on_thread()) {
        work(bus_);
        return;
    }
    submit(std::move(work)).get();
}

void BusThread::notify() {
    if (wakeup_fds_[1] >= 0) {
        uint8_t b = 1;
        ssize_t n = write(wakeup_fds_[1], &b, 1);
        (void)n; // EAGAIN is fine — pipe already has a pending byte
    }
}

bool BusThread::on_thread() const noexcept {
    return std::this_thread::get_id() == thread_id_;
}

void BusThread::loop() {
    while (running_.load()) {
        // 1. Drain queued work items.
        for (;;) {
            std::pair<BusWork, std::promise<void>> item;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (queue_.empty())
                    break;
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                item.first(bus_);
                item.second.set_value();
            } catch (...) {
                item.second.set_exception(std::current_exception());
            }
        }

        // 2. Process pending bus traffic. (Skipped in test mode.)
        if (!skip_bus_calls_for_test_) {
            int r = sd_bus_process(bus_, nullptr);
            if (r < 0) {
                fprintf(stderr, "[bt] BusThread sd_bus_process error: %s\n", strerror(-r));
                running_.store(false);
                stopping_.store(true);
                break;
            }
            if (r > 0)
                continue; // more bus work available; skip the wait
        }

        // 3. Wait for bus activity OR a wakeup-pipe byte, up to 500ms.
        struct pollfd pfds[2];
        int nfds = 0;
        if (!skip_bus_calls_for_test_) {
            int bus_fd = sd_bus_get_fd(bus_);
            if (bus_fd >= 0) {
                pfds[nfds].fd = bus_fd;
                pfds[nfds].events = sd_bus_get_events(bus_);
                nfds++;
            }
        }
        if (wakeup_fds_[0] >= 0) {
            pfds[nfds].fd = wakeup_fds_[0];
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int timeout_ms = 500;
        if (!skip_bus_calls_for_test_) {
            uint64_t timeout_us = 0;
            sd_bus_get_timeout(bus_, &timeout_us);
            if (timeout_us != UINT64_MAX) {
                // sd-bus timeout is absolute µs since epoch; clamp to 500ms max.
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now_us = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
                int64_t delta_ms = (int64_t(timeout_us) - int64_t(now_us)) / 1000;
                if (delta_ms < 0) delta_ms = 0;
                if (delta_ms < timeout_ms) timeout_ms = int(delta_ms);
            }
        }

        poll(pfds, nfds, timeout_ms);

        // Drain the wakeup pipe.
        if (wakeup_fds_[0] >= 0) {
            uint8_t buf[16];
            while (read(wakeup_fds_[0], buf, sizeof(buf)) > 0) {}
        }
    }
}

} // namespace helix::bluetooth
