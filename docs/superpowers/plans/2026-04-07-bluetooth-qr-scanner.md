# Bluetooth QR Scanner (HID Mode) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Bluetooth discovery and pairing for HID-mode QR/barcode scanners to the scanner picker modal, so BT scanners can be selected alongside USB scanners in a unified list.

**Architecture:** Extend the existing `ScannerPickerModal` with BT discovery (reusing `BluetoothLoader`) and pairing (reusing the label printer's BlueZ pair+trust flow). Add a `bus_type` field to `UsbHidDevice` so the UI can show a Bluetooth icon badge. Add `scanner/bt_address` to `SettingsManager` for BT scanner MAC persistence. Create `bt_scanner_discovery_utils.h` with HID UUID + name-based scanner classification (parallel to `bt_discovery_utils.h` for printers).

**Tech Stack:** C++17, LVGL 9.5 XML, BluetoothLoader plugin (dlopen), BlueZ D-Bus (via plugin), spdlog

**Spec:** `docs/superpowers/specs/2026-04-07-bluetooth-qr-scanner-design.md`

---

### Task 1: Add `bus_type` field to `UsbHidDevice` struct

The scanner picker needs to distinguish USB from BT devices to show a Bluetooth badge. Add a `bus_type` field to the existing struct and populate it during enumeration.

**Files:**
- Modify: `include/input_device_scanner.h:22-27` (UsbHidDevice struct)
- Modify: `src/api/input_device_scanner.cpp:434-476` (enumerate_usb_hid_devices)

- [ ] **Step 1: Add bus_type field to UsbHidDevice**

In `include/input_device_scanner.h`, add a `bus_type` field to the `UsbHidDevice` struct:

```cpp
/// USB HID device with vendor/product identification for manual scanner selection.
struct UsbHidDevice {
    std::string name;         // e.g., "TMS HIDKeyBoard"
    std::string vendor_id;    // e.g., "1a2c" (hex from sysfs)
    std::string product_id;   // e.g., "4c5e" (hex from sysfs)
    std::string event_path;   // e.g., "/dev/input/event5"
    int bus_type = 0;         // BUS_USB=0x03, BUS_BLUETOOTH=0x05
};
```

- [ ] **Step 2: Populate bus_type in enumerate_usb_hid_devices()**

In `src/api/input_device_scanner.cpp`, modify the `enumerate_usb_hid_devices()` function. At line 471, include `bus` in the push_back:

Change:
```cpp
        devices.push_back({std::move(name), std::move(vendor), std::move(product),
                           std::move(device_path)});
```
To:
```cpp
        devices.push_back({std::move(name), std::move(vendor), std::move(product),
                           std::move(device_path), bus});
```

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add include/input_device_scanner.h src/api/input_device_scanner.cpp
git commit -m "feat(scanner): add bus_type field to UsbHidDevice for BT badge support"
```

---

### Task 2: Add BT scanner UUID classification helpers

Create `bt_scanner_discovery_utils.h` with HID UUID matching and scanner name-based fallback detection. This parallels `bt_discovery_utils.h` for label printers.

**Files:**
- Create: `include/bt_scanner_discovery_utils.h`
- Create: `tests/unit/test_bt_scanner_discovery_utils.cpp`

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/test_bt_scanner_discovery_utils.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bt_scanner_discovery_utils.h"

#include "../catch_amalgamated.hpp"

using namespace helix::bluetooth;

// ============================================================================
// is_hid_scanner_uuid
// ============================================================================

TEST_CASE("is_hid_scanner_uuid - matches classic HID UUID", "[bluetooth][scanner]") {
    REQUIRE(is_hid_scanner_uuid("00001124-0000-1000-8000-00805f9b34fb"));
}

TEST_CASE("is_hid_scanner_uuid - matches HID prefix only", "[bluetooth][scanner]") {
    REQUIRE(is_hid_scanner_uuid("00001124"));
}

TEST_CASE("is_hid_scanner_uuid - matches HID-over-GATT UUID", "[bluetooth][scanner]") {
    REQUIRE(is_hid_scanner_uuid("00001812-0000-1000-8000-00805f9b34fb"));
}

TEST_CASE("is_hid_scanner_uuid - matches HOGP prefix only", "[bluetooth][scanner]") {
    REQUIRE(is_hid_scanner_uuid("00001812"));
}

TEST_CASE("is_hid_scanner_uuid - case insensitive", "[bluetooth][scanner]") {
    REQUIRE(is_hid_scanner_uuid("00001124-0000-1000-8000-00805F9B34FB"));
    REQUIRE(is_hid_scanner_uuid("00001812-0000-1000-8000-00805F9B34FB"));
}

TEST_CASE("is_hid_scanner_uuid - rejects null", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_hid_scanner_uuid(nullptr));
}

TEST_CASE("is_hid_scanner_uuid - rejects empty string", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_hid_scanner_uuid(""));
}

TEST_CASE("is_hid_scanner_uuid - rejects label printer UUIDs", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_hid_scanner_uuid("00001101-0000-1000-8000-00805f9b34fb")); // SPP
    REQUIRE_FALSE(is_hid_scanner_uuid("0000ff00-0000-1000-8000-00805f9b34fb")); // Phomemo
}

TEST_CASE("is_hid_scanner_uuid - rejects random UUID", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_hid_scanner_uuid("deadbeef-1234-5678-9abc-def012345678"));
}

// ============================================================================
// is_likely_bt_scanner
// ============================================================================

TEST_CASE("is_likely_bt_scanner - matches 'barcode' in name", "[bluetooth][scanner]") {
    REQUIRE(is_likely_bt_scanner("CT10 Barcode Scanner"));
    REQUIRE(is_likely_bt_scanner("barcode reader"));
}

TEST_CASE("is_likely_bt_scanner - matches 'scanner' in name", "[bluetooth][scanner]") {
    REQUIRE(is_likely_bt_scanner("Wireless Scanner"));
    REQUIRE(is_likely_bt_scanner("BT scanner pro"));
}

TEST_CASE("is_likely_bt_scanner - matches known brands", "[bluetooth][scanner]") {
    REQUIRE(is_likely_bt_scanner("Tera HW0002"));
    REQUIRE(is_likely_bt_scanner("Netum C750"));
    REQUIRE(is_likely_bt_scanner("Symcode MJ-2877"));
    REQUIRE(is_likely_bt_scanner("Inateck BCST-70"));
    REQUIRE(is_likely_bt_scanner("Eyoyo EY-015"));
}

TEST_CASE("is_likely_bt_scanner - case insensitive", "[bluetooth][scanner]") {
    REQUIRE(is_likely_bt_scanner("TERA HW0002"));
    REQUIRE(is_likely_bt_scanner("BARCODE SCANNER"));
}

TEST_CASE("is_likely_bt_scanner - rejects null", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_likely_bt_scanner(nullptr));
}

TEST_CASE("is_likely_bt_scanner - rejects empty string", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_likely_bt_scanner(""));
}

TEST_CASE("is_likely_bt_scanner - rejects label printers", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_likely_bt_scanner("Brother QL-800"));
    REQUIRE_FALSE(is_likely_bt_scanner("Phomemo M110"));
    REQUIRE_FALSE(is_likely_bt_scanner("Niimbot B21"));
}

TEST_CASE("is_likely_bt_scanner - rejects generic keyboards", "[bluetooth][scanner]") {
    REQUIRE_FALSE(is_likely_bt_scanner("Logitech K380"));
    REQUIRE_FALSE(is_likely_bt_scanner("Apple Magic Keyboard"));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[bluetooth][scanner]" -v`
Expected: FAIL — `bt_scanner_discovery_utils.h` not found.

- [ ] **Step 3: Write the implementation**

Create `include/bt_scanner_discovery_utils.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file bt_scanner_discovery_utils.h
 * @brief Bluetooth discovery UUID classification for HID barcode/QR scanners.
 *
 * Parallels bt_discovery_utils.h (label printers). Provides UUID-based and
 * name-based classification for BT HID scanners.
 */

#include <cstring>

#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

namespace helix::bluetooth {

/// HID UUID prefix (Classic Bluetooth HID Profile)
inline constexpr const char* HID_UUID_PREFIX = "00001124";
/// HID-over-GATT UUID prefix (BLE HID)
inline constexpr const char* HOGP_UUID_PREFIX = "00001812";

/// Check if a UUID matches a HID scanner service UUID.
/// Matches classic HID (0x1124) and HID-over-GATT (0x1812).
inline bool is_hid_scanner_uuid(const char* uuid)
{
    if (!uuid || !uuid[0]) return false;
    if (strncasecmp(uuid, HID_UUID_PREFIX, 8) == 0) return true;
    if (strncasecmp(uuid, HOGP_UUID_PREFIX, 8) == 0) return true;
    return false;
}

/// Known scanner brand prefixes for name-based fallback detection.
inline constexpr const char* KNOWN_SCANNER_BRANDS[] = {
    "Tera",
    "Netum",
    "Symcode",
    "Inateck",
    "Eyoyo",
};

/// Check if a device name looks like a barcode/QR scanner.
/// Uses keyword matching ("barcode", "scanner") and known brand names.
/// Rejects known label printer names to avoid false positives.
inline bool is_likely_bt_scanner(const char* name)
{
    if (!name || !name[0]) return false;

    // Reject known label printers first (they may contain "scanner" in some configs)
    if (is_likely_label_printer(name)) return false;

    // Keyword match
    if (strcasestr(name, "barcode") != nullptr) return true;
    if (strcasestr(name, "scanner") != nullptr) return true;

    // Known brand match
    for (const auto* brand : KNOWN_SCANNER_BRANDS) {
        if (strncasecmp(name, brand, strlen(brand)) == 0) return true;
    }

    return false;
}

}  // namespace helix::bluetooth
```

Note: This file includes `is_likely_label_printer()` from `bt_discovery_utils.h`. Add the include at the top:

```cpp
#include "bt_discovery_utils.h"
```

(Place this after the `#include <cstring>` line, before the namespace.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[bluetooth][scanner]" -v`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```bash
git add include/bt_scanner_discovery_utils.h tests/unit/test_bt_scanner_discovery_utils.cpp
git commit -m "feat(scanner): add BT scanner UUID and name classification helpers"
```

---

### Task 3: Add `scanner/bt_address` to SettingsManager

Persist the paired BT scanner's MAC address so it can be shown as "(Saved)" in the picker and used for reconnection filtering.

**Files:**
- Modify: `include/settings_manager.h:262-277` (add getter/setter declarations)
- Modify: `src/system/settings_manager.cpp` (add implementation + config load)

- [ ] **Step 1: Add declarations to settings_manager.h**

In `include/settings_manager.h`, after line 276 (`set_scanner_device_name`), add:

```cpp
    /** @brief Get configured BT scanner MAC address (empty = none) */
    std::string get_scanner_bt_address() const;

    /** @brief Set configured BT scanner MAC address (empty = clear) */
    void set_scanner_bt_address(const std::string& address);
```

Also add the member variable. Find the private section where `scanner_device_id_` and `scanner_device_name_` are declared and add:

```cpp
    std::string scanner_bt_address_;
```

- [ ] **Step 2: Add implementation to settings_manager.cpp**

In `src/system/settings_manager.cpp`, after the `set_scanner_device_name()` implementation (around line 543), add:

```cpp
std::string SettingsManager::get_scanner_bt_address() const {
    return scanner_bt_address_;
}

void SettingsManager::set_scanner_bt_address(const std::string& address) {
    spdlog::info("[SettingsManager] set_scanner_bt_address({})", address);
    scanner_bt_address_ = address;
    auto config = helix::Config::instance();
    config->set<std::string>(config->df() + "scanner/bt_address", address);
    config->save();
}
```

In the init/load section (around line 157, after `scanner_device_name_` is loaded), add:

```cpp
    scanner_bt_address_ = config->get<std::string>(config->df() + "scanner/bt_address", "");
    if (!scanner_bt_address_.empty()) {
        spdlog::info("[SettingsManager] Loaded scanner BT address: {}", scanner_bt_address_);
    }
```

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add include/settings_manager.h src/system/settings_manager.cpp
git commit -m "feat(scanner): add scanner/bt_address setting for BT scanner persistence"
```

---

### Task 4: Add BT discovery and pairing to ScannerPickerModal

This is the main task. Extend `ScannerPickerModal` with BT discovery, device list merging, pairing confirmation, and settings persistence.

**Files:**
- Modify: `include/ui_modal_scanner_picker.h`
- Modify: `src/ui/ui_modal_scanner_picker.cpp`

- [ ] **Step 1: Update the header with BT support**

Replace the contents of `include/ui_modal_scanner_picker.h` with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "input_device_scanner.h"
#include "ui_modal.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct helix_bt_context;

namespace helix::ui {

/// Modal that lists connected USB and Bluetooth HID devices for scanner selection.
/// USB devices are enumerated from sysfs. BT devices are discovered via BluetoothLoader.
/// Shows auto-detect option at top, then each discovered device with transport badge.
class ScannerPickerModal : public Modal {
  public:
    using SelectionCallback = std::function<void(const std::string& vendor_product,
                                                 const std::string& device_name)>;

    explicit ScannerPickerModal(SelectionCallback on_select);
    ~ScannerPickerModal() override;

    const char* get_name() const override {
        return "Scanner Picker";
    }
    const char* component_name() const override {
        return "scanner_picker_modal";
    }

  protected:
    void on_show() override;

  private:
    // Device list
    void populate_device_list();
    void add_device_row(lv_obj_t* list, const std::string& label, const std::string& sublabel,
                        const std::string& vendor_product, bool is_bluetooth = false,
                        const std::string& bt_mac = "");
    void handle_device_selected(const std::string& vendor_product, const std::string& device_name,
                                const std::string& bt_mac);

    // Bluetooth discovery
    void start_bt_discovery();
    void stop_bt_discovery();
    void handle_bt_device_found(const std::string& mac, const std::string& name,
                                bool paired, bool is_ble);
    void handle_bt_discovery_finished();

    // Bluetooth pairing
    void pair_bt_device(const std::string& mac, const std::string& name);

    // Pairing modal callbacks
    static void on_pair_confirm(lv_event_t* e);
    static void on_pair_cancel(lv_event_t* e);

    SelectionCallback on_select_;
    std::string current_device_id_;
    lv_obj_t* device_list_ = nullptr;
    lv_obj_t* empty_state_ = nullptr;
    lv_obj_t* bt_scan_btn_ = nullptr;
    lv_obj_t* bt_spinner_ = nullptr;

    // Bluetooth state
    struct BtDeviceInfo {
        std::string mac;
        std::string name;
        std::string vendor_product; // populated after pairing when device appears as HID
        bool paired = false;
        bool is_ble = false;
    };

    struct BtDiscoveryContext {
        std::atomic<bool> alive{true};
        ScannerPickerModal* modal = nullptr;
    };

    helix_bt_context* bt_ctx_ = nullptr;
    std::unique_ptr<BtDiscoveryContext> bt_discovery_ctx_;
    std::vector<BtDeviceInfo> bt_devices_;
    bool bt_discovering_ = false;
    AsyncLifetimeGuard lifetime_;

    /// Active instance pointer (only one picker modal open at a time)
    static ScannerPickerModal* s_active_instance_;
};

} // namespace helix::ui
```

- [ ] **Step 2: Rewrite the implementation**

Replace the contents of `src/ui/ui_modal_scanner_picker.cpp`. The file is substantial, so here is the complete implementation:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal_scanner_picker.h"

#include "bluetooth_loader.h"
#include "bt_scanner_discovery_utils.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "ui_event_safety.h"
#include "ui_icon_codepoints.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <thread>

namespace helix::ui {

ScannerPickerModal* ScannerPickerModal::s_active_instance_ = nullptr;

// ============================================================================
// RowData — attached to each clickable row via lv_obj_set_user_data()
// ============================================================================

struct RowData {
    std::string vendor_product; // "" = auto-detect
    std::string device_name;
    std::string bt_mac;         // non-empty for BT devices
    ScannerPickerModal* modal;
};

// ============================================================================
// CONSTRUCTION / DESTRUCTION
// ============================================================================

ScannerPickerModal::ScannerPickerModal(SelectionCallback on_select)
    : on_select_(std::move(on_select)) {
    current_device_id_ = helix::SettingsManager::instance().get_scanner_device_id();

    // Register XML callbacks BEFORE show() creates the XML component.
    lv_xml_register_event_cb(nullptr, "on_scanner_refresh", [](lv_event_t* /*e*/) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_scanner_refresh");
        if (s_active_instance_) {
            s_active_instance_->populate_device_list();
        }
        LVGL_SAFE_EVENT_CB_END();
    });

    lv_xml_register_event_cb(nullptr, "on_scanner_bt_scan", [](lv_event_t* /*e*/) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_scanner_bt_scan");
        if (s_active_instance_) {
            if (s_active_instance_->bt_discovering_) {
                s_active_instance_->stop_bt_discovery();
            } else {
                s_active_instance_->start_bt_discovery();
            }
        }
        LVGL_SAFE_EVENT_CB_END();
    });
}

ScannerPickerModal::~ScannerPickerModal() {
    stop_bt_discovery();
    s_active_instance_ = nullptr;

    // Deinit BT context
    if (bt_ctx_) {
        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (loader.deinit) {
            loader.deinit(bt_ctx_);
        }
        bt_ctx_ = nullptr;
    }
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void ScannerPickerModal::on_show() {
    s_active_instance_ = this;

    if (dialog()) {
        lv_obj_set_user_data(dialog(), this);
    }

    wire_cancel_button("btn_close");
    wire_cancel_button("btn_primary");

    device_list_ = find_widget("scanner_device_list");
    empty_state_ = find_widget("scanner_empty_state");
    bt_scan_btn_ = find_widget("btn_bt_scan");
    bt_spinner_ = find_widget("bt_scan_spinner");

    // Hide BT scan button if BT hardware not available
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (bt_scan_btn_ && !loader.is_available()) {
        lv_obj_add_flag(bt_scan_btn_, LV_OBJ_FLAG_HIDDEN);
    }
    if (bt_spinner_) {
        lv_obj_add_flag(bt_spinner_, LV_OBJ_FLAG_HIDDEN);
    }

    populate_device_list();

    spdlog::debug("[{}] Shown, current device: '{}'", get_name(),
                  current_device_id_.empty() ? "auto-detect" : current_device_id_);
}

// ============================================================================
// DEVICE LIST POPULATION
// ============================================================================

void ScannerPickerModal::populate_device_list() {
    if (!device_list_) {
        spdlog::warn("[{}] device_list_ widget not found", get_name());
        return;
    }

    lv_obj_clean(device_list_);

    // Auto-detect row
    add_device_row(device_list_, "Auto-detect (default)", "Uses name-based priority", "");

    // Enumerate USB + BT HID devices from sysfs
    auto devices = helix::input::enumerate_usb_hid_devices();
    spdlog::debug("[{}] Found {} HID devices", get_name(), devices.size());

    for (const auto& dev : devices) {
        std::string vendor_product = dev.vendor_id + ":" + dev.product_id;
        bool is_bt = (dev.bus_type == 0x05); // BUS_BLUETOOTH
        std::string sublabel = vendor_product + "  " + dev.event_path;
        if (is_bt) {
            sublabel += "  (Bluetooth)";
        }
        add_device_row(device_list_, dev.name, sublabel, vendor_product, is_bt);
    }

    // Add any BT-discovered devices that aren't yet connected (no /dev/input entry)
    auto saved_bt_mac = helix::SettingsManager::instance().get_scanner_bt_address();
    for (const auto& bt_dev : bt_devices_) {
        // Check if this BT device is already in the sysfs list
        // (it would show up there if connected as HID)
        bool already_listed = false;
        for (const auto& dev : devices) {
            if (dev.bus_type == 0x05 && dev.name == bt_dev.name) {
                already_listed = true;
                break;
            }
        }
        if (!already_listed) {
            std::string status;
            if (bt_dev.paired) {
                status = " (Paired)";
            } else if (bt_dev.mac == saved_bt_mac) {
                status = " (Saved)";
            }
            add_device_row(device_list_, bt_dev.name + status,
                           "Bluetooth  " + bt_dev.mac, bt_dev.vendor_product,
                           true, bt_dev.mac);
        }
    }

    // Show/hide empty state
    bool has_devices = !devices.empty() || !bt_devices_.empty();
    if (empty_state_) {
        if (has_devices) {
            lv_obj_add_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// ROW CREATION
// ============================================================================

void ScannerPickerModal::add_device_row(lv_obj_t* list, const std::string& label,
                                         const std::string& sublabel,
                                         const std::string& vendor_product,
                                         bool is_bluetooth,
                                         const std::string& bt_mac) {
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_md"), 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    // Highlight selected device
    bool is_selected = (vendor_product == current_device_id_) && !vendor_product.empty();
    // Also highlight if this is the saved BT device and no vendor_product match
    if (!is_selected && !bt_mac.empty()) {
        is_selected = (bt_mac == helix::SettingsManager::instance().get_scanner_bt_address());
    }

    if (is_selected) {
        auto primary = theme_manager_get_color("primary");
        lv_obj_set_style_bg_color(row, primary, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, primary, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, 8, 0);
    } else if (vendor_product.empty() && current_device_id_.empty()) {
        // Highlight auto-detect when nothing is configured
        auto primary = theme_manager_get_color("primary");
        lv_obj_set_style_bg_color(row, primary, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, primary, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, 8, 0);
    } else {
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    }

    // Bottom border separator
    if (!is_selected && !(vendor_product.empty() && current_device_id_.empty())) {
        auto border_color = theme_manager_get_color("border");
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, border_color, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    }

    // Name row: optional BT icon + label
    lv_obj_t* name_row = lv_obj_create(row);
    lv_obj_set_width(name_row, lv_pct(100));
    lv_obj_set_height(name_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(name_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(name_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(name_row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_gap(name_row, 6, 0);
    lv_obj_set_style_pad_all(name_row, 0, 0);
    lv_obj_set_style_border_width(name_row, 0, 0);
    lv_obj_set_style_bg_opa(name_row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(name_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(name_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (is_bluetooth) {
        lv_obj_t* bt_icon = lv_label_create(name_row);
        lv_label_set_text(bt_icon, icon_bluetooth);
        lv_obj_set_style_text_font(bt_icon, theme_manager_get_font("font_icon_inline"), 0);
        lv_obj_set_style_text_color(bt_icon, theme_manager_get_color("accent"), 0);
        lv_obj_remove_flag(bt_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(bt_icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    lv_obj_t* name_label = lv_label_create(name_row);
    lv_label_set_text(name_label, label.c_str());
    lv_obj_set_style_text_font(name_label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_flex_grow(name_label, 1);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(name_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(name_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Sublabel
    lv_obj_t* sub_label = lv_label_create(row);
    lv_label_set_text(sub_label, sublabel.c_str());
    lv_obj_set_style_text_font(sub_label, theme_manager_get_font("font_small"), 0);
    lv_obj_set_style_text_color(sub_label, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_width(sub_label, lv_pct(100));
    lv_label_set_long_mode(sub_label, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(sub_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sub_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Attach RowData for click handling
    auto* data = new RowData{vendor_product, label, bt_mac, this};
    lv_obj_set_user_data(row, data);

    // Click handler
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* row_obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            auto* d = static_cast<RowData*>(lv_obj_get_user_data(row_obj));
            if (d && d->modal) {
                d->modal->handle_device_selected(d->vendor_product, d->device_name, d->bt_mac);
            }
        },
        LV_EVENT_CLICKED, nullptr);

    // Clean up RowData on delete
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* row_obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            delete static_cast<RowData*>(lv_obj_get_user_data(row_obj));
            lv_obj_set_user_data(row_obj, nullptr);
        },
        LV_EVENT_DELETE, nullptr);
}

// ============================================================================
// SELECTION HANDLING
// ============================================================================

void ScannerPickerModal::handle_device_selected(const std::string& vendor_product,
                                                 const std::string& device_name,
                                                 const std::string& bt_mac) {
    // If this is an unpaired BT device, prompt for pairing first
    if (!bt_mac.empty()) {
        bool is_paired = false;
        bool is_saved = (bt_mac == helix::SettingsManager::instance().get_scanner_bt_address());
        for (const auto& dev : bt_devices_) {
            if (dev.mac == bt_mac) {
                is_paired = dev.paired;
                break;
            }
        }
        if (!is_paired && !is_saved) {
            pair_bt_device(bt_mac, device_name);
            return;
        }
    }

    spdlog::info("[{}] Selected device: '{}' ({}){}", get_name(), device_name,
                 vendor_product.empty() ? "auto-detect" : vendor_product,
                 bt_mac.empty() ? "" : " BT:" + bt_mac);

    // Persist selection
    helix::SettingsManager::instance().set_scanner_device_id(vendor_product);
    helix::SettingsManager::instance().set_scanner_device_name(
        vendor_product.empty() ? "" : device_name);
    if (!bt_mac.empty()) {
        helix::SettingsManager::instance().set_scanner_bt_address(bt_mac);
    } else {
        // Clear BT address if selecting a non-BT device
        helix::SettingsManager::instance().set_scanner_bt_address("");
    }

    if (on_select_) {
        on_select_(vendor_product, device_name);
    }

    s_active_instance_ = nullptr;
    hide();
}

// ============================================================================
// BLUETOOTH DISCOVERY
// ============================================================================

void ScannerPickerModal::start_bt_discovery() {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available() || !loader.discover) {
        spdlog::warn("[{}] BT discovery unavailable", get_name());
        return;
    }

    if (bt_discovering_) {
        spdlog::debug("[{}] BT discovery already in progress", get_name());
        return;
    }

    // Initialize BT context if needed
    if (!bt_ctx_ && loader.init) {
        bt_ctx_ = loader.init();
        if (!bt_ctx_) {
            spdlog::error("[{}] Failed to init BT context", get_name());
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Bluetooth initialization failed"));
            return;
        }
    }

    bt_discovering_ = true;

    // Keep previously paired devices visible during scan
    bt_devices_.erase(std::remove_if(bt_devices_.begin(), bt_devices_.end(),
                                     [](const BtDeviceInfo& d) { return !d.paired; }),
                      bt_devices_.end());

    // Show spinner, update button text
    if (bt_spinner_) {
        lv_obj_remove_flag(bt_spinner_, LV_OBJ_FLAG_HIDDEN);
    }
    if (bt_scan_btn_) {
        // Update button text to "Stop Scan" while discovering
        lv_obj_t* btn_label = lv_obj_find_by_name(bt_scan_btn_, "label");
        if (btn_label) {
            lv_label_set_text(btn_label, lv_tr("Stop Scan"));
        }
    }

    // Set up C callback safety context
    bt_discovery_ctx_ = std::make_unique<BtDiscoveryContext>();
    bt_discovery_ctx_->alive.store(true);
    bt_discovery_ctx_->modal = this;

    auto* disc_ctx = bt_discovery_ctx_.get();
    auto* ctx = bt_ctx_;
    auto token = lifetime_.token();

    std::thread([ctx, disc_ctx, token, &loader]() {
        loader.discover(
            ctx, 15000,
            [](const helix_bt_device* dev, void* user_data) {
                auto* dctx = static_cast<ScannerPickerModal::BtDiscoveryContext*>(user_data);
                if (!dctx->alive.load())
                    return;

                // Filter: only HID devices or scanner-like names
                bool uuid_match = helix::bluetooth::is_hid_scanner_uuid(dev->service_uuid);
                bool name_match = helix::bluetooth::is_likely_bt_scanner(dev->name);
                if (!uuid_match && !name_match)
                    return;

                // Copy device info
                std::string mac = dev->mac ? dev->mac : "";
                std::string name = dev->name ? dev->name : "Unknown";
                bool paired = dev->paired;
                bool is_ble = dev->is_ble;

                helix::ui::queue_update([dctx, mac, name, paired, is_ble]() {
                    if (!dctx->alive.load())
                        return;
                    dctx->modal->handle_bt_device_found(mac, name, paired, is_ble);
                });
            },
            disc_ctx);

        // Discovery completed
        helix::ui::queue_update([disc_ctx, token]() {
            if (token.expired())
                return;
            if (!disc_ctx->alive.load())
                return;
            disc_ctx->modal->handle_bt_discovery_finished();
        });
    }).detach();

    spdlog::info("[{}] Started Bluetooth scanner discovery", get_name());
}

void ScannerPickerModal::stop_bt_discovery() {
    if (!bt_discovering_)
        return;

    if (bt_discovery_ctx_) {
        bt_discovery_ctx_->alive.store(false);
    }

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (bt_ctx_ && loader.stop_discovery) {
        loader.stop_discovery(bt_ctx_);
    }

    bt_discovering_ = false;

    if (bt_spinner_) {
        lv_obj_add_flag(bt_spinner_, LV_OBJ_FLAG_HIDDEN);
    }
    if (bt_scan_btn_) {
        lv_obj_t* btn_label = lv_obj_find_by_name(bt_scan_btn_, "label");
        if (btn_label) {
            lv_label_set_text(btn_label, lv_tr("Scan Bluetooth"));
        }
    }

    spdlog::debug("[{}] Stopped Bluetooth discovery", get_name());
}

void ScannerPickerModal::handle_bt_device_found(const std::string& mac,
                                                 const std::string& name,
                                                 bool paired, bool is_ble) {
    // Deduplicate
    for (const auto& existing : bt_devices_) {
        if (existing.mac == mac) return;
    }

    bt_devices_.push_back({mac, name, "", paired, is_ble});
    spdlog::debug("[{}] BT scanner discovered: {} ({})", get_name(), name, mac);

    // Refresh the device list to show the new device
    populate_device_list();
}

void ScannerPickerModal::handle_bt_discovery_finished() {
    bt_discovering_ = false;

    if (bt_spinner_) {
        lv_obj_add_flag(bt_spinner_, LV_OBJ_FLAG_HIDDEN);
    }
    if (bt_scan_btn_) {
        lv_obj_t* btn_label = lv_obj_find_by_name(bt_scan_btn_, "label");
        if (btn_label) {
            lv_label_set_text(btn_label, lv_tr("Scan Bluetooth"));
        }
    }

    spdlog::info("[{}] BT scanner discovery finished, {} devices found",
                 get_name(), bt_devices_.size());

    populate_device_list();
}

// ============================================================================
// BLUETOOTH PAIRING
// ============================================================================

/// Pairing data passed through confirmation modal user_data
struct PairData {
    std::string mac;
    std::string name;
};

void ScannerPickerModal::pair_bt_device(const std::string& mac, const std::string& name) {
    auto* pair_data = new PairData{mac, name};

    auto msg = fmt::format("{} {}?", lv_tr("Pair with"), name);
    auto* dlg = modal_show_confirmation(
        lv_tr("Pair Bluetooth Scanner"), msg.c_str(), ModalSeverity::Info,
        lv_tr("Pair"), on_pair_confirm, on_pair_cancel, pair_data);

    if (!dlg) {
        delete pair_data;
        spdlog::warn("[{}] Failed to show pairing confirmation modal", get_name());
    }
}

void ScannerPickerModal::on_pair_confirm(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_pair_confirm");

    auto* pair_data = static_cast<PairData*>(lv_event_get_user_data(e));
    std::string mac = pair_data->mac;
    std::string name = pair_data->name;
    delete pair_data;

    // Close confirmation modal
    auto* top = Modal::get_top();
    if (top)
        Modal::hide(top);

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available() || !loader.pair) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Bluetooth not available"));
        LVGL_SAFE_EVENT_CB_END();
        return;
    }

    if (!s_active_instance_) {
        LVGL_SAFE_EVENT_CB_END();
        return;
    }

    auto* bt_ctx = s_active_instance_->bt_ctx_;
    if (!bt_ctx) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Bluetooth not initialized"));
        LVGL_SAFE_EVENT_CB_END();
        return;
    }

    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Pairing..."), 5000);

    auto token = s_active_instance_->lifetime_.token();

    std::thread([mac, name, bt_ctx, token]() {
        auto& ldr = helix::bluetooth::BluetoothLoader::instance();
        int ret = ldr.pair(bt_ctx, mac.c_str());

        bool paired_ok = (ret == 0);
        if (paired_ok && ldr.is_paired) {
            paired_ok = (ldr.is_paired(bt_ctx, mac.c_str()) == 1);
        }

        helix::ui::queue_update([mac, name, paired_ok, token]() {
            if (token.expired())
                return;

            if (!s_active_instance_)
                return;

            if (paired_ok) {
                ToastManager::instance().show(ToastSeverity::SUCCESS,
                    lv_tr("Paired — scanner will connect automatically"), 3000);

                // Update BT device state
                for (auto& dev : s_active_instance_->bt_devices_) {
                    if (dev.mac == mac) {
                        dev.paired = true;
                        break;
                    }
                }

                // Save settings and auto-select the device
                helix::SettingsManager::instance().set_scanner_bt_address(mac);

                // Refresh list to show updated state
                s_active_instance_->populate_device_list();
            } else {
                auto& ldr2 = helix::bluetooth::BluetoothLoader::instance();
                const char* err = ldr2.last_error
                    ? ldr2.last_error(s_active_instance_->bt_ctx_)
                    : "Unknown error";
                spdlog::error("[ScannerPickerModal] Pairing failed: {}", err);
                ToastManager::instance().show(ToastSeverity::ERROR,
                    lv_tr("Pairing failed"), 3000);
            }
        });
    }).detach();

    LVGL_SAFE_EVENT_CB_END();
}

void ScannerPickerModal::on_pair_cancel(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_pair_cancel");

    auto* pair_data = static_cast<PairData*>(lv_event_get_user_data(e));
    delete pair_data;

    auto* top = Modal::get_top();
    if (top)
        Modal::hide(top);

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
```

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean build, no errors.

- [ ] **Step 4: Commit**

```bash
git add include/ui_modal_scanner_picker.h src/ui/ui_modal_scanner_picker.cpp
git commit -m "feat(scanner): add BT discovery and pairing to scanner picker modal"
```

---

### Task 5: Update scanner picker XML with BT scan button

Add the "Scan Bluetooth" button, spinner, and update empty state text.

**Files:**
- Modify: `ui_xml/scanner_picker_modal.xml`

- [ ] **Step 1: Update the XML**

Replace the contents of `ui_xml/scanner_picker_modal.xml`:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Scanner Device Picker Modal -->
<!-- Lists connected USB and Bluetooth HID devices for barcode scanner selection -->
<component>
  <view name="scanner_picker_modal"
        extends="ui_dialog" width="70%" height="80%" align="center"
        flex_flow="column" style_flex_main_place="start" style_pad_gap="0">
    <!-- Header row: icon + title + close (X) button -->
    <lv_obj width="100%" height="content"
            style_pad_left="#space_lg" style_pad_right="#space_sm" style_pad_top="#space_lg"
            style_pad_bottom="#space_sm" flex_flow="row" style_flex_cross_place="center" style_pad_gap="#space_sm"
            scrollable="false">
      <icon src="qrcode_scan" size="md" variant="accent"/>
      <text_heading text="Barcode Scanner" translation_tag="Barcode Scanner" style_text_align="left" flex_grow="1"/>
      <ui_button name="btn_close" width="40" height="40" variant="ghost" icon="close" focusable="false">
        <event_cb trigger="clicked" callback="on_modal_cancel_clicked"/>
      </ui_button>
    </lv_obj>
    <!-- Scrollable device list -->
    <lv_obj name="scanner_device_list"
            width="100%" flex_grow="1" flex_flow="column" style_pad_gap="0"
            style_pad_left="#space_md" style_pad_right="#space_md"
            style_pad_top="#space_sm" style_pad_bottom="#space_sm"
            scrollable="true" scroll_dir="ver"/>
    <!-- Empty state message (shown when no devices found) -->
    <lv_obj name="scanner_empty_state"
            width="100%" flex_grow="1" style_pad_all="#space_lg" flex_flow="column"
            style_flex_main_place="center" style_flex_cross_place="center"
            style_pad_gap="#space_sm" scrollable="false">
      <text_body name="empty_text"
                 text="No devices detected." translation_tag="No devices detected."
                 style_text_align="center" width="100%"/>
      <text_small text="Plug in a USB scanner and tap Refresh, or scan for Bluetooth devices."
                  translation_tag="Plug in a USB scanner and tap Refresh, or scan for Bluetooth devices."
                  style_text_color="#text_muted" style_text_align="center" width="100%"/>
    </lv_obj>
    <!-- Bottom buttons: BT Scan + Refresh + Cancel -->
    <lv_obj width="100%" height="content" flex_flow="row" style_flex_cross_place="center"
            style_pad_all="#space_md" style_pad_gap="#space_sm" scrollable="false">
      <!-- BT scan spinner (hidden by default) -->
      <lv_spinner name="bt_scan_spinner" width="24" height="24"
                  style_arc_width="3" style_arc_color="#accent"
                  hidden="true"/>
      <!-- Scan Bluetooth button -->
      <ui_button name="btn_bt_scan" text="Scan Bluetooth" translation_tag="Scan Bluetooth"
                 icon="bluetooth" variant="secondary" flex_grow="1">
        <event_cb trigger="clicked" callback="on_scanner_bt_scan"/>
      </ui_button>
      <!-- Refresh (USB) button -->
      <ui_button text="Refresh" translation_tag="Refresh" variant="secondary" flex_grow="1">
        <event_cb trigger="clicked" callback="on_scanner_refresh"/>
      </ui_button>
      <!-- Cancel button -->
      <ui_button name="btn_primary" text="Cancel" translation_tag="Cancel" variant="primary" flex_grow="1">
        <event_cb trigger="clicked" callback="on_modal_cancel_clicked"/>
      </ui_button>
    </lv_obj>
  </view>
</component>
```

Key changes from original:
- Empty state text updated to mention Bluetooth
- Bottom button row changed from `<modal_button_row>` to custom `<lv_obj>` with 3 buttons (BT Scan + Refresh + Cancel) + spinner
- Added `btn_bt_scan` with bluetooth icon
- Added `bt_scan_spinner` (hidden by default)

- [ ] **Step 2: Test hot reload**

Run: `HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv`
Navigate to the scanner picker modal, verify the new layout renders.
Expected: Three buttons at bottom, BT scan button has bluetooth icon.

- [ ] **Step 3: Commit**

```bash
git add ui_xml/scanner_picker_modal.xml
git commit -m "feat(scanner): add Bluetooth scan button to scanner picker modal XML"
```

---

### Task 6: Verify `font_icon_inline` exists for BT badge

The BT badge in device rows uses `theme_manager_get_font("font_icon_inline")`. Verify this font token exists, and if not, use the appropriate existing font.

**Files:**
- None (verification only, may need to adjust `src/ui/ui_modal_scanner_picker.cpp`)

- [ ] **Step 1: Check for font_icon_inline**

Run: `grep -r "font_icon_inline" include/ src/ ui_xml/`

If `font_icon_inline` doesn't exist, check what icon font tokens are available:
Run: `grep -r "font_icon" src/ui/theme_manager.cpp | head -20`

Use the appropriate token (likely `font_icon_sm` or similar). Update the `add_device_row` code in `ui_modal_scanner_picker.cpp` to use the correct font token:

```cpp
lv_obj_set_style_text_font(bt_icon, theme_manager_get_font("font_icon_sm"), 0);
```

- [ ] **Step 2: Build and verify**

Run: `make -j`
Expected: Clean build. If the font token doesn't exist, the fallback is chosen and a spdlog warning appears at runtime.

- [ ] **Step 3: Commit (only if changes needed)**

```bash
git add src/ui/ui_modal_scanner_picker.cpp
git commit -m "fix(scanner): use correct icon font token for BT badge"
```

---

### Task 7: Integration test — full flow manual verification

Run the application and verify the complete BT scanner flow works end-to-end in mock mode.

**Files:**
- None (manual testing)

- [ ] **Step 1: Build**

Run: `make -j`

- [ ] **Step 2: Run in mock mode**

Run: `./build/bin/helix-screen --test -vv`

- [ ] **Step 3: Verify UI flow**

1. Navigate to scanner picker (Settings → Scanner → Pick Scanner, or wherever it's opened from)
2. Verify "Scan Bluetooth" button appears (or is hidden if no BT hardware)
3. Verify "Refresh" and "Cancel" buttons still work
4. Verify empty state text says "No devices detected" / "Plug in a USB scanner..."
5. If BT hardware available: tap "Scan Bluetooth", verify spinner appears, button text changes to "Stop Scan"

- [ ] **Step 4: Run existing tests**

Run: `make test-run`
Expected: All existing tests pass. No regressions.

- [ ] **Step 5: Final commit (if any fixes needed)**

Fix any issues found during manual testing and commit.

---

### Task 8: Add `#include "bt_discovery_utils.h"` guard in scanner utils

Ensure `bt_scanner_discovery_utils.h` properly includes its dependency on `bt_discovery_utils.h` for `is_likely_label_printer()`. This was noted in Task 2 but should be verified.

**Files:**
- Verify: `include/bt_scanner_discovery_utils.h`

- [ ] **Step 1: Verify include**

Read `include/bt_scanner_discovery_utils.h` and confirm it includes `bt_discovery_utils.h` before using `is_likely_label_printer()`.

- [ ] **Step 2: Build with label printer support disabled**

This header is used regardless of `HELIX_HAS_LABEL_PRINTER`, so ensure it compiles in all configurations:

Run: `make -j`
Expected: Clean build.

This task is a verification checkpoint — no code changes expected unless Task 2 missed the include.
