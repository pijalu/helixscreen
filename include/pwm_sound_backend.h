// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sound_backend.h"
#include "sound_theme.h"

#include <string>

/// PWM sysfs backend -- generates tones via hardware PWM on embedded Linux (AD5M)
///
/// Writes to /sys/class/pwm/pwmchipN/pwmM/{period,duty_cycle,enable}
/// Approximates waveform differences via duty cycle ratios:
///   Square=50%, Saw~25%, Triangle~35%, Sine~40%
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

    /// Initialize: verify sysfs paths exist and are writable
    /// @return false if paths don't exist or aren't writable
    bool initialize();

    /// Shutdown: disable PWM output
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

  private:
    std::string base_path_;
    int chip_;
    int channel_;
    bool enabled_ = false;
    bool initialized_ = false;
    Waveform current_wave_ = Waveform::SQUARE;
};
