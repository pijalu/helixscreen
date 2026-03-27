# Modal System (Developer Guide)

How the modal dialog system works internally, how to create new modals, and how to migrate old patterns to the standardized system.

**Key files**: `include/ui_modal.h`, `src/ui/ui_modal.cpp`, `src/ui/ui_dialog.cpp`, `ui_xml/modal_dialog.xml`

---

## Architecture Overview

The modal system has four layers:

```
Modal class (C++ RAII lifecycle, show/hide, button wiring)
  |
  +-> ModalStack (singleton, z-order tracking, entrance/exit animations)
  |
  +-> ui_dialog (XML custom widget, theme-aware card background)
  |
  +-> Reusable XML components:
        modal_button_row  (divider + 2-button footer)
        modal_header      (icon + title row)
        modal_dialog      (generic title/message dialog)
```

### How It Works

1. **C++ calls `Modal::show()` or `modal.show(parent)`**
2. A full-screen **backdrop** is created programmatically (semi-transparent overlay)
3. The XML component is instantiated via `lv_xml_create()` inside the backdrop
4. The dialog gets an **entrance animation** (scale + fade)
5. The **ModalStack** tracks the backdrop/dialog pair for z-ordering
6. On hide, an **exit animation** plays, then both backdrop and dialog are destroyed

Backdrops are always created in C++ -- never in XML. This avoids the old pattern of inline XML backdrops that caused double-backdrop bugs and inconsistent behavior.

---

## When to Use What

| Mechanism | Use Case | Examples |
|-----------|----------|---------|
| **Modal** | Blocking user decision, confirmation, form input | Print cancel, Z-offset save, WiFi password, AMS edit |
| **Overlay** | Full-screen or near-full-screen secondary UI | Network settings, print tune, theme editor |
| **Panel** | Main navigation content | Home, controls, settings, print select |

**Rules of thumb:**
- If the user must respond before continuing, use a **Modal**
- If it replaces the current screen but can be "backed out" of, use an **Overlay** (`ui_nav_push_overlay()`)
- If it's a primary navigation destination, use a **Panel**

---

## Three Ways to Create Modals

The system supports three approaches, from simplest to most flexible.

### 1. Confirmation/Alert Helpers (no subclass, no custom XML)

For standard "title + message + buttons" dialogs, use the helper functions. These use the built-in `modal_dialog.xml` component.

```cpp
#include "ui_modal.h"

// Confirmation dialog (two buttons: confirm + cancel)
dialog_ = ui_modal_show_confirmation(
    lv_tr("Delete File?"),
    lv_tr("This cannot be undone."),
    ModalSeverity::Warning,
    lv_tr("Delete"),
    on_confirm_cb, on_cancel_cb, this);

// Alert dialog (single OK button)
ui_modal_show_alert(
    lv_tr("Tip of the Day"),
    lv_tr("You can long-press the home button..."),
    ModalSeverity::Info);
```

`ui_modal_show_confirmation()` returns the dialog widget pointer for cleanup. Store it in a `ModalGuard` for RAII:

```cpp
#include "ui/ui_modal_guard.h"

class MyPanel {
    helix::ui::ModalGuard delete_dialog_;  // Auto-hides in destructor

    void show_delete() {
        delete_dialog_ = ui_modal_show_confirmation(...);
    }
};
```

**Severity levels** control the header icon:
- `ModalSeverity::Info` -- blue info icon
- `ModalSeverity::Warning` -- yellow alert icon
- `ModalSeverity::Error` -- red octagon icon

### 2. Static `Modal::show()` (no subclass, custom XML)

For modals with custom XML layout but no complex C++ logic:

```cpp
// Show a custom XML modal
lv_obj_t* dialog = Modal::show("my_custom_modal");

// Wire up buttons or do work with the dialog...
lv_obj_t* btn = lv_obj_find_by_name(dialog, "btn_primary");

// Later, hide it
Modal::hide(dialog);
```

### 3. Modal Subclass (custom XML + C++ logic)

For modals with complex behavior, subclass `Modal`:

```cpp
// ui_print_cancel_modal.h
class PrintCancelModal : public Modal {
public:
    using ConfirmCallback = std::function<void()>;

    const char* get_name() const override { return "Print Cancel"; }
    const char* component_name() const override { return "print_cancel_confirm_modal"; }

    void set_on_confirm(ConfirmCallback cb) { on_confirm_cb_ = std::move(cb); }

protected:
    void on_show() override {
        wire_ok_button("btn_primary");       // "Stop" button
        wire_cancel_button("btn_secondary"); // "Keep Printing" button
    }

    void on_ok() override {
        if (on_confirm_cb_) on_confirm_cb_();
        hide();
    }

private:
    ConfirmCallback on_confirm_cb_;
};
```

Usage:

```cpp
// In the panel that owns the modal
PrintCancelModal cancel_modal_;

void show_cancel_dialog() {
    cancel_modal_.set_on_confirm([this]() { execute_cancel(); });
    cancel_modal_.show(lv_screen_active());
}
```

The Modal destructor auto-hides if visible, so storing a `Modal` subclass as a member provides RAII cleanup for free.

---

## Modal Subclass API Reference

### Pure Virtuals (must implement)

| Method | Purpose |
|--------|---------|
| `get_name()` | Human-readable name for log messages |
| `component_name()` | XML component name passed to `lv_xml_create()` |

### Lifecycle Hooks (optional overrides)

| Hook | Default | When Called |
|------|---------|------------|
| `on_show()` | no-op | After modal is created and visible |
| `on_hide()` | no-op | Before modal is destroyed |
| `on_ok()` | `hide()` | When primary button is clicked |
| `on_cancel()` | `hide()` | When secondary button is clicked |
| `on_tertiary()` | `hide()` | Third button clicked |
| `on_quaternary()` | `hide()` | Fourth button clicked |
| `on_quinary()` | `hide()` | Fifth button clicked |
| `on_senary()` | `hide()` | Sixth button clicked |

### Button Wiring Helpers

Call these in `on_show()` to connect XML buttons to the hook methods:

```cpp
void on_show() override {
    wire_ok_button("btn_primary");         // -> on_ok()
    wire_cancel_button("btn_secondary");   // -> on_cancel()
    wire_tertiary_button("btn_tertiary");  // -> on_tertiary()
    // ...etc
}
```

The button names must match `name="..."` attributes in your XML. These use `lv_obj_find_by_name()` internally.

**`wire_*_button()` is self-sufficient.** Each call:
1. Sets `user_data` on the button to `this` (the Modal instance)
2. Adds a direct `LV_EVENT_CLICKED` handler that routes to the corresponding virtual method

This means **no XML callback attribute is needed** on the button. `modal_button_row` can omit `primary_callback` / `secondary_callback` entirely when using a Modal subclass:

```xml
<!-- Preferred for Modal subclasses — no callback attributes needed -->
<modal_button_row primary_text="Save" secondary_text="Cancel"/>
```

The generic XML callbacks (`on_modal_ok_clicked`, `on_modal_cancel_clicked`) are retained for the static `Modal::show("component")` API, which doesn't use `wire_*_button()`.

If both an XML callback and `wire_*_button()` are present on the same button, the handler may fire twice — this is safe because `hide()` guards against double-execution.

### Protected Members

| Member | Type | Purpose |
|--------|------|---------|
| `backdrop_` | `lv_obj_t*` | The full-screen backdrop overlay |
| `dialog_` | `lv_obj_t*` | The dialog card widget |
| `parent_` | `lv_obj_t*` | Parent passed to `show()` |

### Helper: `find_widget(name)`

Convenience wrapper for `lv_obj_find_by_name(dialog_, name)`. Use in `on_show()` for custom widget access.

---

## XML Components

### `ui_dialog` (Custom Widget)

The base container for all modal dialog cards. Registered as a custom LVGL XML widget that provides:

- Theme-aware background color (adapts to light/dark mode via `ThemeManager`)
- Zero padding, zero border, zero shadow by default
- Rounded corner clipping (for full-bleed bottom buttons)
- Disabled state at 50% opacity
- `LV_OBJ_FLAG_USER_1` flag for context-aware input styling

Usage in XML:

```xml
<view name="my_modal"
      extends="ui_dialog" width="70%" height="content" align="center"
      flex_flow="column" style_flex_main_place="start" style_pad_gap="0">
  <!-- content here -->
</view>
```

### `modal_button_row`

Reusable two-button footer with divider. Provides the standard "secondary | primary" button layout.

**API props:**

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `primary_text` | string | "OK" | Primary (right) button label |
| `secondary_text` | string | "Cancel" | Secondary (left) button label |
| `primary_callback` | string | -- | Registered XML callback name |
| `secondary_callback` | string | -- | Registered XML callback name |
| `primary_bg_color` | string | "" | Override primary button color (e.g., `#danger`) |
| `show_secondary` | string | "true" | Show/hide secondary button |

**Note:** `primary_bg_color` and `show_secondary` are declared in the XML API but not yet wired in the component template. They are currently no-ops.

Usage in XML:

```xml
<modal_button_row
    secondary_text="Cancel" secondary_callback="on_my_cancel"
    primary_text="Delete" primary_callback="on_my_confirm"
    primary_bg_color="#danger"/>
```

The component renders as:

```
+--[divider_horizontal]---------+
| [Cancel]    |    [Delete]     |
+-------------------------------+
```

Buttons are edge-to-edge with zero radius, matching the `modal_dialog.xml` style.

### `modal_header`

Reusable icon + title row for modal headers.

**API props:**

| Prop | Type | Default | Description |
|------|------|---------|-------------|
| `icon_src` | string | "" | Icon name (e.g., "alert", "alert_octagon") |
| `icon_variant` | string | "accent" | Icon color variant |
| `title` | string | "" | Header text |
| `title_tag` | string | "" | Translation tag |

Usage in XML:

```xml
<modal_header icon_src="alert_octagon" icon_variant="danger"
              title="Factory Reset" title_tag="Factory Reset"/>
```

### `modal_dialog`

The generic title + message dialog used by `ui_modal_show_confirmation()` and `ui_modal_show_alert()`. Uses subject bindings for dynamic content:

- `dialog_severity` -- controls which icon is shown (0=info, 1=warning, 2=error)
- `dialog_show_cancel` -- toggles cancel button visibility
- `dialog_primary_text` -- primary button label
- `dialog_cancel_text` -- cancel button label

You rarely interact with `modal_dialog` directly. Use `ui_modal_show_confirmation()` or `ui_modal_show_alert()` instead.

---

## Standard Modal XML Template

When creating a new modal with custom layout, follow this template:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- NOTE: Backdrop created programmatically by Modal system -->
<component>
  <view name="my_feature_modal"
        extends="ui_dialog" width="70%" height="content" align="center"
        flex_flow="column" style_flex_main_place="start" style_pad_gap="0">

    <!-- Header (option A: use modal_header component) -->
    <modal_header icon_src="alert" icon_variant="warning"
                  title="My Title" title_tag="My Title"/>

    <!-- Content area -->
    <lv_obj width="100%" height="content"
            style_pad_left="#space_lg" style_pad_right="#space_lg"
            style_pad_top="0" style_pad_bottom="#space_lg">
      <text_body name="dialog_message" width="100%"
                 text="Some message here" long_mode="wrap"/>
    </lv_obj>

    <!-- Button row -->
    <modal_button_row
        secondary_text="Cancel" secondary_callback="on_my_cancel"
        primary_text="Confirm" primary_callback="on_my_confirm"/>
  </view>
</component>
```

Key rules:
- Always `extends="ui_dialog"` (never plain `lv_obj` or `ui_card`)
- Never include a backdrop in XML (comment: "Backdrop created programmatically by Modal system")
- Use `modal_button_row` for standard two-button footers
- Use `modal_header` for icon + title rows (or build custom headers)
- Use design tokens for all spacing (`#space_lg`, `#space_md`, etc.)

---

## ModalGuard (RAII for Static API)

When using `ui_modal_show_confirmation()` or `Modal::show()`, the returned `lv_obj_t*` must eventually be hidden. `ModalGuard` automates this:

```cpp
#include "ui/ui_modal_guard.h"

class ControlsPanel {
    helix::ui::ModalGuard motors_dialog_;
    helix::ui::ModalGuard z_offset_dialog_;

    void confirm_disable_motors() {
        // ModalGuard::operator= hides any previous dialog first
        motors_dialog_ = ui_modal_show_confirmation(
            lv_tr("Disable Motors?"),
            lv_tr("Release all stepper motors."),
            ModalSeverity::Warning, lv_tr("Disable"),
            on_confirm, on_cancel, this);
    }
};
// Panel destructor -> ModalGuard destructor -> ui_modal_hide() called automatically
```

`ModalGuard` supports move semantics, assignment from raw `lv_obj_t*`, explicit `hide()`, and `release()` to take ownership.

---

## ModalStack Internals

`ModalStack` is a singleton that tracks all active modals for:

- **Z-ordering**: Multiple stacked modals maintain correct visual order
- **Top-modal queries**: `Modal::get_top()` returns the topmost dialog
- **Animation state**: `mark_exiting()` prevents double-hide during exit animation
- **Backdrop-to-dialog mapping**: Links each backdrop to its dialog

You should not interact with `ModalStack` directly. Use the `Modal` class API instead.

### Animations

- **Entrance**: 250ms scale (85% to 100%) + fade in, with slight overshoot bounce
- **Exit**: 150ms scale down + fade out, then backdrop and dialog are destroyed
- Constants match `globals.xml` animation tokens (`anim_normal`, `anim_fast`)

---

## Advanced Patterns

### Modals with Dynamic Content (AmsEditModal)

For modals that manage their own subjects and complex state:

```cpp
class AmsEditModal : public Modal {
    SubjectManager subjects_;       // RAII subject lifecycle
    lv_subject_t color_subject_;    // Bound to XML elements
    char color_buf_[32] = {0};      // String buffer for subject

    const char* get_name() const override { return "Edit Filament Modal"; }
    const char* component_name() const override { return "ams_edit_modal"; }

    void on_show() override {
        init_subjects();
        update_ui();
    }

    void on_hide() override {
        deinit_subjects();
    }
};
```

### Modals with Many Buttons (RunoutGuidanceModal)

The hook system supports up to 6 buttons. Wire each to a named hook:

```cpp
void on_show() override {
    wire_ok_button("btn_load_filament");        // -> on_ok()
    wire_cancel_button("btn_resume");           // -> on_cancel()
    wire_tertiary_button("btn_cancel_print");   // -> on_tertiary()
    wire_quaternary_button("btn_unload");       // -> on_quaternary()
    wire_quinary_button("btn_purge");           // -> on_quinary()
    wire_senary_button("btn_ok");               // -> on_senary()
}
```

Some buttons can choose not to hide the modal (useful for "purge" that can be repeated):

```cpp
void on_quinary() override {
    if (on_purge_) on_purge_();
    // Don't call hide() - user may want to purge multiple times
}
```

### Modals with Keyboard Input (WiFi Password)

Use `ui_modal_register_keyboard()` to attach a keyboard to a textarea inside a modal:

```cpp
void on_show() override {
    lv_obj_t* textarea = find_widget("password_input");
    ui_modal_register_keyboard(dialog(), textarea);
}
```

### Modals with Custom Button Styling

In XML, use `primary_bg_color` on `modal_button_row` for destructive actions:

```xml
<modal_button_row
    secondary_text="Keep Printing"
    secondary_callback="on_dismiss"
    primary_text="Stop"
    primary_callback="on_confirm"
    primary_bg_color="#danger"/>
```

---

## Migration Guide (Old Pattern to New Pattern)

The cc046ad2 refactor converted 9 modals. Here is the pattern transformation:

### XML: Inline Backdrop to ui_dialog

**Before** (inline backdrop in XML):
```xml
<component>
  <view name="my_modal_backdrop"
        extends="lv_obj" width="100%" height="100%"
        style_bg_opa="180" style_border_width="0"
        style_radius="0" clickable="true">
    <lv_obj width="400" height="200" align="center"
            style_radius="#border_radius" style_pad_all="#space_2xl"
            flex_flow="column">
      <!-- content -->
      <lv_obj width="100%" height="#button_height" flex_flow="row"
              style_pad_gap="#space_lg">
        <ui_button name="btn_cancel" width="160" text="Cancel">
          <event_cb trigger="clicked" callback="on_cancel"/>
        </ui_button>
        <ui_button name="btn_confirm" width="160" text="Confirm">
          <event_cb trigger="clicked" callback="on_confirm"/>
        </ui_button>
      </lv_obj>
    </lv_obj>
  </view>
</component>
```

**After** (ui_dialog + modal_button_row):
```xml
<component>
  <!-- NOTE: Backdrop created programmatically by Modal system -->
  <view name="my_modal"
        extends="ui_dialog" width="70%" height="content" align="center"
        flex_flow="column" style_flex_main_place="start" style_pad_gap="0">
    <!-- content -->
    <modal_button_row
        secondary_text="Cancel" secondary_callback="on_cancel"
        primary_text="Confirm" primary_callback="on_confirm"/>
  </view>
</component>
```

### C++: Manual Show/Hide to Modal System

**Before** (manual backdrop + hidden flag toggling):
```cpp
// Showing
lv_obj_t* backdrop = lv_obj_find_by_name(screen, "my_modal_backdrop");
lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);

// Hiding
lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
```

**After** (Modal system):
```cpp
// Showing
lv_obj_t* dialog = ui_modal_show("my_modal");

// Hiding
ui_modal_hide(dialog);
```

### C++: Manual Button Wiring to Confirmation Helper

**Before** (18+ lines):
```cpp
const char* attrs[] = {"title", "Delete?", "message", "Cannot be undone.", nullptr};
ui_modal_configure(ModalSeverity::Warning, true, "Delete", "Cancel");
dialog_ = ui_modal_show("modal_dialog", attrs);
if (!dialog_) return;
lv_obj_t* cancel = lv_obj_find_by_name(dialog_, "btn_secondary");
if (cancel) lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, this);
lv_obj_t* confirm = lv_obj_find_by_name(dialog_, "btn_primary");
if (confirm) lv_obj_add_event_cb(confirm, on_confirm, LV_EVENT_CLICKED, this);
```

**After** (single call):
```cpp
dialog_ = ui_modal_show_confirmation(
    "Delete?", "Cannot be undone.",
    ModalSeverity::Warning, "Delete",
    on_confirm, on_cancel, this);
```

### C++: extends="ui_card" to extends="ui_dialog"

If your XML used `extends="ui_card"`, simply change to `extends="ui_dialog"`. The `ui_dialog` widget provides the correct theme-aware background, corner clipping, and context flag.

---

## Checklist: Adding a New Modal

1. **Choose approach**: Helper function, static `Modal::show()`, or subclass?
2. **Create XML** in `ui_xml/` using `extends="ui_dialog"` and `modal_button_row`
3. **Register XML** in `src/xml_registration.cpp` (components must be registered before they're used by other components)
4. **Register callbacks** via `lv_xml_register_event_cb()` in your C++ code
5. **If subclass**: Create header in `include/`, implement `get_name()` and `component_name()`
6. **Wire buttons** in `on_show()` using `wire_ok_button()` / `wire_cancel_button()`
7. **Store the modal** as a member (subclass) or in a `ModalGuard` (static API)
8. **Test**: Modal should auto-hide when parent panel is destroyed

---

## Async Callback Safety

Modal subclasses that make asynchronous API calls (WebSocket, HTTP, Moonraker) need protection against callbacks arriving after the modal is dismissed.

The `Modal` base class provides `lifetime_` (a `helix::AsyncLifetimeGuard`) that handles this automatically:

```cpp
void MyModal::start_operation() {
    auto token = lifetime_.token();
    api->fetch([this, token]() {
        if (token.expired()) return;       // Modal dismissed
        lifetime_.defer([this]() {         // Safe main-thread update
            lv_subject_set_int(&result_subject_, 1);
        });
    });
}
```

`Modal::hide()` calls `lifetime_.invalidate()` before `on_hide()`, so all outstanding tokens expire automatically. Your `on_hide()` override does **not** need to manually invalidate — just handle observer cleanup and state reset.

For cancel-and-retry scenarios (e.g., user clicks "Test Connection" again while a test is in flight), call `lifetime_.invalidate()` explicitly before starting the new operation:

```cpp
void MyModal::handle_retry() {
    lifetime_.invalidate();               // Cancel previous callbacks
    auto token = lifetime_.token();       // Fresh token
    api->test([this, token]() { ... });
}
```

See `include/async_lifetime_guard.h` for the full API documentation.

---

## Legacy API

The following `ui_modal_*()` functions are inline wrappers around the `Modal` class, preserved for backward compatibility:

| Legacy | Current |
|--------|---------|
| `ui_modal_show(name)` | `Modal::show(name)` |
| `ui_modal_hide(dialog)` | `Modal::hide(dialog)` |
| `ui_modal_get_top()` | `Modal::get_top()` |
| `ui_modal_is_visible()` | `Modal::any_visible()` |
| `ui_modal_init_subjects()` | `modal_init_subjects()` |
| `ui_modal_configure(...)` | `modal_configure(...)` |

New code should prefer the `Modal::` class methods or the `ui_modal_show_confirmation()` / `ui_modal_show_alert()` helpers.
