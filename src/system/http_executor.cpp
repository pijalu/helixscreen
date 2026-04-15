// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_executor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix::http {

namespace {

// Thread-local pointer to the executor owning the current worker thread.
// Set by loop() on entry, cleared on exit. Powers on_thread() and correctly
// distinguishes between multiple HttpExecutor instances.
thread_local HttpExecutor* tls_current_executor_ = nullptr;

}  // namespace

HttpExecutor::HttpExecutor(std::string name, std::size_t worker_count)
    : name_(std::move(name)), worker_count_(std::max<std::size_t>(worker_count, 1)) {}

HttpExecutor::~HttpExecutor() {
    stop();
}

void HttpExecutor::start() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (running_) {
            return;
        }
        running_ = true;
        stopping_ = false;
    }

    workers_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
        auto w = std::make_unique<Worker>();
        Worker* raw = w.get();
        w->thread = std::thread([this, raw, i]() {
            loop(i);
            raw->done.store(true);
        });
        workers_.push_back(std::move(w));
    }
}

void HttpExecutor::stop(std::chrono::milliseconds join_timeout) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) {
            return;
        }
        running_ = false;
        stopping_ = true;
    }
    cv_.notify_all();

    // Wait for each worker to finish its in-flight item, up to join_timeout.
    // Poll a per-worker done flag set at the end of loop() — lets us detach
    // a worker that's stuck in a long HTTP call without blocking the others.
    constexpr auto kPollInterval = std::chrono::milliseconds(10);
    auto deadline = std::chrono::steady_clock::now() + join_timeout;
    for (auto& w : workers_) {
        while (!w->done.load()) {
            if (std::chrono::steady_clock::now() > deadline) {
                spdlog::warn("[HttpExecutor:{}] worker did not finish in {}ms — "
                             "detaching (will terminate with process)",
                             name_, join_timeout.count());
                w->thread.detach();
                break;
            }
            std::this_thread::sleep_for(kPollInterval);
        }
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
    workers_.clear();

    // Break promises on anything still queued. By this point submit() rejects
    // new work (stopping_ is true under mu_), so the queue is stable.
    std::deque<std::pair<HttpWork, std::promise<void>>> leftover;
    {
        std::lock_guard<std::mutex> lk(mu_);
        leftover = std::move(queue_);
    }
    // leftover's promises are dropped as it goes out of scope → broken_promise
    // surfaces to anyone holding the futures.
}

std::future<void> HttpExecutor::submit(HttpWork work) {
    std::promise<void> promise;
    auto fut = promise.get_future();

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_ || stopping_) {
            // Reject: drop the promise → broken_promise on future::get().
            return fut;
        }
        queue_.emplace_back(std::move(work), std::move(promise));
    }
    cv_.notify_one();
    return fut;
}

void HttpExecutor::run_sync(HttpWork work) {
    auto fut = submit(std::move(work));
    fut.get();  // Throws std::future_error if stopped before running.
}

bool HttpExecutor::on_thread() const noexcept {
    return tls_current_executor_ == this;
}

void HttpExecutor::loop(std::size_t worker_index) {
    tls_current_executor_ = this;
    spdlog::debug("[HttpExecutor:{}] worker {} started", name_, worker_index);

    while (true) {
        std::pair<HttpWork, std::promise<void>> item;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this]() { return stopping_ || !queue_.empty(); });
            // On stop, exit immediately regardless of remaining queue.
            // Leftover items have their promises broken by stop() afterward.
            if (stopping_) {
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            item.first();
            item.second.set_value();
        } catch (...) {
            // Surface the exception via the future. Worker continues.
            try {
                item.second.set_exception(std::current_exception());
            } catch (...) {
                // Promise already satisfied or no shared state — nothing to do.
            }
        }
    }

    tls_current_executor_ = nullptr;
    spdlog::debug("[HttpExecutor:{}] worker {} exiting", name_, worker_index);
}

// ---------------------------------------------------------------------------
// Process-wide executors
// ---------------------------------------------------------------------------

HttpExecutor& HttpExecutor::fast() {
    // 4 workers: preserves burst parallelism for thumbnail loading and
    // concurrent status polls without reintroducing unbounded spawning.
    static HttpExecutor instance("fast", 4);
    return instance;
}

HttpExecutor& HttpExecutor::slow() {
    // 1 worker: serializes large file transfers, guaranteeing uploads
    // don't saturate the network or flood libhv concurrent-request limits.
    static HttpExecutor instance("slow", 1);
    return instance;
}

void HttpExecutor::start_all() {
    fast().start();
    slow().start();
}

void HttpExecutor::stop_all() {
    // Stop slow first so any in-flight upload gets its join_timeout window
    // before fast lane teardown noise starts.
    slow().stop();
    fast().stop();
}

}  // namespace helix::http
