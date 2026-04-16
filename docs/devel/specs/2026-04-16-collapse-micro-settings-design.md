# Collapse Micro XML Settings Variants via Responsive Components

**Issue:** prestonbrown/helixscreen#805
**Date:** 2026-04-16
**Scope:** Settings-related micro/ XML files only (7 of 11)

## Problem

Most files in `ui_xml/micro/` exist solely to apply tighter padding and hide descriptions on 480x272 screens. This duplication causes bugs when XML changes land in only one variant. The spacing tokens are already breakpoint-aware, so the micro/ files shouldn't need to exist.

## Solution Overview

Three changes eliminate the 7 settings-related micro/ files:

1. **Responsive padding via `bind_style`** — Each setting row component uses `bind_style` with `ui_breakpoint` to swap between standard and compact padding tiers
2. **Info icon with inline description toggle** — On micro/tiny breakpoints, descriptions are hidden by default and a small info icon lets users expand them inline; on medium+, descriptions show normally and the icon is hidden
3. **XML engine enhancement** — Add `hidden_if_prop_eq`, `hidden_if_prop_not_eq`, and `hidden_if_empty` attributes for parse-time string prop comparison

## XML Engine Additions (lib/helix-xml)

### New Attributes

All three are **parse-time, one-shot** — they resolve the prop value during XML component creation and set/clear `LV_OBJ_FLAG_HIDDEN` once. They compose with reactive `bind_flag_if_*` observers.

#### `hidden_if_prop_eq`

General string comparison. Format: `$prop_name|ref_value` (pipe-delimited).

```xml
<!-- hidden if $description resolves to "" -->
<lv_obj hidden_if_prop_eq="$description|"/>

<!-- hidden if $mode resolves to "advanced" -->
<lv_obj hidden_if_prop_eq="$mode|advanced"/>
```

If the resolved prop value equals `ref_value`, sets `LV_OBJ_FLAG_HIDDEN`.

#### `hidden_if_prop_not_eq`

Inverse of `hidden_if_prop_eq`.

```xml
<!-- hidden if $description is NOT empty -->
<lv_obj hidden_if_prop_not_eq="$description|"/>
```

#### `hidden_if_empty`

Shortcut for `hidden_if_prop_eq="$prop|"`.

```xml
<lv_obj hidden_if_empty="$description"/>
```

### Implementation Location

These attributes are processed in `lib/helix-xml` during component XML parsing, alongside existing `hidden` attribute handling. The prop reference (`$prop_name`) is resolved against the component's prop scope, and the flag is set before any reactive observers attach.

## Setting Row Component Changes

### Responsive Padding (all 5 row types + section header)

Each component gains a `pad_compact` named style and a `bind_style` that activates it on Micro breakpoint.

**Example: `setting_toggle_row.xml`**

```xml
<styles>
  <style name="label_enabled" text_color="#text"/>
  <style name="label_disabled" text_color="#text_subtle"/>
  <style name="pad_compact" pad_left="#space_md" pad_right="#space_md"
         pad_top="#space_xs" pad_bottom="#space_xs" pad_gap="#space_xs"/>
</styles>
<view name="setting_row"
      extends="lv_obj" width="100%" height="content" flex_flow="row"
      style_pad_left="#space_lg" style_pad_right="#space_lg"
      style_pad_top="#space_md" style_pad_bottom="#space_md" style_pad_gap="#space_sm"
      ...>
  <bind_style name="pad_compact" subject="ui_breakpoint" ref_value="0"/>
  ...
</view>
```

The inline attributes (`style_pad_left="#space_lg"`, etc.) serve as defaults for Medium+ breakpoints. When `ui_breakpoint == 0` (Micro), `pad_compact` overrides with the tighter tier.

**Mapping per component:**

| Component | Standard Padding | Compact Padding |
|-----------|-----------------|-----------------|
| `setting_toggle_row` | lr=`#space_lg`, tb=`#space_md`, gap=`#space_sm` | lr=`#space_md`, tb=`#space_xs`, gap=`#space_xs` |
| `setting_slider_row` | lr=`#space_lg`, tb=`#space_md`, gap=`#space_xs`, slider_tb=`#space_sm`, slider_mt=`#space_xs` | lr=`#space_md`, tb=`#space_xs`, gap=`#space_xs`, slider_tb=`#space_xs`, slider_mt=`0` |
| `setting_dropdown_row` | lr=`#space_lg`, tb=`#space_md`, gap=`#space_sm` | lr=`#space_md`, tb=`#space_xs`, gap=`#space_xs` |
| `setting_action_row` | lr=`#space_lg`, tb=`#space_md`, gap=`#space_sm` | lr=`#space_md`, tb=`#space_xs`, gap=`#space_xs` |
| `setting_section_header` | lr=`#space_lg`, t=`#space_lg`, b=`#space_xs` | lr=`#space_md`, t=`#space_sm`, b=`#space_xxs` |

The slider row needs an additional `pad_compact_slider` style for the slider container's internal padding.

### Info Icon with Inline Description Toggle

Each setting row type that supports `description` gains an info icon element between the label/description area and the widget (toggle/slider/dropdown/chevron).

**XML structure (using toggle row as example):**

```xml
<!-- Middle: Label and optional description -->
<lv_obj height="content"
        style_pad_all="0" flex_flow="column" style_pad_gap="#space_xs"
        flex_grow="1" scrollable="false">
  <text_body name="label" text="$label" translation_tag="$label_tag">
    <bind_style name="label_enabled" subject="$disabled" ref_value="0"/>
    <bind_style name="label_disabled" subject="$disabled" ref_value="1"/>
  </text_body>
  <!-- Description: hidden on micro/tiny by default via bind_flag -->
  <text_small name="description" text="$description"
              translation_tag="$description_tag">
    <bind_flag_if_lt subject="ui_breakpoint" flag="hidden" ref_value="2"/>
  </text_small>
</lv_obj>
<!-- Info icon: hidden if description is empty, AND hidden on medium+ -->
<lv_obj name="info_btn" width="content" height="content"
        style_pad_all="2" style_bg_opa="0" style_border_width="0"
        clickable="true" event_bubble="false" scrollable="false"
        hidden_if_empty="$description">
  <bind_flag_if_gt subject="ui_breakpoint" flag="hidden" ref_value="1"/>
  <icon src="information_outline" size="xs" variant="muted"
        clickable="false" event_bubble="true"/>
  <event_cb trigger="clicked" callback="on_setting_info_clicked"/>
</lv_obj>
```

**Visibility logic:**

| Condition | Info Icon | Description |
|-----------|-----------|-------------|
| description="" (any breakpoint) | Hidden (parse-time) | Empty, zero-height |
| description set + Micro/Tiny | Visible | Hidden (tap icon to toggle) |
| description set + Medium+ | Hidden | Visible |

**Parse-time + reactive composition:** `hidden_if_empty` runs once at creation. If description is non-empty, the element starts visible, then `bind_flag_if_gt` takes over reactively. If description is empty, the element stays hidden regardless of breakpoint.

### C++ Callback

A single global callback registered in `xml_registration.cpp`:

```cpp
static void on_setting_info_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[Settings] on_setting_info_clicked");
    auto* info_btn = lv_event_get_current_target(e);
    // Walk up to the row container, then find description by name
    auto* row = lv_obj_get_parent(info_btn);
    if (!row) return;
    auto* desc = lv_obj_find_by_name(row, "description");
    if (!desc) return;
    if (lv_obj_has_flag(desc, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(desc, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(desc, LV_OBJ_FLAG_HIDDEN);
    }
    LVGL_SAFE_EVENT_CB_END();
}
```

**Parent traversal note:** The `description` element lives inside a child `lv_obj` container, not directly under the row root. Using `lv_obj_find_by_name(row, "description")` with recursive search handles this regardless of nesting depth. The `setting_action_row` has an extra nesting level (root is `lv_button` with a content container inside), so the callback must walk up to the named row root — use `lv_obj_find_by_name` from `info_btn`'s parent chain until hitting the component root.

Registration:
```cpp
lv_xml_register_event_cb(nullptr, "on_setting_info_clicked", on_setting_info_clicked);
```

### `hide_description` Prop Removal

The `setting_dropdown_row` currently has a `hide_description` prop (line 16). This prop is no longer needed — the breakpoint-based `bind_flag_if_lt` handles visibility automatically. The prop and its `hidden="$hide_description"` usage are removed.

Consumers in `settings_display_sound_overlay.xml` that pass `hide_description="true"` have that attribute removed.

## Overlay and Panel Changes

### `settings_display_sound_overlay.xml`

The outer scroll container gains `bind_style` for compact padding on micro:

```xml
<styles>
  <style name="pad_compact" pad_left="#space_md" pad_right="#space_md"
         pad_top="#space_md" pad_bottom="#space_md" pad_gap="#space_sm"/>
</styles>
```

All `hide_description="true"` attributes removed from setting row usages (handled by components now).

### `settings_panel.xml`

Bottom padding difference handled by `bind_style`:

```xml
<styles>
  <style name="pad_compact" pad_bottom="#space_md"/>
</styles>
```

## Files Deleted

1. `ui_xml/micro/setting_toggle_row.xml`
2. `ui_xml/micro/setting_slider_row.xml`
3. `ui_xml/micro/setting_dropdown_row.xml`
4. `ui_xml/micro/setting_action_row.xml`
5. `ui_xml/micro/setting_section_header.xml`
6. `ui_xml/micro/settings_panel.xml`
7. `ui_xml/micro/settings_display_sound_overlay.xml`

## Out of Scope (Future Work)

These 4 structural micro/ files have genuine layout differences and are not addressed:

- `ui_xml/micro/controls_panel.xml` — different layout proportions, button sizes
- `ui_xml/micro/header_bar.xml` — different height, title alignment
- `ui_xml/micro/theme_preview_overlay.xml` — hides Actions card on tiny
- `ui_xml/micro/theme_editor_overlay.xml` — pins buttons outside scroll area

## Testing

- **Visual verification on 480x272:** Settings rows render with compact padding, info icons appear next to rows with descriptions, tapping toggles description inline
- **Visual verification on 800x480+:** Settings rows render with standard padding, descriptions visible by default, no info icons
- **Regression:** All existing setting row usages across panels/overlays render correctly
- **Edge cases:** Rows without descriptions show no info icon on any breakpoint; disabled rows with info icons still allow info toggle

## Documentation

Update `docs/devel/UI_CONTRIBUTOR_GUIDE.md` with:
- Convention: never create micro/ variants for setting rows — use responsive tokens and `bind_style`
- Info icon behavior: automatic when `description` prop is set
- New XML attributes: `hidden_if_prop_eq`, `hidden_if_prop_not_eq`, `hidden_if_empty`
