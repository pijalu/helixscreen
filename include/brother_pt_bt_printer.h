// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <string>
#include <vector>

struct SpoolInfo;
enum class LabelPreset;

namespace helix::label {

/// Brother PT-series (P-Touch) Bluetooth label printer.
/// Uses RFCOMM transport with auto-detected tape width from status query.
class BrotherPTBluetoothPrinter : public ILabelPrinter {
  public:
    void set_device(const std::string& mac, int channel = 1);

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size,
               PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

    /// Deferred-render print: queries tape, renders at correct size, prints.
    void print_spool(const SpoolInfo& spool, LabelPreset preset,
                     PrintCallback callback);

    /// All supported TZe tape sizes (for settings UI)
    static std::vector<LabelSize> supported_sizes_static();

  private:
    std::string mac_;
    int channel_ = 1;
};

}  // namespace helix::label
