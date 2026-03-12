// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"
#include "moonraker_api.h"
#include "moonraker_types.h"
#include "overlay_base.h"
#include "thumbnail_cache.h"

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

class TimelapseVideosOverlay : public OverlayBase {
  public:
    explicit TimelapseVideosOverlay(MoonrakerAPI* api);

    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;

    [[nodiscard]] const char* get_name() const override {
        return "Timelapse Videos";
    }

    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;

    void set_api(MoonrakerAPI* api) { api_ = api; }

  private:
    struct VideoEntry {
        std::string filename;
        std::string file_info; // "12.4 MB · Mar 12"
        uint64_t size = 0;
        double modified = 0.0;
    };

    void fetch_video_list();
    void populate_video_grid(const std::vector<FileInfo>& files);
    void load_thumbnail_for_card(lv_obj_t* card, const std::string& filename,
                                 const std::set<std::string>& available_files);
    void clear_video_grid();

    void detect_playback_capability();
    void play_video(const std::string& filename);

    void confirm_delete(const std::string& filename);

    static void on_render_now(lv_event_t* e);
    static void on_card_clicked(lv_event_t* e);
    static void on_card_long_pressed(lv_event_t* e);
    static void on_card_delete(lv_event_t* e);
    static void on_delete_confirmed(lv_event_t* e);
    static void on_delete_cancelled(lv_event_t* e);

    MoonrakerAPI* api_;
    std::vector<VideoEntry> videos_;
    bool can_play_ = false;
    std::string player_command_;
    bool is_local_moonraker_ = false;

    std::shared_ptr<std::atomic<bool>> alive_;
    std::atomic<uint32_t> nav_generation_{0};
    std::atomic<uint32_t> thumb_generation_{0};

    lv_obj_t* video_grid_container_ = nullptr;
    lv_obj_t* video_grid_empty_ = nullptr;

    std::string pending_delete_filename_;
    lv_obj_t* delete_confirmation_dialog_ = nullptr;
};

TimelapseVideosOverlay& get_global_timelapse_videos();
void init_global_timelapse_videos(MoonrakerAPI* api);
void open_timelapse_videos();
