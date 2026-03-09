# Split Button Widget Design

**Date:** 2026-03-08
**Status:** Draft

## Context

HelixScreen needs a reusable split button widget — a button with a primary action on the left and a dropdown arrow on the right that opens a selection list. The first use case is a preheat button where the main click preheats with the remembered filament type, and the dropdown lets you pick a different type. The selection persists and updates the button label dynamically.

No existing widget covers this pattern. `ui_button` handles simple buttons, `lv_dropdown` handles selection lists, but nothing combines a primary action button with a dropdown selector.

## Design

### Visual Structure

```
┌─────────────────────────┬─────────┐
│  🔥 Preheat PLA         │    ▼    │
└─────────────────────────┴─────────┘
 ← main click zone →     ← arrow →
```

Arrow click opens LVGL's native dropdown list below the widget.

### XML API

```xml
<!-- Minimal -->
<ui_split_button text="Preheat" options="PLA&#10;PETG&#10;ABS">
  <event_cb trigger="clicked" callback="on_preheat"/>
  <event_cb trigger="value_changed" callback="on_material_changed"/>
</ui_split_button>

<!-- Full -->
<ui_split_button variant="primary" text="Preheat PLA" icon="heat_wave"
                 options="PLA&#10;PETG&#10;ABS&#10;TPU" selected="0"
                 show_selection="true" text_format="Preheat %s">
  <event_cb trigger="clicked" callback="on_preheat"/>
  <event_cb trigger="value_changed" callback="on_material_changed"/>
</ui_split_button>
```

### Attributes

| Attr | Type | Default | Description |
|------|------|---------|-------------|
| `variant` | string | `"primary"` | Same variants as `ui_button` |
| `text` | string | `""` | Button label (static or format template) |
| `icon` | string | `""` | Optional MDI icon name |
| `options` | string | `""` | Newline-separated dropdown options (use `&#10;`) |
| `selected` | int | `0` | Initially selected option index |
| `show_selection` | bool | `"true"` | Update button text with selected option |
| `text_format` | string | `""` | Format string — `%s` replaced with selection |

### Events

- `clicked` — main button area tapped (primary action)
- `value_changed` — dropdown selection changed

### C++ Helpers

```cpp
void ui_split_button_init();  // Register widget
void ui_split_button_set_options(lv_obj_t* sb, const char* options);
void ui_split_button_set_selected(lv_obj_t* sb, uint32_t index);
uint32_t ui_split_button_get_selected(lv_obj_t* sb);
void ui_split_button_set_text(lv_obj_t* sb, const char* text);
```

### Internal Structure

```
lv_obj (container, row flex, styled with variant)
├── lv_button (main_btn, flex_grow=1, ghost)
│   ├── lv_label (icon, MDI font)  [optional]
│   └── lv_label (text)
├── lv_obj (divider, 1px wide)
├── lv_button (arrow_btn, fixed width ~40px, ghost)
│   └── lv_label (chevron_down icon)
└── lv_dropdown (hidden, zero size, opened programmatically)
```

- Container gets variant style (bg color, radius) — inner buttons are ghost
- Auto-contrast on text/icons matches `ui_button` behavior
- Arrow click → `lv_dropdown_open()` → native LVGL list popup
- Dropdown `value_changed` → update label via `text_format` → forward event to container
- Main button `clicked` → forward to container

### User Data

```cpp
struct SplitButtonData {
    static constexpr uint32_t MAGIC = 0x53504C54; // "SPLT"
    uint32_t magic{MAGIC};
    lv_obj_t* main_btn;
    lv_obj_t* arrow_btn;
    lv_obj_t* dropdown;
    lv_obj_t* icon;
    lv_obj_t* label;
    char* text_format;  // owned, nullable
};
```

### Files

| File | Purpose |
|------|---------|
| `include/ui_split_button.h` | Public API |
| `src/ui/ui_split_button.cpp` | Widget implementation |
| `tests/unit/test_split_button.cpp` | Unit tests |

Registration: `ui_split_button_init()` in `main.cpp` alongside `ui_button_init()`.

### Behavior: Select Only

Following standard split button convention: picking from the dropdown changes the remembered selection and updates the label, but does NOT execute the primary action. The user must tap the main button to execute.

## Testing

1. **Unit tests** — text_format formatting, selected index get/set, options parsing
2. **Visual test** — test panel with split buttons in different variants
3. **Integration** — preheat button use case (separate PR)
