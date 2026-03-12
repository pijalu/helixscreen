// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../catch_amalgamated.hpp"

#include "timelapse_thumbnailer.h"

using namespace helix::timelapse;

TEST_CASE("Video list filters correctly", "[timelapse][videos]") {
    SECTION("accepts standard video extensions") {
        REQUIRE(TimelapseThumbnailer::is_video_file("print_001.mp4"));
        REQUIRE(TimelapseThumbnailer::is_video_file("TIMELAPSE.MKV"));
        REQUIRE(TimelapseThumbnailer::is_video_file("video.avi"));
    }

    SECTION("rejects non-video files") {
        REQUIRE_FALSE(TimelapseThumbnailer::is_video_file("frame_001.jpg"));
        REQUIRE_FALSE(TimelapseThumbnailer::is_video_file("benchy.thumb.jpg"));
        REQUIRE_FALSE(TimelapseThumbnailer::is_video_file("config.cfg"));
        REQUIRE_FALSE(TimelapseThumbnailer::is_video_file("readme.txt"));
    }

    SECTION("rejects edge cases") {
        REQUIRE_FALSE(TimelapseThumbnailer::is_video_file(""));
        REQUIRE_FALSE(TimelapseThumbnailer::is_video_file(".mp4"));
        REQUIRE_FALSE(TimelapseThumbnailer::is_video_file("mp4"));
    }
}

TEST_CASE("Playback: local vs remote detection", "[timelapse][videos][playback]") {
    SECTION("localhost is local") {
        REQUIRE(is_local_host("127.0.0.1"));
        REQUIRE(is_local_host("localhost"));
    }

    SECTION("remote hosts are not local") {
        REQUIRE_FALSE(is_local_host("192.168.1.100"));
        REQUIRE_FALSE(is_local_host("printer.local"));
    }
}

TEST_CASE("Playback: command construction", "[timelapse][videos][playback]") {
    SECTION("mpv command") {
        auto cmd = build_player_command("mpv", "/tmp/video.mp4");
        REQUIRE(cmd.find("mpv") != std::string::npos);
        REQUIRE(cmd.find("--fs") != std::string::npos);
        REQUIRE(cmd.find("/tmp/video.mp4") != std::string::npos);
    }

    SECTION("ffplay command") {
        auto cmd = build_player_command("ffplay", "/tmp/video.mp4");
        REQUIRE(cmd.find("ffplay") != std::string::npos);
        REQUIRE(cmd.find("-autoexit") != std::string::npos);
        REQUIRE(cmd.find("-fs") != std::string::npos);
    }
}
