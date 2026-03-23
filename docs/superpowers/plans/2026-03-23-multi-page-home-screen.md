# Multi-Page Home Screen Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add swipeable multi-page home screens with dot indicators, arrow navigation, page add/delete, and per-page widget grids.

**Architecture:** Wrap the home panel's widget container in a `ui_carousel` instance, one tile per config page plus a "+" tile. `PanelWidgetConfig` becomes page-aware with a `pages` array. `PanelWidgetManager` gains a `page_index` parameter. Edit mode scopes to the active page with carousel scroll locked.

**Tech Stack:** C++17, LVGL 9.5, nlohmann::json, Catch2 (tests)

**Spec:** `docs/superpowers/specs/2026-03-23-multi-page-home-screen-design.md`

**Deliberate simplification:** The spec says "pages to either side" of the main page, but this plan implements append-only page addition (new pages always added after the last page). Inserting pages before the main page can be added later if needed — the data model supports it (pages are an ordered array with `main_page_index`).

---

## Chunk 1: PanelWidgetConfig Multi-Page Data Model

### Task 1: Add PageConfig struct and multi-page storage

**Files:**
- Modify: `include/panel_widget_config.h`
- Modify: `src/system/panel_widget_config.cpp`
- Test: `tests/test_panel_widget_config.cpp`

**Docs:** Read spec section "PanelWidgetConfig Refactoring" and "Page ID Generation"

- [ ] **Step 1: Write failing tests for multi-page config**

Create or extend `tests/test_panel_widget_config.cpp` with tests:

```cpp
TEST_CASE("PanelWidgetConfig multi-page: default has one page", "[panel_widget_config]") {
    // Setup a Config with empty panel_widgets.home
    // Load should produce 1 page with id "main"
    auto config = create_test_config("home");
    config.load();
    REQUIRE(config.page_count() == 1);
    REQUIRE(config.main_page_index() == 0);
    REQUIRE(config.page_entries(0).size() > 0);  // default widgets
}

TEST_CASE("PanelWidgetConfig multi-page: legacy flat array migrates", "[panel_widget_config]") {
    // Write a flat array to panel_widgets.home (old format)
    // Load should migrate to pages format
    auto config = create_test_config_with_legacy_array("home");
    config.load();
    REQUIRE(config.page_count() == 1);
    REQUIRE(config.page_entries(0).size() == 2);  // migrated entries
}

TEST_CASE("PanelWidgetConfig multi-page: add and remove pages", "[panel_widget_config]") {
    auto config = create_test_config("home");
    config.load();
    REQUIRE(config.page_count() == 1);

    config.add_page("page_1");
    REQUIRE(config.page_count() == 2);
    REQUIRE(config.page_entries(1).empty());

    config.remove_page(1);
    REQUIRE(config.page_count() == 1);
}

TEST_CASE("PanelWidgetConfig multi-page: cannot remove main page", "[panel_widget_config]") {
    auto config = create_test_config("home");
    config.load();
    config.add_page("page_1");
    REQUIRE(config.page_count() == 2);

    config.remove_page(0);  // main page — should be no-op
    REQUIRE(config.page_count() == 2);
}

TEST_CASE("PanelWidgetConfig multi-page: entries() backward compat returns page 0", "[panel_widget_config]") {
    auto config = create_test_config("home");
    config.load();
    REQUIRE(&config.entries() == &config.page_entries(0));
    REQUIRE(&config.mutable_entries() == &config.page_entries_mut(0));
}

TEST_CASE("PanelWidgetConfig multi-page: delete_entry scans all pages", "[panel_widget_config]") {
    auto config = create_test_config("home");
    config.load();
    config.add_page("page_1");
    // Add a widget to page 1
    config.page_entries_mut(1).push_back({"test_widget", true, {}, 0, 0, 1, 1});
    config.delete_entry("test_widget");
    // Should be removed from page 1
    REQUIRE(config.page_entries(1).empty());
}

TEST_CASE("PanelWidgetConfig multi-page: mint_instance_id unique across pages", "[panel_widget_config]") {
    auto config = create_test_config("home");
    config.load();
    config.add_page("page_1");
    // Add "macros:0" to page 1 (cross-page scan test)
    config.page_entries_mut(1).push_back({"macros:0", true, {}, 0, 0, 1, 1});
    // mint should find macros:0 on page 1 and produce macros:1
    auto id = config.mint_instance_id("macros");
    REQUIRE(id == "macros:1");
}

TEST_CASE("PanelWidgetConfig multi-page: remove_page adjusts main_page_index", "[panel_widget_config]") {
    auto config = create_test_config("home");
    config.load();
    config.add_page("page_1");
    config.add_page("page_2");
    // main_page_index is 0. Remove page_1 (index 1) — main stays 0
    config.remove_page(1);
    REQUIRE(config.main_page_index() == 0);
    REQUIRE(config.page_count() == 2);
}

TEST_CASE("PanelWidgetConfig multi-page: save and reload round-trip", "[panel_widget_config]") {
    auto config = create_test_config("home");
    config.load();
    config.add_page("page_1");
    config.page_entries_mut(1).push_back({"macros", true, {}, 0, 0, 1, 1});
    config.save();

    auto config2 = create_test_config("home");
    config2.load();
    REQUIRE(config2.page_count() == 2);
    REQUIRE(config2.page_entries(1).size() == 1);
    REQUIRE(config2.page_entries(1)[0].id == "macros");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[panel_widget_config]" -v`
Expected: Compilation errors — `page_count()`, `page_entries()`, etc. don't exist yet.

- [ ] **Step 3: Add PageConfig struct and new members to header**

In `include/panel_widget_config.h`, add after `PanelWidgetEntry`:

```cpp
struct PageConfig {
    std::string id;
    std::vector<PanelWidgetEntry> widgets;
};
```

Replace `entries_` with `pages_` in the `PanelWidgetConfig` class. Add new members and accessors:

```cpp
// New storage (replaces entries_)
std::vector<PageConfig> pages_;
int main_page_index_ = 0;
int next_page_id_ = 1;  // monotonic counter for page ID generation

// New multi-page accessors
int page_count() const;
int main_page_index() const;
const std::vector<PanelWidgetEntry>& page_entries(int page_index) const;
std::vector<PanelWidgetEntry>& page_entries_mut(int page_index);
void add_page(const std::string& id);
void remove_page(int page_index);
std::string generate_page_id();  // returns "page_{next_page_id_++}"

// Backward compatibility: entries() / mutable_entries() delegate to page 0
```

Keep `entries()` and `mutable_entries()` but change them to return `pages_[0].widgets`.

- [ ] **Step 4: Implement load() with format detection and migration**

In `src/system/panel_widget_config.cpp`, modify `load()`:

1. Read `panel_widgets.{panel_id}` from config
2. If it's a JSON array (legacy): wrap in `PageConfig{id="main", widgets=<migrated entries>}`, set `main_page_index_=0`, `next_page_id_=1`, call `save()` to write back
3. If it's a JSON object with `"pages"` key: parse each page's `id` and `widgets` array, read `main_page_index` and `next_page_id` (default `next_page_id` to `pages_.size()` if missing — handles first upgrade from legacy where the key doesn't exist yet)
4. If null/missing: call `build_defaults()` for page 0

The existing entry-level parsing (skipping unknowns, deduplicating, favorite_macro migration) runs per-page.

- [ ] **Step 5: Implement save() with pages format**

Modify `save()` to write:
```json
{
  "pages": [{"id": "main", "widgets": [...]}, ...],
  "main_page_index": 0,
  "next_page_id": 2
}
```

- [ ] **Step 6: Implement add_page(), remove_page(), generate_page_id()**

```cpp
void PanelWidgetConfig::add_page(const std::string& id) {
    pages_.push_back({id, {}});
}

void PanelWidgetConfig::remove_page(int page_index) {
    if (page_index == main_page_index_) return;  // guard
    if (page_index < 0 || page_index >= static_cast<int>(pages_.size())) return;
    pages_.erase(pages_.begin() + page_index);
    if (page_index < main_page_index_) {
        main_page_index_--;
    }
}

std::string PanelWidgetConfig::generate_page_id() {
    return "page_" + std::to_string(next_page_id_++);
}
```

- [ ] **Step 7: Update all entry-scanning methods to be page-aware**

Methods that currently scan `entries_` need updating:

- **`delete_entry()`**: iterate all pages' `widgets` vectors. Remove first match found.
- **`mint_instance_id()`**: collect all widget IDs from ALL pages, then find the next unused index.
- **`is_enabled(id)`**: search all pages for the widget, not just page 0.
- **`get_widget_config(id)` / `set_widget_config(id, config)`**: search all pages for the widget by ID.
- **`reorder()` / `set_enabled()`**: these take an index into `entries_` — they continue to operate on page 0 via backward-compat `entries()` delegation. Callers that need page-specific access should use `page_entries_mut()` directly.
- **`reset_to_defaults()`**: resets page 0 to defaults, removes all other pages.

- [ ] **Step 8: Run tests and verify they pass**

Run: `make test && ./build/bin/helix-tests "[panel_widget_config]" -v`
Expected: All new tests PASS. Existing tests also PASS (backward compat via `entries()` delegation).

- [ ] **Step 9: Commit**

```bash
git add include/panel_widget_config.h src/system/panel_widget_config.cpp tests/test_panel_widget_config.cpp
git commit -m "feat(config): multi-page PanelWidgetConfig data model with migration (prestonbrown/helixscreen#484)"
```

---

## Chunk 2: Carousel Widget Extensions

### Task 2: Add set_real_page_count, remove_item, set_scroll_enabled to carousel

**Files:**
- Modify: `include/ui_carousel.h`
- Modify: `src/ui/ui_carousel.cpp`
- Test: `tests/test_carousel.cpp` (create if needed)

**Docs:** Read spec section "Carousel Extensions"

- [ ] **Step 1: Write failing tests for carousel extensions**

```cpp
TEST_CASE("Carousel: set_real_page_count limits indicators", "[carousel]") {
    // Create carousel, add 3 items + 1 "+" item
    // set_real_page_count(3)
    // Verify indicator count is 3, not 4
    auto* carousel = create_test_carousel();
    for (int i = 0; i < 4; i++) {
        auto* tile = lv_obj_create(nullptr);
        ui_carousel_add_item(carousel, tile);
    }
    ui_carousel_set_real_page_count(carousel, 3);
    auto* state = ui_carousel_get_state(carousel);
    REQUIRE(lv_obj_get_child_count(state->indicator_row) == 3);
}

TEST_CASE("Carousel: remove_item removes tile and adjusts state", "[carousel]") {
    auto* carousel = create_test_carousel();
    for (int i = 0; i < 3; i++) {
        auto* tile = lv_obj_create(nullptr);
        ui_carousel_add_item(carousel, tile);
    }
    REQUIRE(ui_carousel_get_page_count(carousel) == 3);
    ui_carousel_remove_item(carousel, 1);
    REQUIRE(ui_carousel_get_page_count(carousel) == 2);
}

TEST_CASE("Carousel: remove_item adjusts current_page if needed", "[carousel]") {
    auto* carousel = create_test_carousel();
    for (int i = 0; i < 3; i++) {
        auto* tile = lv_obj_create(nullptr);
        ui_carousel_add_item(carousel, tile);
    }
    ui_carousel_goto_page(carousel, 2, false);
    ui_carousel_remove_item(carousel, 2);
    REQUIRE(ui_carousel_get_current_page(carousel) == 1);
}

TEST_CASE("Carousel: set_scroll_enabled toggles scrollability", "[carousel]") {
    auto* carousel = create_test_carousel();
    auto* tile = lv_obj_create(nullptr);
    ui_carousel_add_item(carousel, tile);
    auto* state = ui_carousel_get_state(carousel);

    ui_carousel_set_scroll_enabled(carousel, false);
    REQUIRE(!lv_obj_has_flag(state->scroll_container, LV_OBJ_FLAG_SCROLLABLE));

    ui_carousel_set_scroll_enabled(carousel, true);
    REQUIRE(lv_obj_has_flag(state->scroll_container, LV_OBJ_FLAG_SCROLLABLE));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[carousel]" -v`
Expected: Compilation errors — new functions don't exist.

- [ ] **Step 3: Add declarations and programmatic creation API to ui_carousel.h**

**IMPORTANT**: `ui_carousel_create()` is an XML widget factory (`void*(lv_xml_parser_state_t*, const char**)`) — it cannot be called from C++ with a parent `lv_obj_t*`. Add a new programmatic constructor:

```cpp
// Programmatic carousel creation (not from XML)
lv_obj_t* ui_carousel_create_obj(lv_obj_t* parent);
```

This extracts the core carousel creation logic from `ui_carousel_create()` into a reusable function. Both the XML factory and the new C++ API call the same internal setup. The XML factory continues to parse attributes via `ui_carousel_apply()`.

Add to `CarouselState`:
```cpp
int real_page_count = -1;  // -1 means "use real_tiles.size()"
```

Add function declarations:
```cpp
lv_obj_t* ui_carousel_create_obj(lv_obj_t* parent);
void ui_carousel_set_real_page_count(lv_obj_t* carousel, int count);
void ui_carousel_remove_item(lv_obj_t* carousel, int index);
void ui_carousel_set_scroll_enabled(lv_obj_t* carousel, bool enabled);
```

- [ ] **Step 4: Implement ui_carousel_create_obj**

Extract the carousel object creation from `ui_carousel_create()` (which is the XML factory) into a shared internal helper, then expose it as `ui_carousel_create_obj()`:

```cpp
lv_obj_t* ui_carousel_create_obj(lv_obj_t* parent) {
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    // ... same setup as ui_carousel_create: flex column, scroll container,
    //     indicator row, CarouselState allocation, event callbacks ...
    auto* state = new CarouselState();
    state->magic = CAROUSEL_MAGIC;
    lv_obj_set_user_data(container, state);
    // ... create scroll_container, indicator_row, wire events ...
    return container;
}
```

Update the existing XML factory `ui_carousel_create()` to call `ui_carousel_create_obj()` internally, then apply XML attributes via `ui_carousel_apply()`.

- [ ] **Step 5: Implement set_real_page_count**

In `src/ui/ui_carousel.cpp`:

```cpp
void ui_carousel_set_real_page_count(lv_obj_t* carousel, int count) {
    auto* state = ui_carousel_get_state(carousel);
    if (!state) return;
    state->real_page_count = count;
    ui_carousel_rebuild_indicators(carousel);
}
```

Modify `ui_carousel_rebuild_indicators()` to use `state->real_page_count` when >= 0, instead of `real_tiles.size()`, for the number of dots created.

Modify `ui_carousel_goto_page()` to clamp to `real_page_count` when set (so arrows can't navigate to the "+" tile).

- [ ] **Step 6: Implement remove_item**

```cpp
void ui_carousel_remove_item(lv_obj_t* carousel, int index) {
    auto* state = ui_carousel_get_state(carousel);
    if (!state || index < 0 || index >= static_cast<int>(state->real_tiles.size())) return;

    lv_obj_delete(state->real_tiles[index]);
    state->real_tiles.erase(state->real_tiles.begin() + index);

    if (state->current_page >= static_cast<int>(state->real_tiles.size())) {
        state->current_page = std::max(0, static_cast<int>(state->real_tiles.size()) - 1);
    }

    if (state->page_subject) {
        lv_subject_set_int(state->page_subject, state->current_page);
    }

    ui_carousel_rebuild_indicators(carousel);
}
```

- [ ] **Step 7: Implement set_scroll_enabled**

```cpp
void ui_carousel_set_scroll_enabled(lv_obj_t* carousel, bool enabled) {
    auto* state = ui_carousel_get_state(carousel);
    if (!state || !state->scroll_container) return;
    if (enabled) {
        lv_obj_add_flag(state->scroll_container, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_remove_flag(state->scroll_container, LV_OBJ_FLAG_SCROLLABLE);
    }
}
```

- [ ] **Step 8: Run tests and verify they pass**

Run: `make test && ./build/bin/helix-tests "[carousel]" -v`
Expected: All PASS.

- [ ] **Step 9: Commit**

```bash
git add include/ui_carousel.h src/ui/ui_carousel.cpp tests/test_carousel.cpp
git commit -m "feat(carousel): add create_obj, set_real_page_count, remove_item, set_scroll_enabled APIs (prestonbrown/helixscreen#484)"
```

---

## Chunk 3: PanelWidgetManager Per-Page Support

### Task 3: Add page_index parameter to populate_widgets and caching

**Files:**
- Modify: `include/panel_widget_manager.h`
- Modify: `src/ui/panel_widget_manager.cpp`

**Docs:** Read spec section "PanelWidgetManager Interface Changes"

- [ ] **Step 1: Update populate_widgets signature**

In `include/panel_widget_manager.h`, change:
```cpp
std::vector<std::unique_ptr<PanelWidget>> populate_widgets(
    const std::string& panel_id, lv_obj_t* container,
    int page_index = 0, WidgetReuseMap reuse = {});
```

Update `compute_visible_widget_ids()`:
```cpp
std::vector<std::string> compute_visible_widget_ids(
    const std::string& panel_id, int page_index = 0);
```

- [ ] **Step 2: Update caching keys to include page_index**

In `src/ui/panel_widget_manager.cpp`, create a helper:
```cpp
static std::string make_cache_key(const std::string& panel_id, int page_index) {
    return panel_id + ":" + std::to_string(page_index);
}
```

Replace all uses of `panel_id` as a key into `grid_descriptors_` and `active_configs_` with `make_cache_key(panel_id, page_index)`.

- [ ] **Step 3: Fix existing call site for new signature**

The existing call in `ui_panel_home.cpp` line ~208 passes `reuse` as the 3rd arg:
```cpp
mgr.populate_widgets("home", container, std::move(reuse));
```
This must be updated to include the page_index (0 for now, Task 4 will change it):
```cpp
mgr.populate_widgets("home", container, 0, std::move(reuse));
```
Without this fix, the code won't compile between Chunks 3 and 4.

- [ ] **Step 4: Update populate_widgets to read page-scoped entries**

In `populate_widgets()`, change the config read from `config.entries()` to `config.page_entries(page_index)`. All call sites in `panel_widget_manager.cpp` that use `widget_config.entries()` or `widget_config.mutable_entries()`:
- Line ~113: widget collection loop (entries iteration)
- Line ~181: rebuild-skip ID comparison
- Line ~221: `const auto& entries = widget_config.entries()` for grid placement
- Line ~300: `widget_config.mutable_entries()` for disabling widgets that don't fit
- Line ~352: `widget_config.mutable_entries()` for disabling overflow widgets
- Line ~374: `widget_config.mutable_entries()` for writing back computed positions
- Line ~462: `widget_config.entries()` for card background calculation

All change to `widget_config.page_entries(page_index)` or `widget_config.page_entries_mut(page_index)`.

Also update `clear_panel_config()` to erase all keys matching the panel_id prefix from `active_configs_` (e.g., `"home:0"`, `"home:1"`, etc.) instead of just `active_configs_.erase(panel_id)`.

- [ ] **Step 5: Update compute_visible_widget_ids to accept page_index**

Same change: read `config.page_entries(page_index)` instead of `config.entries()`.

- [ ] **Step 6: Build and verify existing behavior unchanged**

Run: `make -j && make test-run`
Expected: All tests pass. Default `page_index=0` means all existing callers are unaffected.

- [ ] **Step 7: Commit**

```bash
git add include/panel_widget_manager.h src/ui/panel_widget_manager.cpp
git commit -m "feat(widgets): add page_index to populate_widgets and caching (prestonbrown/helixscreen#484)"
```

---

## Chunk 4: HomePanel Carousel Integration

### Task 4: Replace single widget_container with carousel of page containers

**Files:**
- Modify: `include/ui_panel_home.h`
- Modify: `src/ui/ui_panel_home.cpp`
- Modify: `ui_xml/home_panel.xml`

**Docs:** Read spec sections "Home Panel Layout", "HomePanel Data Structures", "Page Lifecycle & Performance"

**Key lesson:** Applying [L031] — XML files are loaded at runtime, no rebuild needed for XML-only changes.

- [ ] **Step 1: Update home_panel.xml**

Replace the `widget_container` with a carousel placeholder. The actual carousel is created in C++, but we need a parent container in XML:

```xml
<!-- Replace the widget_container with a carousel_host -->
<lv_obj name="carousel_host"
    style_width="100%" style_height="100%"
    style_bg_opa="0" style_border_width="0" style_pad_all="0">
    <!-- Carousel and page containers created programmatically -->
    <!-- Event callbacks stay on carousel_host for edit mode -->
    <event_cb trigger="long_pressed" callback="on_home_grid_long_press"/>
    <event_cb trigger="clicked" callback="on_home_grid_clicked"/>
    <event_cb trigger="pressing" callback="on_home_grid_pressing"/>
    <event_cb trigger="released" callback="on_home_grid_released"/>
</lv_obj>
```

Keep the E-Stop FAB unchanged.

- [ ] **Step 2: Add new members to HomePanel header**

In `include/ui_panel_home.h`, add:

```cpp
// Multi-page state
lv_obj_t* carousel_ = nullptr;
lv_obj_t* carousel_host_ = nullptr;
lv_obj_t* add_page_tile_ = nullptr;
lv_obj_t* arrow_left_ = nullptr;
lv_obj_t* arrow_right_ = nullptr;
std::vector<std::vector<std::unique_ptr<PanelWidget>>> page_widgets_;
std::vector<lv_obj_t*> page_containers_;
std::vector<std::vector<std::string>> page_visible_ids_;  // per-page cached IDs
int active_page_index_ = 0;
lv_subject_t page_subject_;
ObserverGuard page_observer_;

static constexpr int kMaxPages = 8;

// New methods
void build_carousel();
void rebuild_carousel();
void on_page_changed(int new_page);
void on_add_page_clicked();
void update_arrow_visibility(int page);
void populate_page(int page_index, bool force);
```

Remove `active_widgets_` and `last_visible_widget_ids_` (replaced by per-page vectors).

- [ ] **Step 3: Implement build_carousel()**

Creates the carousel inside `carousel_host_`, adds one tile per config page, plus the "+" tile.

**IMPORTANT**: Use `ui_carousel_create_obj()` (from Chunk 2), NOT `ui_carousel_create()` which is the XML factory.

**IMPORTANT**: The carousel's `page_subject` is only allocated when the XML `current_page_subject` attribute is set. For programmatic creation, allocate `page_subject_` as a member of `HomePanel`, init with `lv_subject_init_int(&page_subject_, 0)`, and set `ui_carousel_get_state(carousel_)->page_subject = &page_subject_` after creation.

```cpp
void HomePanel::build_carousel() {
    auto& config = PanelWidgetManager::instance().get_widget_config("home");

    // Create carousel programmatically (NOT the XML factory)
    carousel_ = ui_carousel_create_obj(carousel_host_);

    // Allocate page subject on HomePanel (carousel doesn't create one for programmatic use)
    lv_subject_init_int(&page_subject_, 0);
    ui_carousel_get_state(carousel_)->page_subject = &page_subject_;

    page_containers_.clear();
    page_widgets_.clear();
    page_visible_ids_.clear();

    // Add one tile per config page
    for (int i = 0; i < config.page_count(); i++) {
        auto* container = lv_obj_create(nullptr);  // detached, carousel will adopt
        lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
        lv_obj_set_name(container, ("page_" + std::to_string(i)).c_str());
        ui_carousel_add_item(carousel_, container);
        page_containers_.push_back(container);
        page_widgets_.emplace_back();
        page_visible_ids_.emplace_back();
    }

    // Add "+" tile (only if under page limit)
    // NOTE: The "+" button uses lv_obj_add_event_cb() — acceptable exception because
    // this tile is created programmatically, not from an XML template. There is no XML
    // component for it to bind callbacks via <event_cb>.
    if (config.page_count() < kMaxPages) {
        add_page_tile_ = lv_obj_create(nullptr);
        lv_obj_set_size(add_page_tile_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(add_page_tile_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(add_page_tile_, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto* btn = lv_obj_create(add_page_tile_);
        // Style as large "+" icon button with design tokens
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<HomePanel*>(lv_event_get_user_data(e));
            self->on_add_page_clicked();
        }, LV_EVENT_CLICKED, this);
        ui_carousel_add_item(carousel_, add_page_tile_);
    }

    // Set real page count (excludes "+" tile)
    ui_carousel_set_real_page_count(carousel_, config.page_count());

    // Create arrow buttons (absolutely positioned on carousel_host)
    // ... create arrow_left_ and arrow_right_ with click callbacks ...
    // Arrow callbacks also use lv_obj_add_event_cb (same exception — programmatic)

    // Observe page changes via page_subject_
    page_observer_ = observe_int_sync<HomePanel>(&page_subject_, this,
        [](HomePanel* self, int page) { self->on_page_changed(page); });

    // Navigate to main page
    ui_carousel_goto_page(carousel_, config.main_page_index(), false);
    active_page_index_ = config.main_page_index();

    // Populate all pages
    for (int i = 0; i < config.page_count(); i++) {
        populate_page(i, true);
    }

    // Activate only the main page's widgets
    for (auto& w : page_widgets_[active_page_index_]) {
        if (w) w->on_activate();
    }

    update_arrow_visibility(active_page_index_);
}
```

- [ ] **Step 4: Implement populate_page()**

Per-page version of the old `populate_widgets()`:

```cpp
void HomePanel::populate_page(int page_index, bool force) {
    if (page_index < 0 || page_index >= static_cast<int>(page_containers_.size())) return;

    auto* container = page_containers_[page_index];
    auto& mgr = PanelWidgetManager::instance();

    if (!force) {
        auto new_ids = mgr.compute_visible_widget_ids("home", page_index);
        if (new_ids == page_visible_ids_[page_index]) return;
    }

    // Detach existing widgets, build reuse map
    WidgetReuseMap reuse;
    for (auto& w : page_widgets_[page_index]) {
        if (w) {
            auto key = w->reuse_key();
            if (!key.empty()) reuse[key] = std::move(w);
            else w.reset();
        }
    }
    page_widgets_[page_index].clear();

    // Freeze queue, drain, clean container
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(container);
    lv_obj_clean(container);

    // Populate via manager
    page_widgets_[page_index] = mgr.populate_widgets("home", container, page_index, std::move(reuse));

    // Enable event bubbling for edit mode long-press detection
    set_event_bubble_recursive(container);

    // Cache visible IDs
    page_visible_ids_[page_index] = mgr.compute_visible_widget_ids("home", page_index);

    // If this page is active and panel is active, activate widgets
    if (page_index == active_page_index_ && is_active()) {
        for (auto& w : page_widgets_[page_index]) {
            if (w) w->on_activate();
        }
    }
}
```

- [ ] **Step 5: Implement on_page_changed()**

```cpp
void HomePanel::on_page_changed(int new_page) {
    if (new_page == active_page_index_) return;
    if (new_page < 0 || new_page >= static_cast<int>(page_widgets_.size())) return;

    // Deactivate old page's widgets
    for (auto& w : page_widgets_[active_page_index_]) {
        if (w) w->on_deactivate();
    }

    active_page_index_ = new_page;

    // Activate new page's widgets
    for (auto& w : page_widgets_[active_page_index_]) {
        if (w) w->on_activate();
    }

    update_arrow_visibility(new_page);
}
```

- [ ] **Step 6: Implement arrow buttons and update_arrow_visibility()**

Create left/right arrow buttons in `build_carousel()`, positioned absolutely on the carousel_host:

```cpp
void HomePanel::update_arrow_visibility(int page) {
    auto& config = PanelWidgetManager::instance().get_widget_config("home");
    int page_count = config.page_count();

    if (arrow_left_) {
        if (page <= 0) lv_obj_add_flag(arrow_left_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(arrow_left_, LV_OBJ_FLAG_HIDDEN);
    }
    if (arrow_right_) {
        if (page >= page_count - 1) lv_obj_add_flag(arrow_right_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(arrow_right_, LV_OBJ_FLAG_HIDDEN);
    }
}
```

Arrow click callbacks:
```cpp
// Left arrow
ui_carousel_goto_page(carousel_, ui_carousel_get_current_page(carousel_) - 1);
// Right arrow
ui_carousel_goto_page(carousel_, ui_carousel_get_current_page(carousel_) + 1);
```

- [ ] **Step 7: Implement rebuild_carousel()**

Full teardown and rebuild. Called after page add/delete:

```cpp
void HomePanel::rebuild_carousel() {
    int prev_page = active_page_index_;

    // Deactivate current page's widgets
    if (prev_page >= 0 && prev_page < static_cast<int>(page_widgets_.size())) {
        for (auto& w : page_widgets_[prev_page]) {
            if (w) w->on_deactivate();
        }
    }

    // Detach all widget instances (allow reuse where possible)
    // Then clear per-page state
    for (auto& pw : page_widgets_) {
        for (auto& w : pw) w.reset();
    }
    page_widgets_.clear();
    page_containers_.clear();
    page_visible_ids_.clear();

    // Disconnect page observer before destroying carousel
    page_observer_.reset();

    // Destroy old carousel and its children
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();
    if (carousel_) {
        lv_obj_delete(carousel_);
        carousel_ = nullptr;
    }
    add_page_tile_ = nullptr;
    arrow_left_ = nullptr;
    arrow_right_ = nullptr;

    // Deinit page subject (will be re-inited in build_carousel)
    lv_subject_deinit(&page_subject_);

    // Rebuild
    build_carousel();

    // Try to restore previous page, or clamp to last valid page
    auto& config = PanelWidgetManager::instance().get_widget_config("home");
    int target = std::min(prev_page, config.page_count() - 1);
    if (target >= 0) {
        ui_carousel_goto_page(carousel_, target, false);
    }
}
```

- [ ] **Step 8: Implement on_add_page_clicked()**

```cpp
void HomePanel::on_add_page_clicked() {
    auto& config = PanelWidgetManager::instance().get_widget_config("home");
    if (config.page_count() >= kMaxPages) return;

    auto new_id = config.generate_page_id();
    config.add_page(new_id);
    config.save();

    int new_page_index = config.page_count() - 1;
    rebuild_carousel();
    ui_carousel_goto_page(carousel_, new_page_index, true);
}
```

- [ ] **Step 9: Update setup() to use build_carousel()**

In `setup()`, replace the old `populate_widgets()` call with:
```cpp
carousel_host_ = lv_obj_find_by_name(widget_, "carousel_host");
build_carousel();
```

Update `setup_widget_gate_observers()` callback to rebuild all pages:
```cpp
mgr.setup_gate_observers("home", [this]() {
    if (grid_edit_mode_.is_active()) return;
    for (int i = 0; i < static_cast<int>(page_containers_.size()); i++) {
        populate_page(i, false);
    }
});
```

- [ ] **Step 10: Update on_activate() / on_deactivate()**

`on_activate()`: activate only `page_widgets_[active_page_index_]`.
`on_deactivate()`: deactivate only `page_widgets_[active_page_index_]`. If edit mode active and not catalog open, exit edit mode.

- [ ] **Step 11: Update destructor and cleanup**

Null all carousel-related pointers. Clear `page_widgets_` and `page_containers_` vectors. The LVGL objects are children of the panel widget and get cleaned up by `lv_obj_clean()` during panel destruction.

- [ ] **Step 12: Build and test manually**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Expected: Home panel shows single page with existing widgets, dot indicator (1 dot), "+" tile accessible by swiping right.

- [ ] **Step 13: Commit**

```bash
git add include/ui_panel_home.h src/ui/ui_panel_home.cpp ui_xml/home_panel.xml
git commit -m "feat(home): multi-page carousel with swipe navigation and add-page (prestonbrown/helixscreen#484)"
```

---

## Chunk 5: Grid Edit Mode Per-Page Scoping

### Task 5: Scope edit mode to active page with carousel scroll lock

**Files:**
- Modify: `include/grid_edit_mode.h`
- Modify: `src/ui/grid_edit_mode.cpp`
- Modify: `src/ui/ui_panel_home.cpp` (edit mode entry/exit hooks)

**Docs:** Read spec sections "GridEditMode Scoping", "Carousel Scroll Lock", "Page Deletion"

- [ ] **Step 1: Add page_index parameter to GridEditMode::enter()**

In `include/grid_edit_mode.h`:
```cpp
void enter(lv_obj_t* container, PanelWidgetConfig* config, int page_index = 0);
```

Add member: `int page_index_ = 0;`

In `src/ui/grid_edit_mode.cpp`, `enter()`:
```cpp
void GridEditMode::enter(lv_obj_t* container, PanelWidgetConfig* config, int page_index) {
    // ... existing code ...
    page_index_ = page_index;
    // ... rest unchanged ...
}
```

- [ ] **Step 2: Update all config entry access to use page_index_**

In `grid_edit_mode.cpp`, **all** `config_->entries()` and `config_->mutable_entries()` calls must change. There are ~15 call sites across the file. Search for both patterns and replace:
- `config_->mutable_entries()` → `config_->page_entries_mut(page_index_)`
- `config_->entries()` → `config_->page_entries(page_index_)`

Key functions affected (non-exhaustive — grep for all occurrences):
- `enter()` / `exit()` — config access during save
- `sync_config_from_screen()` — reads entries to map widgets back to config
- `find_config_index_for_widget()` — searches entries by widget name
- `handle_click()` — reads config for selection logic
- `place_widget_from_catalog()` — reads/writes entries for new widget placement
- `handle_released()` — reads entries for snap position updates
- `on_remove_widget()` — accesses entries to disable/remove selected widget
- Various resize/drag helpers that read widget config

Run `grep -n 'entries()' src/ui/grid_edit_mode.cpp` to find all call sites before starting.

- [ ] **Step 3: Add carousel scroll lock in HomePanel edit mode entry/exit**

In `src/ui/ui_panel_home.cpp`, `on_home_grid_long_press()`:
```cpp
// Before entering edit mode:
if (carousel_) ui_carousel_set_scroll_enabled(carousel_, false);
grid_edit_mode_.enter(page_containers_[active_page_index_], &config, active_page_index_);
```

In the edit mode exit path (rebuild callback):
```cpp
// After edit mode exits:
if (carousel_) ui_carousel_set_scroll_enabled(carousel_, true);
```

- [ ] **Step 4: Add "Delete Page" button to edit mode toolbar**

In `grid_edit_mode.cpp`, add to `create_dots_overlay()` or a new helper — create a delete-page button near the existing catalog "+" button. The button:
- Is hidden when `page_index_ == config_->main_page_index()`
- Shows a trash/delete icon with "Delete Page" label
- On click: calls a `delete_page_cb_` callback registered by HomePanel

In `include/grid_edit_mode.h`:
```cpp
using DeletePageCallback = std::function<void()>;
void set_delete_page_callback(DeletePageCallback cb);
```

- [ ] **Step 5: Implement page deletion in HomePanel**

```cpp
grid_edit_mode_.set_delete_page_callback([this]() {
    modal_show_confirmation(
        lv_tr("Delete Page"),            // title
        lv_tr("Remove this page and all its widgets?"),  // message
        ConfirmationSeverity::Warning,
        lv_tr("Delete"),                 // button text
        [this](void*) {                  // on_confirm
            auto& config = PanelWidgetManager::instance().get_widget_config("home");
            config.remove_page(active_page_index_);
            config.save();
            grid_edit_mode_.exit();
            rebuild_carousel();
        },
        nullptr,  // on_cancel
        nullptr   // data
    );
});
```

- [ ] **Step 6: Build and test manually**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Test: Long-press home screen → edit mode enters, carousel scroll locked. Add a second page via "+", switch to it, long-press → edit mode on page 2. "Delete Page" button visible (not on main page).

- [ ] **Step 7: Commit**

```bash
git add include/grid_edit_mode.h src/ui/grid_edit_mode.cpp src/ui/ui_panel_home.cpp
git commit -m "feat(edit): scope grid edit mode to active page with scroll lock and page deletion (prestonbrown/helixscreen#484)"
```

---

## Chunk 6: Polish and Integration Testing

### Task 6: Arrow button styling, indicator polish, edge cases

**Files:**
- Modify: `src/ui/ui_panel_home.cpp` (arrow styling, edge cases)
- Modify: `src/ui/ui_carousel.cpp` (indicator styling for single page)

- [ ] **Step 1: Style arrow buttons with design tokens**

Use `#icon_secondary` color, `#space_sm` padding, semi-transparent background. Chevron icons from MDI font (`chevron-left`, `chevron-right`). Buttons should be ~40x40px touch targets.

Applying [L008]: Use design tokens for spacing and colors, semantic widgets for typography.
Applying [L009]: If chevron icons need to be added to the font, update `codepoints.h` + `make regen-fonts` + rebuild.

- [ ] **Step 2: Handle single-page case gracefully**

When there's only 1 page:
- No dot indicators shown (carousel already handles this)
- No arrow buttons shown
- "+" tile is still accessible by swiping

- [ ] **Step 3: Handle page deletion when only 2 pages exist**

After deleting a page, if only 1 remains:
- Arrow buttons disappear
- Dot indicators disappear
- Carousel degrades to single-page mode

- [ ] **Step 4: Verify rebuild_carousel preserves active page**

When rebuilding after config changes, if the previously active page still exists, navigate back to it. If it was deleted, navigate to the nearest page.

- [ ] **Step 5: Manual integration test**

Run: `make -j && ./build/bin/helix-screen --test -vv`

Test checklist:
- [ ] Default config: 1 page, no dots, no arrows, swipe right shows "+" tile
- [ ] Tap "+": new blank page created, carousel animates to it, dots appear (2 dots)
- [ ] Swipe between pages: dots update, arrows show/hide at edges
- [ ] Arrow tap: navigates to next/prev page
- [ ] Long-press page 2 → edit mode: carousel locked, can drag widgets
- [ ] "Delete Page" button visible on page 2, hidden on main page
- [ ] Delete page 2: confirmation modal, page removed, back to page 1
- [ ] Add widgets to page 2 via catalog: widgets appear on page 2 only
- [ ] Restart app: multi-page config persists, pages load correctly
- [ ] Legacy config: old single-page config migrates cleanly on first load

- [ ] **Step 6: Commit**

```bash
git add src/ui/ui_panel_home.cpp src/ui/ui_carousel.cpp
git commit -m "fix(home): polish arrow styling, single-page handling, and rebuild edge cases (prestonbrown/helixscreen#484)"
```
