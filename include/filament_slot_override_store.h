// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>

#include "filament_slot_override.h"

class IMoonrakerAPI;
class FilamentSlotOverrideStoreTestAccess;

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
    // Test-only access to mutate load_timeout_ without exposing a public
    // setter. Per L065, prefer friend-class over test-only public methods.
    friend class ::FilamentSlotOverrideStoreTestAccess;

    IMoonrakerAPI* api_;
    std::string backend_id_;
    // Adopts the AFC/OrcaSlicer lane_data Moonraker convention. Each slot is
    // stored under key "laneN" where N is the 1-based slot index (lane1, lane2,
    // ...). Slot index 0 in HelixScreen maps to "lane1" on disk.
    std::string namespace_ = "lane_data";
    // Local timeout for load_blocking()'s cv.wait_for. Defaults to 5 seconds;
    // overridable by FilamentSlotOverrideStoreTestAccess for timeout tests.
    // Stored as milliseconds (not seconds) because tests need sub-second
    // resolution — a chrono::seconds member would truncate 50ms to 0s.
    std::chrono::milliseconds load_timeout_{5000};
    // Returns the Moonraker DB key for a given slot.
    //
    // IMPORTANT: the DB key is 1-based (AFC convention: lane1, lane2, ...) but
    // the "lane" field *inside* each record is 0-based (matches Orca's
    // tool-index interpretation, written by to_lane_data_record in the .cpp).
    // Easy to get wrong — changing one without the other silently breaks
    // interop with AFC and Orca.
    static std::string lane_key(int slot_index) {
        return "lane" + std::to_string(slot_index + 1);
    }
};

} // namespace helix::ams
