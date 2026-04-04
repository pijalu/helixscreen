// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sound_backend.h"
#include "sound_theme.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// PWM sysfs backend -- generates tones via hardware PWM on embedded Linux (AD5M)
///
/// Two modes of operation:
/// 1. Tone mode: set_tone() writes frequency/duty to sysfs (SFX, system sounds)
/// 2. PCM mode: render thread calls external render source, modulates PWM duty cycle
///    at 16kHz to reproduce audio waveforms (tracker playback)
///
/// Writes to /sys/class/pwm/pwmchipN/pwmM/{period,duty_cycle,enable}
class PWMSoundBackend : public SoundBackend {
  public:
    /// @param base_path  Override sysfs base path (for testing with temp dirs)
    /// @param chip       pwmchip number (e.g. 0 for pwmchip0)
    /// @param channel    PWM channel number (e.g. 6 for pwm6)
    explicit PWMSoundBackend(const std::string& base_path = "/sys/class/pwm", int chip = 0,
                             int channel = 6);
    ~PWMSoundBackend() override;

    // SoundBackend interface
    void set_tone(float freq_hz, float amplitude, float duty_cycle) override;
    void silence() override;
    void set_waveform(Waveform w) override;

    bool supports_waveforms() const override;
    bool supports_amplitude() const override;
    bool supports_filter() const override;
    float min_tick_ms() const override;

    // PCM render source support (tracker playback via duty cycle modulation)
    bool supports_render_source() const override {
        return initialized_;
    }
    void set_render_source(std::function<void(float*, size_t, int)> fn) override;
    void clear_render_source() override;

    /// Initialize: verify sysfs paths exist and are writable
    /// @return false if paths don't exist or aren't writable
    bool initialize();

    /// Shutdown: disable PWM output and stop render thread
    void shutdown();

    /// Get the constructed path to the PWM channel directory
    /// e.g. "/sys/class/pwm/pwmchip0/pwm6"
    std::string channel_path() const;

    /// Convert frequency in Hz to period in nanoseconds
    /// @return period_ns = 1e9 / freq_hz, or 0 if freq_hz <= 0
    static uint32_t freq_to_period_ns(float freq_hz);

    /// Get the base duty cycle ratio for a given waveform type
    /// Square=0.50, Saw=0.25, Triangle=0.35, Sine=0.40
    static float waveform_duty_ratio(Waveform w);

    /// Check if PWM is currently enabled
    bool is_enabled() const;

    /// PCM render sample rate (Hz)
    static constexpr int PCM_SAMPLE_RATE = 16000;

    /// PWM carrier frequency for PCM mode (Hz) — above audible range
    static constexpr int PCM_CARRIER_HZ = 62500;

  private:
    void start_render_thread();
    void stop_render_thread();
    void render_loop();

    /// Switch PWM to PCM carrier frequency (fixed period, variable duty)
    void enter_pcm_mode();

    /// Switch PWM back to tone mode (variable period for frequency control)
    void exit_pcm_mode();

    std::string base_path_;
    int chip_;
    int channel_;
    bool enabled_ = false;
    bool initialized_ = false;
    Waveform current_wave_ = Waveform::SQUARE;

    // PCM render state
    std::function<void(float*, size_t, int)> render_source_;
    std::mutex render_source_mutex_;
    std::thread render_thread_;
    std::atomic<bool> render_running_{false};
    bool in_pcm_mode_ = false;

    // Pre-opened file descriptors for fast sysfs writes in render loop
    int fd_duty_ = -1;
    int fd_period_ = -1;
    int fd_enable_ = -1;

    // Render buffer
    std::vector<float> render_buf_;
};
