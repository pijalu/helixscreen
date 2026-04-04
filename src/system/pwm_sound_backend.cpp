// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pwm_sound_backend.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

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
    initialized_ = true;
    return true;
}

void PWMSoundBackend::shutdown() {
    if (!initialized_) {
        return;
    }
    silence();
    initialized_ = false;
}

void PWMSoundBackend::set_tone(float freq_hz, float amplitude, float /* duty_cycle */) {
    if (!initialized_) {
        return;
    }

    // Clamp amplitude to [0, 1]
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);

    // Zero amplitude or zero/negative frequency -> silence
    if (amplitude == 0.0f || freq_hz <= 0.0f) {
        silence();
        return;
    }

    uint32_t period_ns = freq_to_period_ns(freq_hz);
    if (period_ns == 0) {
        silence();
        return;
    }

    float ratio = waveform_duty_ratio(current_wave_);
    uint32_t duty_ns = static_cast<uint32_t>(static_cast<float>(period_ns) * ratio * amplitude);

    std::string path = channel_path();

    // Write period first, then duty_cycle, then enable (sysfs order matters)
    std::ofstream(path + "/period") << period_ns;
    std::ofstream(path + "/duty_cycle") << duty_ns;

    // Only write enable if not already enabled (avoid redundant writes)
    if (!enabled_) {
        std::ofstream(path + "/enable") << "1";
        enabled_ = true;
    }
}

void PWMSoundBackend::silence() {
    if (!initialized_) {
        return;
    }

    std::string path = channel_path();
    std::ofstream(path + "/enable") << "0";
    enabled_ = false;
}

void PWMSoundBackend::set_waveform(Waveform w) {
    if (!initialized_) {
        return;
    }
    current_wave_ = w;
}
