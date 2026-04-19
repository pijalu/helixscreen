// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "filament_slot_override.h"

class IMoonrakerAPI;

namespace helix::ams {

class FilamentSlotOverrideStore {
  public:
    FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id);

    // Blocking load from Moonraker database (called only at backend init time).
    // Later tasks will add local-cache fallback.
    std::unordered_map<int, FilamentSlotOverride> load_blocking();

    using SaveCallback = std::function<void(bool success, std::string error)>;
    void save_async(int slot_index, const FilamentSlotOverride& override, SaveCallback cb);
    void clear_async(int slot_index, SaveCallback cb);

    const std::string& backend_id() const {
        return backend_id_;
    }

  private:
    IMoonrakerAPI* api_;
    std::string backend_id_;
    std::string namespace_ = "filament_slots";
    std::string key() const {
        return backend_id_ + ":slots";
    }
};

} // namespace helix::ams
