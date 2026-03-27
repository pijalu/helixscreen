// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_config_manager.h"

#include "moonraker_api.h"

#include <spdlog/spdlog.h>

AfcConfigManager::AfcConfigManager(MoonrakerAPI* api)
    : api_(api) {}

AfcConfigManager::AfcConfigManager(MoonrakerAPI* api, helix::LifetimeToken token)
    : api_(api), token_(std::move(token)) {}

void AfcConfigManager::load(const std::string& filename, Callback on_done) {
    if (!api_) {
        spdlog::error("[AfcConfigManager] Cannot load '{}': no API connection", filename);
        if (on_done) {
            on_done(false, "No API connection");
        }
        return;
    }

    spdlog::info("[AfcConfigManager] Loading config file: {}", filename);

    auto token = token_;
    api_->transfers().download_file(
        "config", filename,
        // Download success
        [this, token, filename, on_done](const std::string& content) {
            if (token && token->expired()) return;
            spdlog::debug("[AfcConfigManager] Downloaded '{}' ({} bytes)", filename,
                          content.size());

            load_from_string(content, filename);

            if (on_done) {
                on_done(true, "");
            }
        },
        // Download error
        [filename, on_done](const MoonrakerError& err) {
            spdlog::error("[AfcConfigManager] Failed to download '{}': {}", filename, err.message);
            if (on_done) {
                on_done(false, err.message);
            }
        });
}

void AfcConfigManager::save(const std::string& filename, Callback on_done) {
    if (!api_) {
        spdlog::error("[AfcConfigManager] Cannot save '{}': no API connection", filename);
        if (on_done) {
            on_done(false, "No API connection");
        }
        return;
    }

    std::string content = parser_.serialize();
    spdlog::info("[AfcConfigManager] Saving config file: {} ({} bytes)", filename, content.size());

    auto token = token_;
    api_->transfers().upload_file(
        "config", filename, content,
        // Upload success
        [this, token, filename, content, on_done]() {
            if (token && token->expired()) return;
            spdlog::info("[AfcConfigManager] Successfully saved '{}'", filename);

            // Update baseline so discard would revert to this saved state
            original_content_ = content;
            loaded_filename_ = filename;
            dirty_ = false;

            if (on_done) {
                on_done(true, "");
            }
        },
        // Upload error
        [filename, on_done](const MoonrakerError& err) {
            spdlog::error("[AfcConfigManager] Failed to save '{}': {}", filename, err.message);
            if (on_done) {
                on_done(false, err.message);
            }
        });
}

void AfcConfigManager::load_from_string(const std::string& content, const std::string& filename) {
    original_content_ = content;
    loaded_filename_ = filename;
    dirty_ = false;
    loaded_ = true;
    parser_ = KlipperConfigParser();
    parser_.parse(content);
}

KlipperConfigParser& AfcConfigManager::parser() {
    return parser_;
}

const KlipperConfigParser& AfcConfigManager::parser() const {
    return parser_;
}

bool AfcConfigManager::has_unsaved_changes() const {
    return dirty_;
}

void AfcConfigManager::mark_dirty() {
    dirty_ = true;
}

void AfcConfigManager::discard_changes() {
    parser_ = KlipperConfigParser();
    if (!original_content_.empty()) {
        parser_.parse(original_content_);
    }
    dirty_ = false;
}

bool AfcConfigManager::is_loaded() const {
    return loaded_;
}

const std::string& AfcConfigManager::loaded_filename() const {
    return loaded_filename_;
}
