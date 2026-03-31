# Snake Game Rebuild Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the snake game easter egg for smooth interpolated animation, responsive adaptive input (D-pad on resistive/SDL, swipe on capacitive), and enhanced visual effects.

**Architecture:** Refactor in-place in `src/ui/ui_snake_game.cpp`. Replace the fixed-interval game tick with a fixed-timestep + interpolated rendering loop. Organize state into plain structs within the anonymous namespace. Add an adaptive input system that detects display backend type to choose between D-pad overlay and improved swipe controls.

**Tech Stack:** C++17, LVGL 9.5, spdlog. No new dependencies.

**Spec:** `docs/devel/specs/2026-03-31-snake-game-rebuild-design.md`

---

## File Structure

All changes are in two existing files:

| File | Role |
|------|------|
| `src/ui/ui_snake_game.cpp` | Full rewrite — game loop, input, rendering, state |
| `include/ui_snake_game.h` | No changes (public API is unchanged) |

Additional includes needed in the .cpp:
- `display_manager.h` — for `DisplayManager::instance()->backend()->type()` to detect SDL vs hardware
- `display_backend.h` — for `DisplayBackendType` enum

---

### Task 1: State Structs and Constants

Replace the ~20 anonymous namespace globals with organized structs and updated constants.

**Files:**
- Modify: `src/ui/ui_snake_game.cpp:30-121`

- [ ] **Step 1: Define new state structs and constants**

Replace everything from `// CONSTANTS` through `// FORWARD DECLARATIONS` (lines 30-125) with:

```cpp
#include "display_manager.h"
#include "display_backend.h"

#include <array>
#include <cmath>
#include <vector>

namespace helix {

// ============================================================================
// CONSTANTS
// ============================================================================

static constexpr int32_t CELL_SIZE = 20;
static constexpr uint32_t INITIAL_TICK_MS = 150;
static constexpr uint32_t MIN_TICK_MS = 70;
static constexpr int SPEED_UP_INTERVAL = 5;
static constexpr const char* HIGH_SCORE_KEY = "/display/frame_counter";

// Render timer fires at ~60fps for smooth interpolation
static constexpr uint32_t RENDER_TICK_MS = 16;

// Input queue depth (buffer one turn ahead)
static constexpr size_t INPUT_QUEUE_MAX = 2;

// Swipe threshold (reduced from 20 for better responsiveness)
static constexpr int32_t SWIPE_THRESHOLD = 12;

// Particle system
static constexpr int MAX_PARTICLES = 8;
static constexpr uint32_t PARTICLE_LIFETIME_MS = 300;

// Death animation timing
static constexpr uint32_t DEATH_FLASH_MS = 50;
static constexpr uint32_t DEATH_SHRINK_MS = 200;
static constexpr uint32_t DEATH_CARD_MS = 300;
static constexpr uint32_t DEATH_INPUT_MS = 600;

// Food pulse animation
static constexpr float FOOD_PULSE_HZ = 2.0f;
static constexpr float FOOD_PULSE_AMPLITUDE = 2.0f;

// Head squash on direction change
static constexpr float HEAD_SQUASH_FACTOR = 0.15f;
static constexpr uint32_t HEAD_SQUASH_MS = 100;

// Tail taper: last N segments fade in width and opacity
static constexpr int TAIL_TAPER_COUNT = 3;

// Speed tier colors (border tint progression)
static constexpr uint32_t TIER_COLORS[] = {
    0x444444, // Tier 0: neutral gray
    0x1a5a1a, // Tier 1: green
    0x5a5a1a, // Tier 2: yellow
    0x5a3a1a, // Tier 3: orange
    0x5a1a1a, // Tier 4: red
};
static constexpr int NUM_TIERS = static_cast<int>(sizeof(TIER_COLORS) / sizeof(TIER_COLORS[0]));

// Filament colors for snake body
static constexpr uint32_t FILAMENT_COLORS[] = {
    0xED1C24, 0x00A651, 0x2E3192, 0xFFF200, 0xF7941D,
    0x92278F, 0x00AEEF, 0xEC008C, 0x8DC63F, 0xF15A24,
};
static constexpr int NUM_FILAMENT_COLORS =
    static_cast<int>(sizeof(FILAMENT_COLORS) / sizeof(FILAMENT_COLORS[0]));

// Food spool colors
static constexpr uint32_t FOOD_COLORS[] = {
    0xFF6B35, 0x00D2FF, 0xFFD700, 0xFF1493, 0x7FFF00, 0xDA70D6,
};
static constexpr int NUM_FOOD_COLORS =
    static_cast<int>(sizeof(FOOD_COLORS) / sizeof(FOOD_COLORS[0]));

// ============================================================================
// TYPES
// ============================================================================

enum class Direction { UP, DOWN, LEFT, RIGHT };

struct GridPos {
    int x;
    int y;
    bool operator==(const GridPos& o) const { return x == o.x && y == o.y; }
    bool operator!=(const GridPos& o) const { return !(*this == o); }
};

enum class InputMode { SWIPE, DPAD };

struct Particle {
    float x, y;      // pixel position
    float vx, vy;    // velocity (pixels per second)
    lv_color_t color;
    uint32_t born_ms; // timestamp when created
    bool active;
};

// ============================================================================
// GAME STATE
// ============================================================================

namespace {

// --- Grid ---
struct GridState {
    int cols = 0;
    int rows = 0;
    int offset_x = 0;
    int offset_y = 0;
    std::vector<GridPos> free_cells; // for O(1) food placement

    void rebuild_free_cells(const std::deque<GridPos>& snake) {
        free_cells.clear();
        free_cells.reserve(cols * rows);
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                GridPos p{x, y};
                if (std::find(snake.begin(), snake.end(), p) == snake.end()) {
                    free_cells.push_back(p);
                }
            }
        }
    }

    void remove_cell(const GridPos& p) {
        auto it = std::find(free_cells.begin(), free_cells.end(), p);
        if (it != free_cells.end()) {
            // Swap with last and pop for O(1) removal
            std::swap(*it, free_cells.back());
            free_cells.pop_back();
        }
    }

    void add_cell(const GridPos& p) {
        free_cells.push_back(p);
    }
} g_grid;

// --- Game logic ---
struct GameState {
    std::deque<GridPos> snake;
    std::deque<GridPos> prev_snake; // snapshot for interpolation
    Direction direction = Direction::RIGHT;
    Direction prev_direction = Direction::RIGHT;
    bool game_over = false;
    bool game_started = false;
    int score = 0;
    int high_score = 0;
    uint32_t tick_ms = INITIAL_TICK_MS;
    int speed_tier = 0;

    // Food
    GridPos food = {0, 0};
    lv_color_t food_color = {};
    lv_color_t snake_color = {};
} g_game;

// --- Render / animation ---
struct RenderState {
    float interp = 0.0f;           // 0.0..1.0 between game ticks
    uint32_t tick_accumulator = 0; // ms accumulated since last game tick
    uint32_t last_render_ms = 0;   // timestamp of last render frame

    // Food pulse
    float food_pulse_phase = 0.0f;

    // Head squash
    uint32_t squash_start_ms = 0;
    bool squash_active = false;

    // Particles
    std::array<Particle, MAX_PARTICLES> particles = {};

    // Death animation
    uint32_t death_start_ms = 0;
    bool death_animating = false;
    bool death_input_ready = false;
} g_render;

// --- Input ---
struct InputState {
    InputMode mode = InputMode::SWIPE;
    std::array<Direction, INPUT_QUEUE_MAX> queue = {};
    size_t queue_size = 0;

    // Touch tracking
    lv_point_t touch_start = {0, 0};
    bool swipe_handled = false;

    void push_direction(Direction dir, Direction current_dir) {
        // 180-degree reversal prevention
        Direction check_against = queue_size > 0 ? queue[queue_size - 1] : current_dir;
        if ((dir == Direction::UP && check_against == Direction::DOWN) ||
            (dir == Direction::DOWN && check_against == Direction::UP) ||
            (dir == Direction::LEFT && check_against == Direction::RIGHT) ||
            (dir == Direction::RIGHT && check_against == Direction::LEFT)) {
            return;
        }
        if (queue_size < INPUT_QUEUE_MAX) {
            queue[queue_size++] = dir;
        }
    }

    bool pop_direction(Direction& out) {
        if (queue_size == 0) return false;
        out = queue[0];
        // Shift remaining
        for (size_t i = 1; i < queue_size; i++) {
            queue[i - 1] = queue[i];
        }
        queue_size--;
        return true;
    }
} g_input;

// --- LVGL objects ---
lv_obj_t* g_overlay = nullptr;
lv_obj_t* g_game_area = nullptr;
lv_obj_t* g_score_label = nullptr;
lv_obj_t* g_gameover_label = nullptr;
lv_obj_t* g_close_btn = nullptr;
lv_timer_t* g_render_timer = nullptr; // single timer for both logic + rendering

// D-pad buttons (only created in DPAD mode)
lv_obj_t* g_dpad_up = nullptr;
lv_obj_t* g_dpad_down = nullptr;
lv_obj_t* g_dpad_left = nullptr;
lv_obj_t* g_dpad_right = nullptr;
```

- [ ] **Step 2: Build and verify it compiles**

The file won't compile yet (forward declarations and function bodies still reference old globals). This step is just to verify the struct definitions are syntactically correct. Comment out the old function bodies temporarily if needed, or wait until Task 2 to verify compilation.

Run: `make -j 2>&1 | head -30`
Expected: May have errors from old code referencing removed globals — that's expected, will fix in subsequent tasks.

---

### Task 2: Game Loop — Fixed Timestep with Interpolated Rendering

Replace the old `game_tick` timer with a single render timer that accumulates time, runs game logic at fixed intervals, and calculates interpolation factor.

**Files:**
- Modify: `src/ui/ui_snake_game.cpp` (game logic section)

- [ ] **Step 1: Write the render tick function**

This replaces both the old `game_tick` and drives rendering. Add after the state structs:

```cpp
// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

void init_game();
void game_logic_tick();
void render_tick(lv_timer_t* timer);
void draw_cb(lv_event_t* e);
void touch_cb(lv_event_t* e);
void input_cb(lv_event_t* e);
void close_cb(lv_event_t* e);
void place_food();
void update_score_label();
void show_game_over();
void create_overlay();
void destroy_overlay();
void spawn_eat_particles(int32_t px, int32_t py, lv_color_t color);
void update_particles(uint32_t now_ms);
void create_dpad(lv_obj_t* parent);
void dpad_cb(lv_event_t* e);
InputMode detect_input_mode();

// ============================================================================
// HELPERS
// ============================================================================

uint32_t now_ms() {
    return lv_tick_get();
}

void grid_to_pixel(const GridPos& pos, int32_t& px, int32_t& py) {
    px = g_grid.offset_x + pos.x * CELL_SIZE + CELL_SIZE / 2;
    py = g_grid.offset_y + pos.y * CELL_SIZE + CELL_SIZE / 2;
}

lv_color_t random_filament_color() {
    return lv_color_hex(FILAMENT_COLORS[rand() % NUM_FILAMENT_COLORS]);
}

lv_color_t random_food_color() {
    return lv_color_hex(FOOD_COLORS[rand() % NUM_FOOD_COLORS]);
}

int current_speed_tier() {
    return LV_MIN(g_game.score / SPEED_UP_INTERVAL, NUM_TIERS - 1);
}

// ============================================================================
// GAME LOGIC (runs at fixed timestep)
// ============================================================================

void game_logic_tick() {
    if (g_game.game_over || !g_game.game_started) return;

    // Snapshot previous state for interpolation
    g_game.prev_snake = g_game.snake;
    g_game.prev_direction = g_game.direction;

    // Consume one input from queue
    Direction new_dir;
    if (g_input.pop_direction(new_dir)) {
        g_game.direction = new_dir;
    }

    // Calculate new head position
    GridPos head = g_game.snake.back();
    GridPos new_head = head;
    switch (g_game.direction) {
    case Direction::UP:    new_head.y--; break;
    case Direction::DOWN:  new_head.y++; break;
    case Direction::LEFT:  new_head.x--; break;
    case Direction::RIGHT: new_head.x++; break;
    }

    // Wall collision
    if (new_head.x < 0 || new_head.x >= g_grid.cols ||
        new_head.y < 0 || new_head.y >= g_grid.rows) {
        g_game.game_over = true;
        show_game_over();
        return;
    }

    // Self collision
    if (std::find(g_game.snake.begin(), g_game.snake.end(), new_head) != g_game.snake.end()) {
        g_game.game_over = true;
        show_game_over();
        return;
    }

    // Move snake
    g_game.snake.push_back(new_head);
    g_grid.remove_cell(new_head);

    // Check food
    if (new_head == g_game.food) {
        g_game.score++;

        // Spawn eat particles at food pixel position
        int32_t fx, fy;
        grid_to_pixel(g_game.food, fx, fy);
        spawn_eat_particles(static_cast<float>(fx), static_cast<float>(fy), g_game.food_color);

        // Trigger head squash
        g_render.squash_start_ms = now_ms();
        g_render.squash_active = true;

        // Update speed tier
        int new_tier = current_speed_tier();
        if (new_tier != g_game.speed_tier) {
            g_game.speed_tier = new_tier;
        }

        // Speed up
        if (g_game.score % SPEED_UP_INTERVAL == 0 && g_game.tick_ms > MIN_TICK_MS) {
            g_game.tick_ms -= 10;
        }

        update_score_label();
        place_food();
    } else {
        // Remove tail
        GridPos old_tail = g_game.snake.front();
        g_game.snake.pop_front();
        g_grid.add_cell(old_tail);
    }

    // Sync prev_snake size for interpolation (if food was eaten, prev is shorter)
    // We handle this in the renderer by checking size mismatch
}

// ============================================================================
// RENDER LOOP
// ============================================================================

void render_tick(lv_timer_t* /*timer*/) {
    uint32_t now = now_ms();
    uint32_t dt = now - g_render.last_render_ms;
    g_render.last_render_ms = now;

    // Clamp dt to avoid spiral of death after pause/debug
    if (dt > 200) dt = 200;

    if (g_game.game_started && !g_game.game_over) {
        g_render.tick_accumulator += dt;

        // Run game logic ticks
        while (g_render.tick_accumulator >= g_game.tick_ms) {
            game_logic_tick();
            g_render.tick_accumulator -= g_game.tick_ms;
            if (g_game.game_over) break;
        }

        // Calculate interpolation factor
        g_render.interp = static_cast<float>(g_render.tick_accumulator) /
                          static_cast<float>(g_game.tick_ms);
    }

    // Update food pulse phase
    g_render.food_pulse_phase += static_cast<float>(dt) / 1000.0f * FOOD_PULSE_HZ * 2.0f * 3.14159f;
    if (g_render.food_pulse_phase > 6.28318f) g_render.food_pulse_phase -= 6.28318f;

    // Update head squash
    if (g_render.squash_active && (now - g_render.squash_start_ms) > HEAD_SQUASH_MS) {
        g_render.squash_active = false;
    }

    // Update particles
    update_particles(now);

    // Update death animation state
    if (g_render.death_animating) {
        uint32_t elapsed = now - g_render.death_start_ms;
        if (elapsed >= DEATH_INPUT_MS) {
            g_render.death_input_ready = true;
        }
    }

    // Request redraw
    if (g_game_area) {
        lv_obj_invalidate(g_game_area);
    }
}
```

- [ ] **Step 2: Build and verify**

Run: `make -j 2>&1 | tail -20`
Expected: Should compile (particle functions are forward-declared, will be implemented in Task 4).

- [ ] **Step 3: Commit**

```bash
git add src/ui/ui_snake_game.cpp
git commit -m "refactor(snake): replace fixed-tick with interpolated game loop"
```

---

### Task 3: Input System — Adaptive D-pad / Swipe

Implement the adaptive input system with D-pad for resistive/SDL and improved swipe for capacitive.

**Files:**
- Modify: `src/ui/ui_snake_game.cpp` (input section)

- [ ] **Step 1: Write input mode detection**

```cpp
InputMode detect_input_mode() {
    auto* dm = DisplayManager::instance();
    if (dm && dm->backend()) {
        if (dm->backend()->type() == DisplayBackendType::SDL) {
            return InputMode::DPAD;
        }
    }
    // TODO: Add resistive touchscreen detection when we have a config flag.
    // For now, default to SWIPE on all hardware (D-pad on SDL only).
    // AD5M users will get D-pad once we add device detection.
    return InputMode::SWIPE;
}
```

- [ ] **Step 2: Write improved swipe handler**

Replace the old `touch_cb` with:

```cpp
void touch_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &g_input.touch_start);
        g_input.swipe_handled = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (g_input.swipe_handled || g_game.game_over) return;

        lv_point_t current;
        lv_indev_get_point(indev, &current);

        int32_t dx = current.x - g_input.touch_start.x;
        int32_t dy = current.y - g_input.touch_start.y;
        int32_t abs_dx = LV_ABS(dx);
        int32_t abs_dy = LV_ABS(dy);

        if (abs_dx < SWIPE_THRESHOLD && abs_dy < SWIPE_THRESHOLD) return;

        Direction dir;
        if (abs_dx > abs_dy) {
            dir = dx > 0 ? Direction::RIGHT : Direction::LEFT;
        } else {
            dir = dy > 0 ? Direction::DOWN : Direction::UP;
        }
        g_input.push_direction(dir, g_game.direction);
        g_input.swipe_handled = true;

        // Reset origin for chained swipes without lifting finger
        g_input.touch_start = current;
    } else if (code == LV_EVENT_RELEASED) {
        if (!g_input.swipe_handled && !g_game.game_over) {
            // Fallback swipe detection on release
            lv_point_t end;
            lv_indev_get_point(indev, &end);

            int32_t dx = end.x - g_input.touch_start.x;
            int32_t dy = end.y - g_input.touch_start.y;
            int32_t abs_dx = LV_ABS(dx);
            int32_t abs_dy = LV_ABS(dy);

            if (abs_dx >= SWIPE_THRESHOLD || abs_dy >= SWIPE_THRESHOLD) {
                Direction dir;
                if (abs_dx > abs_dy) {
                    dir = dx > 0 ? Direction::RIGHT : Direction::LEFT;
                } else {
                    dir = dy > 0 ? Direction::DOWN : Direction::UP;
                }
                g_input.push_direction(dir, g_game.direction);
            }
        }

        // Tap to restart when game over (and death animation is done)
        if (g_game.game_over && g_render.death_input_ready && !g_input.swipe_handled) {
            lv_point_t end;
            lv_indev_get_point(indev, &end);
            int32_t dx = LV_ABS(end.x - g_input.touch_start.x);
            int32_t dy = LV_ABS(end.y - g_input.touch_start.y);
            if (dx < SWIPE_THRESHOLD && dy < SWIPE_THRESHOLD) {
                init_game();
            }
        }

        g_input.swipe_handled = false;
    }
}
```

- [ ] **Step 3: Write keyboard handler**

```cpp
void input_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;

    uint32_t key = lv_event_get_key(e);

    if (key == LV_KEY_ESC) {
        SnakeGame::hide();
        return;
    }

    if (g_game.game_over && g_render.death_input_ready) {
        init_game();
        return;
    }

    switch (key) {
    case LV_KEY_UP:    g_input.push_direction(Direction::UP, g_game.direction);    break;
    case LV_KEY_DOWN:  g_input.push_direction(Direction::DOWN, g_game.direction);  break;
    case LV_KEY_LEFT:  g_input.push_direction(Direction::LEFT, g_game.direction);  break;
    case LV_KEY_RIGHT: g_input.push_direction(Direction::RIGHT, g_game.direction); break;
    default: break;
    }
}

void close_cb(lv_event_t* /*e*/) {
    SnakeGame::hide();
}
```

- [ ] **Step 4: Write D-pad overlay**

```cpp
void dpad_cb(lv_event_t* e) {
    auto* btn = lv_event_get_current_target_obj(e);
    Direction dir;
    if (btn == g_dpad_up)         dir = Direction::UP;
    else if (btn == g_dpad_down)  dir = Direction::DOWN;
    else if (btn == g_dpad_left)  dir = Direction::LEFT;
    else if (btn == g_dpad_right) dir = Direction::RIGHT;
    else return;

    if (g_game.game_over && g_render.death_input_ready) {
        init_game();
        return;
    }

    g_input.push_direction(dir, g_game.direction);
}

lv_obj_t* create_dpad_button(lv_obj_t* parent, const char* label, lv_align_t align,
                              int32_t x_ofs, int32_t y_ofs) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, 56, 56);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(btn, dpad_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(lbl, LV_OPA_70, LV_PART_MAIN);
    lv_obj_center(lbl);

    return btn;
}

void create_dpad(lv_obj_t* parent) {
    // D-pad centered horizontally, anchored to bottom of game area
    // Layout: UP top center, LEFT/RIGHT middle, DOWN bottom center
    int32_t cx = 0;  // center-relative offsets
    int32_t btn_gap = 58; // button size + small gap

    g_dpad_up    = create_dpad_button(parent, LV_SYMBOL_UP,    LV_ALIGN_BOTTOM_MID, cx, -(btn_gap * 2 + 8));
    g_dpad_left  = create_dpad_button(parent, LV_SYMBOL_LEFT,  LV_ALIGN_BOTTOM_MID, -btn_gap, -(btn_gap + 4));
    g_dpad_right = create_dpad_button(parent, LV_SYMBOL_RIGHT, LV_ALIGN_BOTTOM_MID, btn_gap, -(btn_gap + 4));
    g_dpad_down  = create_dpad_button(parent, LV_SYMBOL_DOWN,  LV_ALIGN_BOTTOM_MID, cx, -4);
}
```

- [ ] **Step 5: Build and verify**

Run: `make -j 2>&1 | tail -20`
Expected: Compiles. Input system complete.

- [ ] **Step 6: Commit**

```bash
git add src/ui/ui_snake_game.cpp
git commit -m "feat(snake): adaptive input system — D-pad on SDL, swipe on touch"
```

---

### Task 4: Particle System and Animation Effects

Implement eat particles, food pulse, head squash, and tail tapering.

**Files:**
- Modify: `src/ui/ui_snake_game.cpp` (rendering section)

- [ ] **Step 1: Write particle system**

```cpp
void spawn_eat_particles(int32_t px, int32_t py, lv_color_t color) {
    for (auto& p : g_render.particles) {
        if (p.active) continue;
        p.active = true;
        p.x = static_cast<float>(px);
        p.y = static_cast<float>(py);
        // Random velocity: -120..120 pixels/sec in each axis
        p.vx = static_cast<float>((rand() % 240) - 120);
        p.vy = static_cast<float>((rand() % 240) - 120);
        p.color = color;
        p.born_ms = now_ms();
    }
}

void update_particles(uint32_t now) {
    for (auto& p : g_render.particles) {
        if (!p.active) continue;
        uint32_t age = now - p.born_ms;
        if (age >= PARTICLE_LIFETIME_MS) {
            p.active = false;
            continue;
        }
        float dt_sec = static_cast<float>(RENDER_TICK_MS) / 1000.0f;
        p.x += p.vx * dt_sec;
        p.y += p.vy * dt_sec;
    }
}

void draw_particles(lv_layer_t* layer, const lv_area_t& obj_area) {
    uint32_t now = now_ms();
    for (const auto& p : g_render.particles) {
        if (!p.active) continue;
        uint32_t age = now - p.born_ms;
        float life = 1.0f - static_cast<float>(age) / static_cast<float>(PARTICLE_LIFETIME_MS);

        lv_draw_arc_dsc_t arc_dsc;
        lv_draw_arc_dsc_init(&arc_dsc);
        arc_dsc.color = p.color;
        arc_dsc.start_angle = 0;
        arc_dsc.end_angle = 360;
        arc_dsc.radius = LV_MAX(1, static_cast<int32_t>(4.0f * life));
        arc_dsc.width = arc_dsc.radius;
        arc_dsc.opa = static_cast<lv_opa_t>(LV_OPA_COVER * life);
        arc_dsc.center.x = obj_area.x1 + static_cast<int32_t>(p.x);
        arc_dsc.center.y = obj_area.y1 + static_cast<int32_t>(p.y);
        lv_draw_arc(layer, &arc_dsc);
    }
}
```

- [ ] **Step 2: Build and verify**

Run: `make -j 2>&1 | tail -10`
Expected: Compiles.

- [ ] **Step 3: Commit**

```bash
git add src/ui/ui_snake_game.cpp
git commit -m "feat(snake): particle system for food eat burst effect"
```

---

### Task 5: Draw Callback — Interpolated Rendering with Effects

Rewrite the draw callback to use interpolated positions, tail tapering, head squash, food pulse, speed tier border colors, and particle rendering.

**Files:**
- Modify: `src/ui/ui_snake_game.cpp` (draw_cb and tube drawing)

- [ ] **Step 1: Keep existing tube drawing helpers unchanged**

The `draw_flat_line()` and `draw_tube_segment()` functions (lines 142-187 in the original) are reused as-is. Copy them into the new file structure if not already present.

- [ ] **Step 2: Write the new draw_cb**

```cpp
/// Linearly interpolate between two grid positions in pixel space
void lerp_grid_to_pixel(const GridPos& from, const GridPos& to, float t,
                        int32_t& px, int32_t& py) {
    float fx1 = static_cast<float>(g_grid.offset_x + from.x * CELL_SIZE + CELL_SIZE / 2);
    float fy1 = static_cast<float>(g_grid.offset_y + from.y * CELL_SIZE + CELL_SIZE / 2);
    float fx2 = static_cast<float>(g_grid.offset_x + to.x * CELL_SIZE + CELL_SIZE / 2);
    float fy2 = static_cast<float>(g_grid.offset_y + to.y * CELL_SIZE + CELL_SIZE / 2);
    px = static_cast<int32_t>(fx1 + (fx2 - fx1) * t);
    py = static_cast<int32_t>(fy1 + (fy2 - fy1) * t);
}

void draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_obj_t* obj = lv_event_get_current_target_obj(e);

    lv_area_t obj_area;
    lv_obj_get_coords(obj, &obj_area);

    // --- Background with subtle grid ---
    {
        lv_draw_rect_dsc_t bg_dsc;
        lv_draw_rect_dsc_init(&bg_dsc);
        bg_dsc.bg_color = lv_color_hex(0x0a0a0a);
        bg_dsc.bg_opa = LV_OPA_COVER;

        lv_area_t bg_area = {
            obj_area.x1 + g_grid.offset_x,
            obj_area.y1 + g_grid.offset_y,
            obj_area.x1 + g_grid.offset_x + g_grid.cols * CELL_SIZE,
            obj_area.y1 + g_grid.offset_y + g_grid.rows * CELL_SIZE,
        };
        lv_draw_rect(layer, &bg_dsc, &bg_area);

        // Subtle grid lines
        lv_draw_line_dsc_t grid_dsc;
        lv_draw_line_dsc_init(&grid_dsc);
        grid_dsc.color = lv_color_hex(0x1a1a1a);
        grid_dsc.width = 1;

        for (int x = 0; x <= g_grid.cols; x++) {
            int32_t px = obj_area.x1 + g_grid.offset_x + x * CELL_SIZE;
            grid_dsc.p1 = {px, bg_area.y1};
            grid_dsc.p2 = {px, bg_area.y2};
            lv_draw_line(layer, &grid_dsc);
        }
        for (int y = 0; y <= g_grid.rows; y++) {
            int32_t py = obj_area.y1 + g_grid.offset_y + y * CELL_SIZE;
            grid_dsc.p1 = {bg_area.x1, py};
            grid_dsc.p2 = {bg_area.x2, py};
            lv_draw_line(layer, &grid_dsc);
        }
    }

    // --- Border with speed tier color ---
    {
        lv_draw_rect_dsc_t border_dsc;
        lv_draw_rect_dsc_init(&border_dsc);
        border_dsc.bg_opa = LV_OPA_TRANSP;
        border_dsc.border_color = lv_color_hex(TIER_COLORS[g_game.speed_tier]);
        border_dsc.border_opa = LV_OPA_COVER;
        border_dsc.border_width = 2;
        border_dsc.radius = 4;

        lv_area_t border_area = {
            obj_area.x1 + g_grid.offset_x - 2,
            obj_area.y1 + g_grid.offset_y - 2,
            obj_area.x1 + g_grid.offset_x + g_grid.cols * CELL_SIZE + 1,
            obj_area.y1 + g_grid.offset_y + g_grid.rows * CELL_SIZE + 1,
        };
        lv_draw_rect(layer, &border_dsc, &border_area);
    }

    if (!g_game.game_started) return;

    // --- Food with pulse animation ---
    {
        int32_t fx, fy;
        grid_to_pixel(g_game.food, fx, fy);
        fx += obj_area.x1;
        fy += obj_area.y1;
        float pulse = sinf(g_render.food_pulse_phase) * FOOD_PULSE_AMPLITUDE;
        int32_t food_radius = CELL_SIZE / 4 + static_cast<int32_t>(pulse);
        ui_draw_spool_box(layer, fx, fy, g_game.food_color, true, LV_MAX(2, food_radius));
    }

    // --- Snake body with interpolation and tail taper ---
    lv_color_t body_color = g_render.death_animating ? lv_color_hex(0xCC2222) : g_game.snake_color;
    int32_t tube_width = CELL_SIZE * 2 / 3;
    float interp = g_render.interp;

    size_t snake_len = g_game.snake.size();
    size_t prev_len = g_game.prev_snake.size();

    for (size_t i = 1; i < snake_len; i++) {
        int32_t x1, y1, x2, y2;

        // Use interpolation if prev_snake is available and same length
        if (prev_len == snake_len && i < prev_len) {
            lerp_grid_to_pixel(g_game.prev_snake[i - 1], g_game.snake[i - 1], interp, x1, y1);
            lerp_grid_to_pixel(g_game.prev_snake[i], g_game.snake[i], interp, x2, y2);
        } else {
            // Fallback: no interpolation (first tick, or snake grew)
            grid_to_pixel(g_game.snake[i - 1], x1, y1);
            grid_to_pixel(g_game.snake[i], x2, y2);
        }

        x1 += obj_area.x1;
        y1 += obj_area.y1;
        x2 += obj_area.x1;
        y2 += obj_area.y1;

        bool is_head = (i == snake_len - 1);
        int32_t w = tube_width;
        lv_color_t c = body_color;

        // Tail tapering: last TAIL_TAPER_COUNT segments fade
        if (static_cast<int>(i) < TAIL_TAPER_COUNT) {
            float taper = static_cast<float>(i) / static_cast<float>(TAIL_TAPER_COUNT);
            w = static_cast<int32_t>(static_cast<float>(tube_width) * (0.4f + 0.6f * taper));
        }

        // Head: wider, brighter, with squash effect
        if (is_head) {
            w = tube_width + 2;
            c = ui_color_lighten(body_color, 20);

            if (g_render.squash_active) {
                // Squash perpendicular to movement direction
                w = static_cast<int32_t>(static_cast<float>(w) * (1.0f + HEAD_SQUASH_FACTOR));
            }
        }

        // Death shrink animation: segments shrink from tail to head
        if (g_render.death_animating) {
            uint32_t elapsed = now_ms() - g_render.death_start_ms;
            if (elapsed < DEATH_SHRINK_MS) {
                float progress = static_cast<float>(elapsed) / static_cast<float>(DEATH_SHRINK_MS);
                float seg_threshold = progress * static_cast<float>(snake_len);
                if (static_cast<float>(i) < seg_threshold) {
                    float seg_progress = (seg_threshold - static_cast<float>(i)) /
                                         static_cast<float>(snake_len);
                    w = static_cast<int32_t>(static_cast<float>(w) * (1.0f - LV_MIN(1.0f, seg_progress)));
                    if (w < 1) continue;
                }
            }
        }

        draw_tube_segment(layer, x1, y1, x2, y2, c, w);
    }

    // --- Snake eyes ---
    if (snake_len >= 2) {
        int32_t hx, hy;
        if (prev_len == snake_len) {
            lerp_grid_to_pixel(g_game.prev_snake.back(), g_game.snake.back(), interp, hx, hy);
        } else {
            grid_to_pixel(g_game.snake.back(), hx, hy);
        }
        hx += obj_area.x1;
        hy += obj_area.y1;

        int32_t eye_offset = CELL_SIZE / 4;
        int32_t ex1 = hx, ey1 = hy, ex2 = hx, ey2 = hy;

        switch (g_game.direction) {
        case Direction::UP:
        case Direction::DOWN:
            ex1 = hx - eye_offset;
            ex2 = hx + eye_offset;
            ey1 = ey2 = hy + (g_game.direction == Direction::UP ? -eye_offset / 2 : eye_offset / 2);
            break;
        case Direction::LEFT:
        case Direction::RIGHT:
            ey1 = hy - eye_offset;
            ey2 = hy + eye_offset;
            ex1 = ex2 = hx + (g_game.direction == Direction::LEFT ? -eye_offset / 2 : eye_offset / 2);
            break;
        }

        lv_draw_arc_dsc_t eye_dsc;
        lv_draw_arc_dsc_init(&eye_dsc);
        eye_dsc.width = 3;
        eye_dsc.start_angle = 0;
        eye_dsc.end_angle = 360;
        eye_dsc.color = lv_color_white();
        eye_dsc.radius = 3;

        eye_dsc.center = {ex1, ey1};
        lv_draw_arc(layer, &eye_dsc);
        eye_dsc.center = {ex2, ey2};
        lv_draw_arc(layer, &eye_dsc);

        eye_dsc.color = lv_color_black();
        eye_dsc.radius = 2;
        eye_dsc.width = 2;
        eye_dsc.center = {ex1, ey1};
        lv_draw_arc(layer, &eye_dsc);
        eye_dsc.center = {ex2, ey2};
        lv_draw_arc(layer, &eye_dsc);
    }

    // --- Particles ---
    draw_particles(layer, obj_area);

    // --- Death flash overlay ---
    if (g_render.death_animating) {
        uint32_t elapsed = now_ms() - g_render.death_start_ms;
        if (elapsed < DEATH_FLASH_MS) {
            lv_draw_rect_dsc_t flash_dsc;
            lv_draw_rect_dsc_init(&flash_dsc);
            flash_dsc.bg_color = lv_color_white();
            flash_dsc.bg_opa = LV_OPA_20;
            lv_area_t flash_area = {obj_area.x1, obj_area.y1, obj_area.x2, obj_area.y2};
            lv_draw_rect(layer, &flash_dsc, &flash_area);
        }
    }
}
```

- [ ] **Step 3: Build and verify**

Run: `make -j 2>&1 | tail -10`
Expected: Compiles.

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_snake_game.cpp
git commit -m "feat(snake): interpolated rendering with tail taper, food pulse, death animation"
```

---

### Task 6: Game Init, Food Placement, Score, and Game Over

Implement the remaining game logic: initialization, O(1) food placement, score display, and death animation trigger.

**Files:**
- Modify: `src/ui/ui_snake_game.cpp`

- [ ] **Step 1: Write game init and food placement**

```cpp
void load_high_score() {
    auto* cfg = Config::get_instance();
    if (cfg) {
        g_game.high_score = cfg->get<int>(HIGH_SCORE_KEY, 0);
    }
    spdlog::debug("[SnakeGame] Loaded high score: {}", g_game.high_score);
}

void save_high_score() {
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set(HIGH_SCORE_KEY, g_game.high_score);
        cfg->save();
    }
    spdlog::info("[SnakeGame] Saved new high score: {}", g_game.high_score);
}

void place_food() {
    if (g_grid.free_cells.empty()) {
        // Snake filled the grid — you win!
        g_game.game_over = true;
        spdlog::info("[SnakeGame] Snake filled the grid — you win!");

        if (g_game.score > g_game.high_score) {
            g_game.high_score = g_game.score;
            save_high_score();
        }

        if (g_gameover_label) {
            char buf[96];
            snprintf(buf, sizeof(buf), "YOU WIN!\nScore: %d\nTap to play again", g_game.score);
            lv_label_set_text(g_gameover_label, buf);
            lv_obj_remove_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // O(1) random pick from free cells
    int idx = rand() % static_cast<int>(g_grid.free_cells.size());
    g_game.food = g_grid.free_cells[idx];
    g_game.food_color = random_food_color();
}

void update_score_label() {
    if (!g_score_label) return;
    char buf[48];
    if (g_game.high_score > 0) {
        snprintf(buf, sizeof(buf), "Score: %d  |  Best: %d", g_game.score, g_game.high_score);
    } else {
        snprintf(buf, sizeof(buf), "Score: %d", g_game.score);
    }
    lv_label_set_text(g_score_label, buf);
}

void show_game_over() {
    bool new_high = g_game.score > g_game.high_score && g_game.score > 0;
    if (new_high) {
        g_game.high_score = g_game.score;
        save_high_score();
    }

    spdlog::info("[SnakeGame] Game over! Score: {} | Best: {}{}", g_game.score, g_game.high_score,
                 new_high ? " (NEW!)" : "");

    // Start death animation
    g_render.death_start_ms = now_ms();
    g_render.death_animating = true;
    g_render.death_input_ready = false;

    // Show game over label after card delay
    if (g_gameover_label) {
        char buf[96];
        if (new_high) {
            snprintf(buf, sizeof(buf), "NEW HIGH SCORE!\n%d\nTap to play again", g_game.score);
        } else {
            snprintf(buf, sizeof(buf), "Game Over!\nScore: %d\nTap to restart", g_game.score);
        }
        lv_label_set_text(g_gameover_label, buf);
        lv_obj_remove_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN);
    }

    update_score_label();
}

void init_game() {
    // Reset game state
    g_game.snake.clear();
    g_game.prev_snake.clear();
    g_game.direction = Direction::RIGHT;
    g_game.prev_direction = Direction::RIGHT;
    g_game.game_over = false;
    g_game.game_started = true;
    g_game.score = 0;
    g_game.tick_ms = INITIAL_TICK_MS;
    g_game.speed_tier = 0;
    g_game.snake_color = random_filament_color();

    // Reset render state
    g_render.interp = 0.0f;
    g_render.tick_accumulator = 0;
    g_render.last_render_ms = now_ms();
    g_render.food_pulse_phase = 0.0f;
    g_render.squash_active = false;
    g_render.death_animating = false;
    g_render.death_input_ready = false;
    for (auto& p : g_render.particles) p.active = false;

    // Reset input
    g_input.queue_size = 0;
    g_input.swipe_handled = false;

    // Start snake in center, 3 segments
    int start_x = g_grid.cols / 2;
    int start_y = g_grid.rows / 2;
    for (int i = 2; i >= 0; i--) {
        g_game.snake.push_back({start_x - i, start_y});
    }

    // Build free cell list
    g_grid.rebuild_free_cells(g_game.snake);

    place_food();
    update_score_label();

    if (g_gameover_label) {
        lv_obj_add_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN);
    }
}
```

- [ ] **Step 2: Build and verify**

Run: `make -j 2>&1 | tail -10`
Expected: Compiles.

- [ ] **Step 3: Commit**

```bash
git add src/ui/ui_snake_game.cpp
git commit -m "feat(snake): O(1) food placement, death animation trigger, game init"
```

---

### Task 7: Overlay Lifecycle — Create and Destroy

Wire up the overlay creation/destruction with the new render timer, D-pad, and input mode detection.

**Files:**
- Modify: `src/ui/ui_snake_game.cpp` (overlay lifecycle + public API)

- [ ] **Step 1: Write create_overlay**

```cpp
void create_overlay() {
    if (g_overlay) {
        spdlog::warn("[SnakeGame] Overlay already exists");
        return;
    }

    spdlog::info("[SnakeGame] Launching snake game!");

    load_high_score();
    srand(static_cast<unsigned>(time(nullptr)));

    // Detect input mode
    g_input.mode = detect_input_mode();
    spdlog::debug("[SnakeGame] Input mode: {}", g_input.mode == InputMode::DPAD ? "DPAD" : "SWIPE");

    // Create full-screen backdrop on top layer
    lv_obj_t* parent = lv_layer_top();
    g_overlay = lv_obj_create(parent);
    lv_obj_set_size(g_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(g_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_overlay, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(g_overlay, 4, LV_PART_MAIN);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Header row (score + close button)
    lv_obj_t* header = lv_obj_create(g_overlay);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    g_score_label = lv_label_create(header);
    lv_obj_set_style_text_color(g_score_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_score_label, theme_manager_get_font("font_heading"), LV_PART_MAIN);
    lv_label_set_text(g_score_label, "Score: 0");

    g_close_btn = lv_button_create(header);
    lv_obj_set_size(g_close_btn, 36, 36);
    lv_obj_set_style_bg_color(g_close_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_close_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_close_btn, 18, LV_PART_MAIN);
    lv_obj_add_event_cb(g_close_btn, close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* close_label = lv_label_create(g_close_btn);
    lv_label_set_text(close_label, "X");
    lv_obj_set_style_text_color(close_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_label, theme_manager_get_font("font_heading"), LV_PART_MAIN);
    lv_obj_center(close_label);

    // Game area
    g_game_area = lv_obj_create(g_overlay);
    lv_obj_set_style_bg_opa(g_game_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_game_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_game_area, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(g_game_area, 1);
    lv_obj_set_width(g_game_area, LV_PCT(100));
    lv_obj_remove_flag(g_game_area, LV_OBJ_FLAG_SCROLLABLE);

    // Calculate grid
    lv_coord_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screen_h = lv_display_get_vertical_resolution(nullptr);
    lv_coord_t avail_w = screen_w - 24;
    lv_coord_t avail_h = screen_h - 64;

    g_grid.cols = avail_w / CELL_SIZE;
    g_grid.rows = avail_h / CELL_SIZE;
    g_grid.offset_x = (avail_w - g_grid.cols * CELL_SIZE) / 2;
    g_grid.offset_y = (avail_h - g_grid.rows * CELL_SIZE) / 2;

    spdlog::debug("[SnakeGame] Grid: {}x{} cells, offset: ({}, {})", g_grid.cols, g_grid.rows,
                  g_grid.offset_x, g_grid.offset_y);

    // Draw callback
    lv_obj_add_event_cb(g_game_area, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    // Touch input
    lv_obj_add_flag(g_game_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_game_area, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(g_game_area, touch_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g_game_area, touch_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(g_game_area, touch_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(g_game_area, input_cb, LV_EVENT_KEY, nullptr);

    // Keyboard focus
    lv_group_t* group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, g_game_area);
        lv_group_focus_obj(g_game_area);
    }

    // D-pad overlay (only in DPAD mode)
    if (g_input.mode == InputMode::DPAD) {
        create_dpad(g_game_area);
    }

    // Game over label (floating, hidden initially)
    g_gameover_label = lv_label_create(g_overlay);
    lv_obj_set_style_text_color(g_gameover_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_gameover_label, theme_manager_get_font("font_heading"), LV_PART_MAIN);
    lv_obj_set_style_text_align(g_gameover_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(g_gameover_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(g_gameover_label, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_FLOATING);

    lv_obj_move_foreground(g_overlay);

    // Initialize game
    init_game();

    // Start render timer (~60fps)
    g_render_timer = lv_timer_create(render_tick, RENDER_TICK_MS, nullptr);

    spdlog::info("[SnakeGame] Game started! Grid: {}x{}, input: {}", g_grid.cols, g_grid.rows,
                 g_input.mode == InputMode::DPAD ? "DPAD" : "SWIPE");
}
```

- [ ] **Step 2: Write destroy_overlay**

```cpp
void destroy_overlay() {
    if (g_render_timer) {
        lv_timer_delete(g_render_timer);
        g_render_timer = nullptr;
    }

    if (g_game_area) {
        lv_group_t* group = lv_group_get_default();
        if (group) {
            lv_group_remove_obj(g_game_area);
        }
    }

    if (helix::ui::safe_delete(g_overlay)) {
        g_game_area = nullptr;
        g_score_label = nullptr;
        g_gameover_label = nullptr;
        g_close_btn = nullptr;
        g_dpad_up = nullptr;
        g_dpad_down = nullptr;
        g_dpad_left = nullptr;
        g_dpad_right = nullptr;
    }

    g_game.snake.clear();
    g_game.prev_snake.clear();
    g_game.game_started = false;
    g_game.game_over = false;
    g_input.swipe_handled = false;
    g_input.queue_size = 0;
    g_grid.free_cells.clear();

    spdlog::info("[SnakeGame] Game closed");
}

} // anonymous namespace

// ============================================================================
// PUBLIC API
// ============================================================================

void SnakeGame::show() {
    if (g_overlay) {
        spdlog::debug("[SnakeGame] Already visible");
        return;
    }
    create_overlay();
}

void SnakeGame::hide() {
    destroy_overlay();
}

bool SnakeGame::is_visible() {
    return g_overlay != nullptr;
}

} // namespace helix
```

- [ ] **Step 3: Build and verify full compilation**

Run: `make -j 2>&1 | tail -20`
Expected: Clean compile. No errors.

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_snake_game.cpp
git commit -m "feat(snake): wire up overlay lifecycle with render timer and D-pad"
```

---

### Task 8: Integration Test — Manual Gameplay Verification

Launch the game in test mode and verify all features work.

**Files:** No code changes — this is a manual test task.

- [ ] **Step 1: Launch the app in test mode**

```bash
./build/bin/helix-screen --test -vv -p about_settings_overlay 2>&1 | tee /tmp/snake-test.log
```

Launch with `run_in_background: true`.

- [ ] **Step 2: User tests gameplay**

Ask the user to:
1. Tap the Printer Name row 7 times to trigger the easter egg
2. Verify smooth snake movement (interpolated, not jerky)
3. Test swipe controls — direction should change within ~16ms of swipe
4. Eat food and verify: particle burst, food pulse animation, score updates
5. Hit speed tier 2+ and verify border color changes
6. Die and verify: white flash, red shrink animation, score card, tap to restart
7. Verify high score persists across game restarts
8. Press ESC or X button to close the game
9. If running SDL: verify D-pad overlay appears and works

- [ ] **Step 3: Read test log and check for errors**

Read `/tmp/snake-test.log` and look for:
- `[SnakeGame]` log lines confirming launch, score, and close
- No spdlog errors or LVGL warnings
- No crashes or segfaults

- [ ] **Step 4: Commit final state**

```bash
git add src/ui/ui_snake_game.cpp
git commit -m "feat(snake): complete rebuild — smooth animation, adaptive input, visual effects

Rebuilt the snake game easter egg with:
- Fixed-timestep game loop with interpolated rendering (~60fps)
- Adaptive input: D-pad overlay on SDL/resistive, improved swipe on capacitive
- 2-deep input queue for responsive direction buffering
- O(1) food placement via free cell tracking
- Visual effects: food pulse, eat particles, head squash, tail taper
- Speed tier progression with border color shifts
- Death animation: white flash, red shrink, score card
- Instant restart on tap after 600ms cooldown"
```
