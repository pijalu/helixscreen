// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pwm_sound_backend.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

// Buffer size for render loop (frames per render call)
static constexpr size_t RENDER_BUFFER_FRAMES = 128;

PWMSoundBackend::PWMSoundBackend(const std::string& base_path, int chip, int channel)
    : base_path_(base_path), chip_(chip), channel_(channel) {}

PWMSoundBackend::~PWMSoundBackend() {
    shutdown();
}

std::string PWMSoundBackend::channel_path() const {
    return base_path_ + "/pwmchip" + std::to_string(chip_) + "/pwm" + std::to_string(channel_);
}

uint32_t PWMSoundBackend::freq_to_period_ns(float freq_hz) {
    if (freq_hz <= 0.0f) {
        return 0;
    }
    return static_cast<uint32_t>(1e9f / freq_hz);
}

float PWMSoundBackend::waveform_duty_ratio(Waveform w) {
    switch (w) {
    case Waveform::SQUARE:
        return 0.50f;
    case Waveform::SAW:
        return 0.25f;
    case Waveform::TRIANGLE:
        return 0.35f;
    case Waveform::SINE:
        return 0.40f;
    }
    return 0.50f;
}

bool PWMSoundBackend::supports_waveforms() const {
    return false;
}

bool PWMSoundBackend::supports_amplitude() const {
    return true;
}

bool PWMSoundBackend::supports_filter() const {
    return false;
}

float PWMSoundBackend::min_tick_ms() const {
    return 2.0f;
}

bool PWMSoundBackend::is_enabled() const {
    return enabled_;
}

bool PWMSoundBackend::initialize() {
    std::string path = channel_path();
    if (!std::filesystem::exists(path)) {
        return false;
    }

    // Pre-open file descriptors for fast writes in render loop
    fd_duty_ = ::open((path + "/duty_cycle").c_str(), O_WRONLY);
    fd_period_ = ::open((path + "/period").c_str(), O_WRONLY);
    fd_enable_ = ::open((path + "/enable").c_str(), O_WRONLY);

    if (fd_duty_ < 0 || fd_period_ < 0 || fd_enable_ < 0) {
        spdlog::warn("[PWMSoundBackend] Failed to open sysfs fds, falling back to ofstream");
        if (fd_duty_ >= 0)
            ::close(fd_duty_);
        if (fd_period_ >= 0)
            ::close(fd_period_);
        if (fd_enable_ >= 0)
            ::close(fd_enable_);
        fd_duty_ = fd_period_ = fd_enable_ = -1;
    }

    render_buf_.resize(RENDER_BUFFER_FRAMES);

    initialized_ = true;
    return true;
}

void PWMSoundBackend::shutdown() {
    if (!initialized_) {
        return;
    }

    stop_render_thread();
    silence();

    if (fd_duty_ >= 0)
        ::close(fd_duty_);
    if (fd_period_ >= 0)
        ::close(fd_period_);
    if (fd_enable_ >= 0)
        ::close(fd_enable_);
    fd_duty_ = fd_period_ = fd_enable_ = -1;

    initialized_ = false;
}

// ============================================================================
// Tone mode (SFX, system sounds) — variable frequency via period
// ============================================================================

static void sysfs_write_fd(int fd, int value) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", value);
    ::lseek(fd, 0, SEEK_SET);
    ::write(fd, buf, len);
}

void PWMSoundBackend::set_tone(float freq_hz, float amplitude, float /* duty_cycle */) {
    if (!initialized_) {
        return;
    }

    amplitude = std::clamp(amplitude, 0.0f, 1.0f);

    if (amplitude == 0.0f || freq_hz <= 0.0f) {
        silence();
        return;
    }

    // If in PCM mode, exit first (tone mode needs variable period)
    if (in_pcm_mode_) {
        exit_pcm_mode();
    }

    uint32_t period_ns = freq_to_period_ns(freq_hz);
    if (period_ns == 0) {
        silence();
        return;
    }

    float ratio = waveform_duty_ratio(current_wave_);
    uint32_t duty_ns = static_cast<uint32_t>(static_cast<float>(period_ns) * ratio * amplitude);

    if (fd_duty_ >= 0) {
        // Must disable before period change to avoid EINVAL when new period < old duty
        if (enabled_) {
            sysfs_write_fd(fd_enable_, 0);
            enabled_ = false;
        }
        sysfs_write_fd(fd_duty_, 0);
        sysfs_write_fd(fd_period_, static_cast<int>(period_ns));
        sysfs_write_fd(fd_duty_, static_cast<int>(duty_ns));
        sysfs_write_fd(fd_enable_, 1);
        enabled_ = true;
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/period") << period_ns;
        std::ofstream(path + "/duty_cycle") << duty_ns;
        if (!enabled_) {
            std::ofstream(path + "/enable") << "1";
            enabled_ = true;
        }
    }
}

void PWMSoundBackend::silence() {
    if (!initialized_) {
        return;
    }

    if (fd_enable_ >= 0) {
        sysfs_write_fd(fd_enable_, 0);
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/enable") << "0";
    }
    enabled_ = false;
}

void PWMSoundBackend::set_waveform(Waveform w) {
    if (!initialized_) {
        return;
    }
    current_wave_ = w;
}

// ============================================================================
// PCM mode (tracker playback) — fixed carrier, variable duty cycle
// ============================================================================

void PWMSoundBackend::set_render_source(std::function<void(float*, size_t, int)> fn) {
    {
        std::lock_guard<std::mutex> lock(render_source_mutex_);
        render_source_ = std::move(fn);
    }
    start_render_thread();
}

void PWMSoundBackend::clear_render_source() {
    stop_render_thread();
    {
        std::lock_guard<std::mutex> lock(render_source_mutex_);
        render_source_ = nullptr;
    }
}

void PWMSoundBackend::enter_pcm_mode() {
    if (in_pcm_mode_)
        return;

    uint32_t carrier_period = static_cast<uint32_t>(1e9 / PCM_CARRIER_HZ);

    if (fd_duty_ >= 0) {
        sysfs_write_fd(fd_enable_, 0);
        sysfs_write_fd(fd_duty_, 0);
        sysfs_write_fd(fd_period_, static_cast<int>(carrier_period));
        sysfs_write_fd(fd_enable_, 1);
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/enable") << "0";
        std::ofstream(path + "/duty_cycle") << "0";
        std::ofstream(path + "/period") << carrier_period;
        std::ofstream(path + "/enable") << "1";
    }

    enabled_ = true;
    in_pcm_mode_ = true;
    spdlog::debug("[PWMSoundBackend] Entered PCM mode (carrier {}Hz, sample rate {}Hz)",
                  PCM_CARRIER_HZ, PCM_SAMPLE_RATE);
}

void PWMSoundBackend::exit_pcm_mode() {
    if (!in_pcm_mode_)
        return;

    silence();
    in_pcm_mode_ = false;
    spdlog::debug("[PWMSoundBackend] Exited PCM mode");
}

void PWMSoundBackend::start_render_thread() {
    if (render_running_.load())
        return;

    render_running_.store(true);
    render_thread_ = std::thread(&PWMSoundBackend::render_loop, this);
    spdlog::info("[PWMSoundBackend] PCM render thread started");
}

void PWMSoundBackend::stop_render_thread() {
    if (!render_running_.load())
        return;

    render_running_.store(false);
    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    if (in_pcm_mode_) {
        exit_pcm_mode();
    }

    spdlog::info("[PWMSoundBackend] PCM render thread stopped");
}

void PWMSoundBackend::render_loop() {
    enter_pcm_mode();

    const uint32_t carrier_period = static_cast<uint32_t>(1e9 / PCM_CARRIER_HZ);
    const long sample_interval_ns = 1000000000L / PCM_SAMPLE_RATE;

    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    long base_ns = start_ts.tv_sec * 1000000000L + start_ts.tv_nsec;
    long sample_index = 0;

    while (render_running_.load()) {
        // Get render source under lock
        std::function<void(float*, size_t, int)> source;
        {
            std::lock_guard<std::mutex> lock(render_source_mutex_);
            source = render_source_;
        }

        if (!source) {
            struct timespec sl = {0, 1000000}; // 1ms
            nanosleep(&sl, nullptr);
            // Reset timing base so we don't try to "catch up" when source appears
            clock_gettime(CLOCK_MONOTONIC, &start_ts);
            base_ns = start_ts.tv_sec * 1000000000L + start_ts.tv_nsec;
            sample_index = 0;
            continue;
        }

        // Render a buffer of samples
        size_t frames = render_buf_.size();
        std::memset(render_buf_.data(), 0, frames * sizeof(float));
        source(render_buf_.data(), frames, PCM_SAMPLE_RATE);

        // Output each sample at precise intervals via duty cycle modulation
        for (size_t i = 0; i < frames && render_running_.load(); i++) {
            float sample = std::clamp(render_buf_[i], -1.0f, 1.0f);

            // Map [-1, 1] → duty [1%, 99%] of carrier period
            float normalized = (sample + 1.0f) / 2.0f;
            int duty = static_cast<int>(carrier_period * (0.01f + normalized * 0.98f));

            if (fd_duty_ >= 0) {
                sysfs_write_fd(fd_duty_, duty);
            }

            sample_index++;

            // Wait for next sample time using nanosleep
            long target_ns = base_ns + sample_index * sample_interval_ns;
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            long now_ns = now_ts.tv_sec * 1000000000L + now_ts.tv_nsec;
            long remaining = target_ns - now_ns;

            if (remaining > 1000) { // > 1us
                struct timespec sl = {0, remaining};
                nanosleep(&sl, nullptr);
            }
        }
    }

    // Silence on exit
    if (fd_duty_ >= 0) {
        sysfs_write_fd(fd_duty_, 0);
    }
}
