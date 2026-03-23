# Multi-Page Home Screen

**Issue:** #484 (partial — multi-page home screens only)
**Date:** 2026-03-23

## Overview

Add support for multiple home screen pages with horizontal swipe navigation (Android launcher style). The existing main home page remains the default anchor. Users can add additional pages to either side, each containing its own widget grid. Pages are navigated by swiping or tapping arrow buttons, with dot indicators showing position.

## Data Model

### Configuration Schema

Extend `PanelWidgetConfig` to store a `pages` array inside the existing `panel_widgets.home` key:

```json
{
  "printers": {
    "active-printer": {
      "panel_widgets": {
        "home": {
          "pages": [
            {
              "id": "main",
              "widgets": [
                { "id": "print_status", "enabled": true, "col": 0, "row": 0, "colspan": 2, "rowspan": 2, "config": {} },
                { "id": "temperature", "enabled": true, "col": 2, "row": 0, "colspan": 1, "rowspan": 1, "config": {} }
              ]
            },
            {
              "id": "page_1",
              "widgets": [
                { "id": "macros", "enabled": true, "col": 0, "row": 0, "colspan": 2, "rowspan": 1, "config": {} }
              ]
            }
          ],
          "main_page_index": 0
        }
      }
    }
  }
}
```

### Key Properties

- **`pages`**: Ordered array of page objects. Each page has an `id` (auto-generated: `"main"`, `"page_1"`, `"page_2"`, etc.) and a `widgets` array with the same `PanelWidgetEntry` structure used today.
- **`main_page_index`**: Index of the anchor page (cannot be deleted). Defaults to 0.
- **Page IDs**: Internal only, not user-facing. Used for grid cache keys and config identity.

### Migration

On load, if `panel_widgets.home` is a flat array (legacy format), migrate it in-place to:
```json
{
  "pages": [{ "id": "main", "widgets": <existing array> }],
  "main_page_index": 0
}
```
This is transparent — existing configs work without user intervention.

### Defaults

A fresh install gets a single page (`"main"`) with the default widget set. Maximum soft cap of 8 pages — the "+" tile is hidden once this limit is reached.

## Interface Contracts

### PanelWidgetConfig Refactoring

`PanelWidgetConfig` becomes page-aware. Currently `load()` expects `panel_widgets.home` to be a flat JSON array and stores entries in `entries_`. The refactoring:

1. **`load()` detects format**: If `panel_widgets.home` is an array (legacy), migrate to the `pages` object format and write back. If it's an object with a `pages` key, parse the pages array.
2. **New internal storage**: Replace `std::vector<PanelWidgetEntry> entries_` with `std::vector<PageConfig> pages_` where:
   ```cpp
   struct PageConfig {
       std::string id;
       std::vector<PanelWidgetEntry> widgets;
   };
   ```
   Plus `int main_page_index_`.
3. **New accessors**:
   ```cpp
   int page_count() const;
   int main_page_index() const;
   const std::vector<PanelWidgetEntry>& page_entries(int page_index) const;
   std::vector<PanelWidgetEntry>& page_entries_mut(int page_index);
   void add_page(const std::string& id);       // appends empty page
   void remove_page(int page_index);            // removes, adjusts main_page_index
   ```
4. **Backward compatibility**: The existing `entries()` and `mutable_entries()` accessors return `page_entries(0)` / `page_entries_mut(0)` so callers that don't know about pages (e.g., other panels, `GridEditMode` via `config_->mutable_entries()`) continue working unchanged.
5. **`save()`** writes the new object format with `pages` array and `main_page_index`.
6. **`delete_entry()` and `mint_instance_id()`** scan across ALL pages, not just one. Instance IDs must be unique across all pages to prevent duplicate widget IDs. `delete_entry(id)` removes the first match across all pages.
7. **`remove_page()` guard**: `remove_page(main_page_index_)` is a no-op (defensive check). Callers should not attempt it, but the API is safe if they do.
8. **Migration write-back**: `load()` calls `save()` immediately after migrating from flat array to `pages` format, so the on-disk format is updated and re-migration doesn't occur on every load.

### Page ID Generation

Page IDs use a monotonic counter stored in config (`"next_page_id": 3`). When a page is added, it gets `page_{counter}` and the counter increments. This prevents ID reuse after deletion, which matters since IDs are used as grid cache keys.

### PanelWidgetManager Interface Changes

`populate_widgets()` gains a page index parameter:

```cpp
std::vector<std::unique_ptr<PanelWidget>> populate_widgets(
    const std::string& panel_id, lv_obj_t* container,
    int page_index = 0, WidgetReuseMap reuse = {});
```

Internally, it calls `config.page_entries(page_index)` instead of `config.entries()`. Default `page_index=0` preserves backward compatibility for non-home panels.

**Caching changes:**
- **`grid_descriptors_`**: Keyed by `panel_id:page_index` (e.g., `"home:0"`, `"home:1"`) instead of just `panel_id`. All pages on the same panel share identical grid dimensions (determined by breakpoint), but each page needs its own descriptor array since LVGL grid objects hold a pointer to the array. The descriptors must remain alive for the lifetime of the LVGL objects.
- **`active_configs_`** (rebuild-skip cache): Also keyed by `panel_id:page_index`. This ensures the short-circuit check compares the correct page's widget IDs, not a different page's.
- **`compute_visible_widget_ids()`**: Gains a `page_index` parameter and reads `config.page_entries(page_index)`. HomePanel calls it per-page when checking whether a rebuild is needed.

### HomePanel Data Structures

`HomePanel` stores per-page widget vectors:

```cpp
std::vector<std::vector<std::unique_ptr<PanelWidget>>> page_widgets_;  // [page_index][widget]
std::vector<lv_obj_t*> page_containers_;  // widget_container per page
int active_page_index_ = 0;
```

`on_activate()` / `on_deactivate()` on page widgets is driven by carousel `page_subject` observer. Only `page_widgets_[active_page_index_]` has active widgets at any time.

### GridEditMode Scoping

`GridEditMode::enter()` signature is unchanged — it takes a `container` and `PanelWidgetConfig*`. HomePanel passes:
- `container`: `page_containers_[active_page_index_]`
- `config`: the shared `PanelWidgetConfig` instance

But `GridEditMode` needs to know which page's entries to read/write. Add a `page_index` parameter:

```cpp
void GridEditMode::enter(lv_obj_t* container, PanelWidgetConfig* config, int page_index = 0);
```

Internally uses `config->page_entries_mut(page_index)` instead of `config->entries()`. Default preserves backward compatibility.

### Gate Observers

The single `"home"` gate observer triggers a full rebuild callback. `HomePanel::populate_widgets(force)` iterates all pages, calling `PanelWidgetManager::populate_widgets()` for each. No per-page gate observer registration needed.

## Home Panel Layout

### Structure

```
home_panel (100% x 100%)
├── home_carousel (ui_carousel, 100% x 100%, show_indicators=true)
│   ├── Page 0: widget_container (grid layout)
│   ├── Page 1: widget_container (grid layout)
│   ├── ...
│   └── "+" tile (add-page button, not a real page)
├── Arrow buttons (left/right chevrons, absolutely positioned at content edges)
└── E-Stop FAB (unchanged, floats above everything)
```

### Carousel Creation

The carousel is created programmatically in `HomePanel::setup()`, not in XML, because page count is dynamic. The XML `home_panel.xml` provides a placeholder container. Tiles are added via `ui_carousel_add_item()`.

Each page gets its own `widget_container` — a fresh `lv_obj` with grid layout. `PanelWidgetManager::populate_widgets()` is called per page, receiving that page's widget array from config.

### The "+" Tile

A special tile appended as the last carousel item. Contains a centered "+" icon button. Not a real page in config — always present as the final tile. Tapping it:

1. Creates a new empty page entry in config
2. Saves config
3. Rebuilds the carousel
4. Animates to the new page

### Arrow Buttons

Two chevron buttons (`<` / `>`) absolutely positioned at the left and right edges of the content area, vertically centered. Wired to `ui_carousel_goto_page(current ± 1)`.

- Left arrow hidden on first page
- Right arrow hidden on last real page
- The "+" tile is not reachable via arrows — only by swiping past the last real page
- Visibility updated reactively via the carousel's `current_page_subject`

### Dot Indicators

Provided by the carousel widget. The "+" tile is excluded from indicator count (see Carousel Extensions). Users see dots only for their real pages.

### Startup

Carousel opens to `main_page_index`. Only that page's widgets receive `on_activate()`.

## Carousel Extensions

Four additions to the existing `ui_carousel` widget:

### 1. Real Page Count

```cpp
void ui_carousel_set_real_page_count(lv_obj_t* carousel, int count);
```

Sets the number of "real" pages. This governs both indicator dot count AND navigation bounds for `goto_page()` / arrow buttons. Tiles beyond `count` (i.e., the "+" tile) are still swipeable but excluded from indicators and `goto_page()` clamping. When not set, defaults to `real_tiles.size()` (existing behavior preserved).

HomePanel calls this with `config.page_count()` after building tiles, so the "+" tile is excluded from dots and arrow navigation.

### 2. Remove Item

```cpp
void ui_carousel_remove_item(lv_obj_t* carousel, int index);
```

Removes a tile at the given index:
- Deletes the tile's LVGL object and all its children (`lv_obj_delete()`)
- Erases from `real_tiles` vector
- If `current_page >= real_tiles.size()`, decrements to last valid page
- Updates `page_subject` if current page changed
- Rebuilds indicators (respecting real page count override)
- **Must not be called during scroll animation** — caller (HomePanel) ensures this by only removing pages from edit mode (carousel is scroll-locked)

### 3. Scroll Enable/Disable

```cpp
void ui_carousel_set_scroll_enabled(lv_obj_t* carousel, bool enabled);
```

Enables or disables horizontal scrolling on the carousel's internal scroll container. Used by HomePanel to lock scrolling during grid edit mode. Encapsulates access to `CarouselState::scroll_container` so HomePanel doesn't need to reach into carousel internals.

### 4. No Other Changes

Snap scrolling, page tracking via subject, and touch handling all work as-is. Note: `page_subject` updates happen on the UI thread (carousel events fire from LVGL's event loop), so HomePanel's observer for page transitions is safe without `ui_queue_update()`.

## Grid Edit Mode

### Per-Page Scoping

Edit mode operates on the currently visible page. `HomePanel` passes the active page's `widget_container` and config slice to `GridEditMode::enter()`. From GridEditMode's perspective, nothing changes — it still works on one container with one widget list.

### Carousel Scroll Lock

On edit mode entry, `HomePanel` disables carousel scrolling:
```cpp
ui_carousel_set_scroll_enabled(carousel_, false);
```
Re-enabled on exit via `ui_carousel_set_scroll_enabled(carousel_, true)`. This prevents horizontal swipe from being captured by the carousel when the user is dragging widgets.

### Page Deletion

A "Delete Page" button is added to the edit mode toolbar (near the existing "+" catalog button). It is hidden/disabled on the main page (`main_page_index`). Tapping it:

1. Confirms via `modal_show_confirmation()`
2. Removes the page from config
3. Adjusts `main_page_index` if needed (if a page before the main page was deleted)
4. Saves config
5. Exits edit mode
6. Rebuilds carousel (navigates to adjacent page)

### Widget Catalog

Unchanged — places widgets on the currently active page's container.

### No Cross-Page Dragging

Widgets stay on their page. Moving to another page is out of scope.

## Page Lifecycle & Performance

### Active vs. Inactive Pages

- **Only the visible page's widgets are "active."** When the carousel page changes, `HomePanel` calls `on_deactivate()` on the old page's widgets and `on_activate()` on the new page's widgets. This stops timers and pauses observers on off-screen pages.
- **All pages' LVGL objects exist simultaneously.** The carousel needs all tiles to exist for scroll animation. Widget objects are created for all pages at startup, but only the active page's widgets are ticking.

### Gate Observers

Hardware gate changes (fan discovered, power device appears) trigger a full rebuild of all pages via `populate_widgets(force=true)`. Cost scales linearly with page count, but 2-4 pages is the practical maximum, so this is acceptable.

### Widget Count

No hard page limit. Each page has the same breakpoint-dependent grid dimensions (e.g., 4x4 on medium). Empty pages have negligible cost.

## Files Affected

| File | Changes |
|------|---------|
| `src/system/panel_widget_config.cpp` / `.h` | Multi-page data model, `pages` array, migration from flat format |
| `src/ui/ui_panel_home.cpp` / `.h` | Carousel creation, arrow buttons, per-page lifecycle, "+" tile handling |
| `src/ui/ui_carousel.cpp` / `.h` | `set_real_page_count()`, `remove_item()`, `set_scroll_enabled()` APIs |
| `src/ui/grid_edit_mode.cpp` / `.h` | Carousel scroll lock on enter/exit, "Delete Page" button |
| `src/ui/panel_widget_manager.cpp` / `.h` | Per-page `populate_widgets()` (receives page index / widget slice) |
| `ui_xml/home_panel.xml` | Structural changes — carousel placeholder, arrow button containers |

## Out of Scope

These are separate issues to be filed from #484:

- Cross-page widget dragging
- Page reordering (drag-to-reorder)
- Resizable/expandable widgets
- Optional text labels under icons
- Customizable grid dimensions (rows/columns)
- New widget types
- Auto-advance / timed page rotation
