// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

/**
 * @file thumbnail_load_context.h
 * @brief Async safety context for thumbnail loading operations
 *
 * ThumbnailLoadContext encapsulates the common pattern used across panels
 * when loading thumbnails asynchronously:
 * 1. A lifetime check to detect if the caller was destroyed
 * 2. A generation counter to detect stale callbacks
 *
 * Supports both the legacy shared_ptr<atomic<bool>> alive pattern and the
 * newer AsyncLifetimeGuard token pattern. When both are set, both must
 * report alive/valid for is_valid() to return true.
 *
 * ## Usage Example (AsyncLifetimeGuard — preferred)
 * ```cpp
 * // In your panel class (OverlayBase provides lifetime_):
 * void load_thumbnail() {
 *     auto ctx = ThumbnailLoadContext::create(lifetime_);
 *
 *     get_thumbnail_cache().fetch_for_detail_view(
 *         api_, path, ctx,
 *         [this, ctx](const std::string& lvgl_path) {
 *             if (!ctx.is_valid()) return;
 *             // ... use lvgl_path ...
 *         });
 * }
 * ```
 *
 * ## Usage Example (legacy alive flag)
 * ```cpp
 * auto ctx = ThumbnailLoadContext::create(m_alive, &thumbnail_gen_);
 * ```
 *
 * @see ThumbnailCache::fetch_for_detail_view
 * @see ThumbnailCache::fetch_for_card_view
 */
struct ThumbnailLoadContext {
    /// Shared flag indicating if the owner object is still alive (legacy pattern)
    std::shared_ptr<std::atomic<bool>> alive;

    /// Lifetime token from AsyncLifetimeGuard (preferred pattern)
    std::optional<helix::LifetimeToken> lifetime_token;

    /// Pointer to the owner's generation counter (may be nullptr if not used)
    std::atomic<uint32_t>* generation = nullptr;

    /// The generation value captured at creation time
    uint32_t captured_gen = 0;

    /**
     * @brief Check if this context is still valid
     *
     * A context is valid if:
     * 1. The lifetime check passes (alive flag true OR lifetime token not expired)
     * 2. The generation counter hasn't changed (no newer request superseded this one)
     *
     * @return true if the callback should proceed, false if it should abort
     */
    [[nodiscard]] bool is_valid() const {
        // Check lifetime: token takes priority if set, otherwise check alive flag
        if (lifetime_token.has_value()) {
            if (lifetime_token->expired()) {
                return false;
            }
        } else if (!alive || !alive->load()) {
            return false;
        }
        // If no generation tracking, always valid
        if (generation == nullptr) {
            return true;
        }
        // Check generation hasn't changed
        return captured_gen == generation->load();
    }

    /**
     * @brief Create a context from AsyncLifetimeGuard, optionally with generation tracking
     *
     * @param guard The AsyncLifetimeGuard from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that can be passed to async callbacks
     */
    static ThumbnailLoadContext create(helix::AsyncLifetimeGuard& guard,
                                       std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.lifetime_token = guard.token();
        ctx.generation = gen;
        ctx.captured_gen = gen ? ++(*gen) : 0;
        return ctx;
    }

    /**
     * @brief Create a context from legacy alive flag, incrementing the generation counter
     *
     * @param alive_flag Shared alive flag from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that can be passed to async callbacks
     */
    static ThumbnailLoadContext create(std::shared_ptr<std::atomic<bool>> alive_flag,
                                       std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.alive = std::move(alive_flag);
        ctx.generation = gen;
        ctx.captured_gen = gen ? ++(*gen) : 0;
        return ctx;
    }

    /**
     * @brief Capture a context from AsyncLifetimeGuard without incrementing generation
     *
     * @param guard The AsyncLifetimeGuard from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that captures current generation without incrementing
     */
    static ThumbnailLoadContext capture(helix::AsyncLifetimeGuard& guard,
                                        std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.lifetime_token = guard.token();
        ctx.generation = gen;
        ctx.captured_gen = gen ? gen->load() : 0;
        return ctx;
    }

    /**
     * @brief Capture a context from legacy alive flag without incrementing generation
     *
     * @param alive_flag Shared alive flag from the calling object
     * @param gen Pointer to generation counter (nullptr if not used)
     * @return A context that captures current generation without incrementing
     */
    static ThumbnailLoadContext capture(std::shared_ptr<std::atomic<bool>> alive_flag,
                                        std::atomic<uint32_t>* gen = nullptr) {
        ThumbnailLoadContext ctx;
        ctx.alive = std::move(alive_flag);
        ctx.generation = gen;
        ctx.captured_gen = gen ? gen->load() : 0;
        return ctx;
    }
};
