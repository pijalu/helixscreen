// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file async_lifetime_guard.h
 * @brief Generation-counter-based async callback safety
 *
 * Provides a lightweight mechanism to detect whether an object is still valid
 * when a deferred callback fires. The guard lives in the protected object;
 * lambdas capture a LifetimeToken (which holds a shared_ptr to the generation
 * counter, NOT a pointer to the guard itself). This makes the check safe even
 * if the guard has been destroyed before the callback executes.
 *
 * Usage:
 * @code
 * class MyOverlay {
 *     helix::AsyncLifetimeGuard guard_;
 *
 *     void start_async_work() {
 *         // Callback will silently skip if overlay is dismissed before it fires
 *         guard_.defer("MyOverlay::on_result", [this]() {
 *             update_ui_with_result();
 *         });
 *     }
 *
 *     ~MyOverlay() {
 *         // guard_ destructor calls invalidate(), expiring all outstanding tokens
 *     }
 * };
 * @endcode
 */

#pragma once

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <utility>

namespace helix {

class AsyncLifetimeGuard;

/**
 * @brief Lightweight, copyable token captured in lambdas to check validity
 *
 * Holds a shared_ptr to the generation counter (NOT to the guard), so the
 * token remains safe to query even after the guard is destroyed.
 */
class LifetimeToken {
  public:
    /**
     * @brief Check if the generation has advanced past the snapshot
     * @return true if the owning object has been invalidated/destroyed
     */
    bool expired() const {
        return gen_->load(std::memory_order_acquire) != snapshot_;
    }

    /**
     * @brief Convenience: true if NOT expired (object still alive)
     */
    explicit operator bool() const {
        return !expired();
    }

  private:
    friend class AsyncLifetimeGuard;

    LifetimeToken(std::shared_ptr<std::atomic<uint64_t>> gen, uint64_t snapshot)
        : gen_(std::move(gen)), snapshot_(snapshot) {}

    std::shared_ptr<std::atomic<uint64_t>> gen_;
    uint64_t snapshot_;
};

/**
 * @brief Owned by the protected object; produces tokens and defers callbacks
 *
 * Non-copyable, non-movable. Destructor calls invalidate() to expire all
 * outstanding tokens. The defer() methods capture a shared_ptr to the
 * generation counter (not `this`), so the lambda is safe even if the guard
 * is destroyed before it fires.
 */
class AsyncLifetimeGuard {
  public:
    AsyncLifetimeGuard() : gen_(std::make_shared<std::atomic<uint64_t>>(0)) {}

    ~AsyncLifetimeGuard() {
        invalidate();
    }

    // Non-copyable, non-movable
    AsyncLifetimeGuard(const AsyncLifetimeGuard&) = delete;
    AsyncLifetimeGuard& operator=(const AsyncLifetimeGuard&) = delete;
    AsyncLifetimeGuard(AsyncLifetimeGuard&&) = delete;
    AsyncLifetimeGuard& operator=(AsyncLifetimeGuard&&) = delete;

    /**
     * @brief Capture the current generation as a token
     *
     * The token can be copied into lambdas and checked later. It will report
     * expired() == true once invalidate() is called (or the guard is destroyed).
     */
    LifetimeToken token() const {
        return LifetimeToken(gen_, gen_->load(std::memory_order_acquire));
    }

    /**
     * @brief Advance the generation counter, expiring all outstanding tokens
     *
     * Safe to call multiple times. Each call expires tokens from every
     * previous generation.
     */
    void invalidate() {
        gen_->fetch_add(1, std::memory_order_release);
    }

    /**
     * @brief Queue a callback that is skipped if the guard has been invalidated
     *
     * Captures a shared_ptr to the generation counter and a snapshot of the
     * current generation. When the callback fires, it compares the snapshot
     * to the current generation; if they differ, the callback is silently
     * skipped.
     *
     * @tparam F Callable with signature void()
     * @param fn The callback to defer
     */
    template <typename F>
    void defer(F&& fn) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        helix::ui::queue_update([gen, snapshot, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot) {
                return;
            }
            f();
        });
    }

    /**
     * @brief Queue a tagged callback that is skipped if the guard has been invalidated
     *
     * Same as defer(fn), but the tag is passed to the UpdateQueue for crash
     * diagnostics. If the callback is skipped, a trace log is emitted with the tag.
     *
     * @tparam F Callable with signature void()
     * @param tag String literal identifying the caller (for crash diagnostics)
     * @param fn The callback to defer
     */
    template <typename F>
    void defer(const char* tag, F&& fn) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        helix::ui::queue_update(tag, [gen, snapshot, tag, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot) {
                spdlog::trace("[AsyncLifetimeGuard] Skipped expired callback: {}",
                              tag ? tag : "unknown");
                return;
            }
            f();
        });
    }

  private:
    std::shared_ptr<std::atomic<uint64_t>> gen_;
};

} // namespace helix
