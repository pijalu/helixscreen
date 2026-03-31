# Snake Game Rebuild — Design Spec

**Date:** 2026-03-31
**Status:** Approved
**Scope:** Rebuild the snake game easter egg in the about panel for smoother animation and better input responsiveness.

## Overview

The snake game easter egg (activated by 7 taps on Printer Name in the about panel) gets a full gameplay rebuild. The core problems are jerky movement (fixed-interval grid teleportation) and unresponsive swipe controls (direction changes delayed until next game tick, unreliable PRESSING events on some hardware). The visual theme (3D filament tubes, directional eyes, spool box food) is good and stays.

## Game Loop Architecture

**Fixed timestep with interpolated rendering.**

An LVGL timer fires at ~16ms (render tick). Each render tick:

1. Accumulates elapsed time into `tick_accumulator`
2. While `tick_accumulator >= game_tick_interval`: run one game logic step (move snake, check collisions, consume input from queue). Subtract `game_tick_interval` from accumulator.
3. Calculate `interpolation_factor = tick_accumulator / game_tick_interval` (0.0 to 1.0)
4. Render snake at interpolated positions between previous and current grid positions
5. Update particle and animation state

Game tick interval starts at 150ms and decreases to 70ms minimum. Collision and game logic remain grid-based — only visuals interpolate. This keeps the game deterministic.

## Input System

### Adaptive Input

Input mode is selected at game start based on the runtime environment:

- **SDL** → D-pad mode
- **Resistive touchscreen (e.g. AD5M)** → D-pad mode
- **Capacitive touchscreen (e.g. K1C, Pi DSI)** → Swipe mode
- **Keyboard** → Always available as fallback in both modes

Detection: check `RuntimeConfig` for SDL, or query `lv_indev_get_type()` / device capability config.

### Swipe Mode (Capacitive)

- `LV_EVENT_PRESSING` with 12px threshold (reduced from 20px)
- Direction change applied immediately on detection, pushed into input queue
- `LV_EVENT_RELEASED` fallback for devices where PRESSING is flaky
- After swipe detection, reset touch origin to current position so the player can chain swipes without lifting their finger
- 180-degree reversal blocked

### D-pad Mode (Resistive / SDL)

- Semi-transparent 4-button directional overlay rendered over the bottom portion of the game area
- ~30% opacity idle, ~50% on press
- Each tap pushes direction into the same input queue
- Brief press animation (scale/color change) for feedback

### Keyboard

- Arrow keys → direction input queue
- ESC → close game
- Any key on game over → restart

### 2-Deep Input Queue

All input modes feed into a 2-deep direction queue consumed one-per-game-tick. This lets the player buffer one turn ahead (e.g., swipe down then quickly left — "down" applies next tick, "left" applies the tick after). More than 2 deep would feel laggy. The queue is sampled every render frame (~16ms) so input capture latency is minimal.

## Visual System

### Rendering Pipeline (draw order)

1. **Background** — dark fill, subtle grid lines at cell boundaries
2. **Border** — rounded rect, color shifts with speed tier
3. **Food** — pulsing spool box (cycles size ±2px at 2Hz)
4. **Snake body** — 3D filament tubes at interpolated positions
5. **Snake head** — wider tube + directional eyes, squash/stretch on direction change
6. **Particles** — burst on food eat (6-8 small circles, fade over 300ms)
7. **HUD** — score label top-left, high score top-right, D-pad overlay (if active)

### Kept From Current

- 3D filament tube rendering (shadow/body/highlight three-layer technique)
- Directional eyes on the head (white circles with black pupils)
- Random snake color per game (10 filament colors)
- Spool box food rendering (6 food colors)
- `draw_tube_segment()` / `draw_flat_line()` helpers

### New Visual Effects

**Food pulse:** Food gently oscillates in rendered size (±2px at 2Hz) so it feels alive. Pure render effect, no collision box change.

**Eat particles:** When food is eaten, 6-8 small circles burst outward from the food position with random velocities. Color matches the eaten food. Fade to 0 opacity over 300ms. Stored as a small particle array, updated each render frame, expired particles recycled.

**Head squash/stretch:** On direction change, the head squashes (15% wider, 15% shorter along the new axis) for ~100ms, then springs back. Gives a sense of weight and momentum.

**Tail tapering:** Last 3 body segments taper in width and opacity. The tail tip fades smoothly during interpolation rather than popping in/out.

**Speed tier progression:** Every 5 food items, the speed tier advances. The border color and background tint subtly shift through a progression: neutral → green → yellow → orange → red. Subconscious tension building, not in-your-face.

## Scoring & Progression

- **Score** = food count (same as current)
- **High score** persisted to same config key (`/display/frame_counter`)
- **Speed tiers:** every 5 food, tick interval decreases by 10ms (150 → 140 → 130 → ... → 70)
- **Win condition:** snake fills entire grid (`free_cells` empty). Displays "YOU WIN!" message.

## Game Over

1. **0ms** — white flash overlay at 20% opacity
2. **0-200ms** — snake turns red, segments shrink from tail toward head
3. **300ms** — score card fades in (score, high score, "NEW HIGH SCORE!" if applicable, "Tap to play")
4. **600ms** — input accepted, tap anywhere to restart instantly

Total death-to-playing-again time: under 1 second if the player taps immediately at 600ms.

## Wall Behavior

Classic wall death. Hitting any edge ends the game. No wrap-around.

## Food Placement

Replace random-retry with a free cell list:

- Maintain `std::vector<GridPos> free_cells` alongside the snake
- When food is needed: pick random index from `free_cells` — O(1)
- When snake moves: remove new head position from free cells, add vacated tail position back
- Win detection: `free_cells.empty()`

Eliminates the O(n²) stutter when the snake is long.

## State Management

Organized structs in anonymous namespace (not classes — this is an easter egg, not a library):

- **GameState** — snake deque, direction, score, game_over flag, speed tier, tick interval, game timer
- **RenderState** — interpolation factor, particle array, death animation progress, squash timer, food pulse phase
- **InputState** — 2-deep direction queue, touch origin, swipe handled flag, input mode enum (dpad/swipe)
- **GridState** — dimensions, offsets, cell size, free_cells vector

All game logic remains as free functions operating on these structs.

## Integration

- **No changes to the about panel.** `SnakeGame::show()` and `SnakeGame::hide()` keep the same signatures.
- 7-tap easter egg activation unchanged
- High score persistence unchanged (same config key)
- File: `src/ui/ui_snake_game.cpp` (rewrite in-place), `include/ui_snake_game.h` (minimal changes if any)

## Out of Scope

- Levels or obstacles
- Bonus food items or combo multipliers
- Wrap-around mode
- Multiplayer
- Sound effects (could be added later but not in this rebuild)
- Leaderboards
