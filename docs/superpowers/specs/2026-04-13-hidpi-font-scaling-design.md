# HiDPI Font Scaling & Per-Platform Font Pruning

**Issue:** prestonbrown/helixscreen#773
**Date:** 2026-04-13
**Status:** Draft

## Problem

The XLARGE breakpoint (height >700px) serves displays from 720p to 1440p+. On a 5.5"
QHD panel (2560x1440, ~534 PPI), the current XLARGE font sizes (e.g., 24px body, 28px
heading) produce ~0.9mm tall characters — unreadable at normal operating distance.
Icons flatline at the same sizes as LARGE.

Meanwhile, all platforms ship every font size regardless of their fixed display
resolution, wasting binary size and resident memory on constrained devices.

## Solution

Two coordinated changes:

1. **Split XLARGE into XLARGE + XXLARGE** — XLARGE covers 701-1000px height (720p-ish),
   XXLARGE covers >1000px (1440p+). Both get properly scaled values instead of the
   current plateau at LARGE sizes.

2. **Per-platform font pruning** — each platform declares which breakpoint tiers it
   needs. The build system compiles only those font assets. Fixed-display platforms
   ship their tier + one tier up. Dynamic platforms (Pi, x86) ship all tiers.

## Breakpoint Tiers (updated)

| Tier | Height range | Typical hardware |
|------|-------------|-----------------|
| MICRO | ≤272 | CC1 480x272 |
| TINY | 273-390 | Snapmaker U1 480x320 |
| SMALL | 391-460 | K1 480x400 |
| MEDIUM | 461-550 | AD5M/AD5X 800x480 |
| LARGE | 551-700 | K2 480x800, 1024x600 SBCs |
| XLARGE | 701-1000 | 1280x720, 1024x768 |
| XXLARGE | >1000 | 1440p, 4K |

New constant in `ui_breakpoint.h`: `UI_BREAKPOINT_XLARGE_MAX = 1000`.

## Font Values

### Text fonts

| Semantic | MICRO | TINY | SMALL | MED | LARGE | XLARGE | XXLARGE |
|----------|-------|------|-------|-----|-------|--------|---------|
| heading | 14 | 16 | 20 | 26 | 28 | 32 | 40 |
| xl (bold) | bold_16 | bold_18 | bold_20 | bold_28 | bold_28 | bold_32 | bold_40 |
| body | 10 | 12 | 14 | 18 | 20 | 24 | 32 |
| small (light) | light_10 | light_11 | light_12 | light_16 | light_18 | light_20 | light_26 |
| xs (light) | light_10 | light_10 | light_10 | light_12 | light_14 | light_16 | light_20 |
| mono | 8 | 10 | 12 | 14 | 16 | 18 | 24 |

### Icon fonts

| Semantic | MICRO | TINY | SMALL | MED | LARGE | XLARGE | XXLARGE |
|----------|-------|------|-------|-----|-------|--------|---------|
| xs | 16 | 16 | 16 | 16 | 16 | 24 | 32 |
| sm | 16 | 24 | 24 | 24 | 24 | 32 | 48 |
| md | 24 | 32 | 32 | 32 | 32 | 48 | 64 |
| lg | 24 | 32 | 48 | 48 | 48 | 64 | 96 |
| xl | 32 | 48 | 64 | 64 | 64 | 80 | 128 |

### icon_xl as % of screen height (validation)

| Tier | Typical height | icon_xl | % |
|------|---------------|---------|---|
| MICRO | 272 | 32 | 11.8% |
| TINY | 320 | 48 | 15.0% |
| SMALL | 400 | 64 | 16.0% |
| MEDIUM | 480 | 64 | 13.3% |
| LARGE | 600 | 64 | 10.7% |
| XLARGE | 720 | 80 | 11.1% |
| XXLARGE | 1440 | 128 | 8.9% |

## Spacing Values

### Core spacing

| Token | TINY | SMALL | MED | LARGE | XLARGE | XXLARGE |
|-------|------|-------|-----|-------|--------|---------|
| space_xxs | 1 | 2 | 3 | 4 | 5 | 6 |
| space_xs | 2 | 4 | 5 | 6 | 8 | 10 |
| space_sm | 4 | 6 | 7 | 8 | 10 | 12 |
| space_md | 6 | 8 | 10 | 12 | 16 | 20 |
| space_lg | 8 | 12 | 16 | 20 | 24 | 32 |
| space_xl | 8 | 16 | 20 | 24 | 32 | 40 |
| space_2xl | — | 24 | 32 | 40 | 48 | 64 |
| space_2xl_neg | — | -24 | -32 | -40 | -48 | -64 |

### Component sizes

| Token | MICRO | TINY | SMALL | MED | LARGE | XLARGE | XXLARGE |
|-------|-------|------|-------|-----|-------|--------|---------|
| button_height_sm | 24 | 28 | 40 | 40 | 40 | 48 | 56 |
| button_height | 26 | 32 | 48 | 52 | 72 | 80 | 96 |
| button_height_lg | 34 | 40 | 64 | 70 | 96 | 112 | 128 |
| header_height | 28 | 32 | 48 | 56 | 60 | 68 | 80 |
| temp_card_height | 42 | 48 | 64 | 72 | 80 | 96 | 112 |
| badge_size | — | 10 | 16 | 18 | 20 | 24 | 28 |
| icon_size | md | md | md | lg | xl | xl | xl |
| border_radius | — | — | 4 | 9 | 12 | 14 | 16 |

### Spinners

| Token | SMALL | MED | LARGE | XLARGE | XXLARGE |
|-------|-------|-----|-------|--------|---------|
| spinner_lg | 48 | 56 | 64 | 80 | 96 |
| spinner_md | 24 | 28 | 32 | 40 | 48 |
| spinner_sm | 16 | 18 | 20 | 24 | 28 |
| spinner_xs | 12 | 14 | 16 | 18 | 20 |
| spinner_arc_lg | 3 | 4 | 4 | 5 | 6 |
| spinner_arc_md | 2 | 3 | 3 | 4 | 4 |

### AMS

| Token | SMALL | MED | LARGE | XLARGE | XXLARGE |
|-------|-------|-----|-------|--------|---------|
| ams_logo_size | 20 | 24 | 28 | 32 | 40 |
| ams_bars_height | 36 | 48 | 56 | 68 | 80 |
| ams_card_min_width | 80 | 100 | 120 | 140 | 180 |
| ams_card_max_width | 160 | 200 | 240 | 280 | 360 |

## New Font Assets to Generate

### Text fonts (`.c` files via `lv_font_conv`)

- noto_sans_32.c, noto_sans_40.c
- noto_sans_bold_32.c, noto_sans_bold_40.c
- noto_sans_light_20.c, noto_sans_light_26.c
- source_code_pro_18.c, source_code_pro_20.c, source_code_pro_24.c

### Icon fonts (`.c` files via `lv_font_conv`)

- mdi_icons_20.c, mdi_icons_28.c, mdi_icons_40.c
- mdi_icons_56.c, mdi_icons_80.c, mdi_icons_96.c, mdi_icons_128.c

### CJK fallback fonts (`.bin` files)

New sizes needed for CJK fallback on XLARGE/XXLARGE: noto_sans_cjk_32.bin,
noto_sans_cjk_40.bin, noto_sans_cjk_bold_32.bin, noto_sans_cjk_bold_40.bin,
noto_sans_cjk_light_20.bin, noto_sans_cjk_light_26.bin.

## Per-Platform Font Pruning

### Platform tier assignments

Each platform in `cross.mk` declares `FONT_TIERS` — its own tier plus one tier up.

| Platform | Fixed resolution | FONT_TIERS |
|----------|-----------------|------------|
| cc1 | 480x272 | micro tiny |
| snapmaker-u1 | 480x320 | tiny small |
| k1/mips | 480x400 | small medium |
| ad5m | 800x480 | medium large |
| ad5x | 800x480 | medium large |
| k2 | 480x800 | large xlarge |
| pi, pi-fbdev, pi32 | auto-detect | all |
| x86, x86-fbdev | auto-detect | all |
| native | variable | all |

### Build system mechanism

`mk/fonts.mk` defines font file lists per tier. Each tier list contains exactly the
text fonts and icon fonts referenced by that tier's `globals.xml` constants:

```makefile
FONTS_MICRO := noto_sans_10 noto_sans_14 noto_sans_bold_16 noto_sans_light_10 \
               source_code_pro_8 mdi_icons_16 mdi_icons_24 mdi_icons_32
FONTS_TINY  := noto_sans_12 noto_sans_16 noto_sans_bold_18 noto_sans_light_11 \
               source_code_pro_10 mdi_icons_24 mdi_icons_32 mdi_icons_48
# ... etc for each tier
```

`FONT_SRCS` is assembled as the union of all declared tiers (deduplicating shared
fonts). Platforms with `FONT_TIERS := all` get every font.

### Theme registration — smart fallback with warnings

The build system embeds a compile-time constant (e.g., `HELIX_MAX_FONT_TIER`)
indicating the highest tier compiled into the binary. The theme registration uses
this to distinguish expected vs. unexpected missing fonts:

- **Font missing but above the compiled tier** → silent fallback. AD5M ships
  `medium large` — if `noto_sans_32` (XLARGE) isn't found, fall back silently.
  This is intentional pruning.
- **Font missing within the compiled tier range** → warn. If AD5M can't find
  `noto_sans_18` (MEDIUM), that's a build bug — log a warning.
- **No tier resolves at all** → error. Something is fundamentally broken.

The fallback chain: `_xxlarge` → `_xlarge` → `_large` → `_medium` → `_small` →
`_tiny` → `_micro`.

## Files Changed

| File | Change |
|------|--------|
| `ui_xml/globals.xml` | Fix XLARGE values, add XXLARGE variants for all responsive constants |
| `include/ui_breakpoint.h` | Add `UI_BREAKPOINT_XLARGE_MAX = 1000`, XXLARGE enum/suffix |
| `src/ui/theme_manager.cpp` | Handle `_xxlarge` suffix, silent fallback for missing fonts |
| `mk/cross.mk` | Each platform declares `FONT_TIERS` |
| `mk/fonts.mk` | Per-tier font lists, assemble `FONT_SRCS` from declared tiers |
| `scripts/regen_mdi_fonts.sh` | Add 20/28/40/56/80/96/128 icon sizes |
| `scripts/regen_text_fonts.sh` | Add new text font sizes, CJK `.bin` equivalents |
| `assets/fonts/` | New `.c` font files |
| `include/ui_fonts.h` | Extern declarations for new fonts (conditional on tier) |
| `src/system/cjk_font_manager.cpp` | Add mappings for new font sizes |

## Out of Scope

- Runtime font scale multiplier (user-facing setting) — revisit if XXLARGE proves
  insufficient for extreme DPI panels
- LayoutManager changes — already uses post-rotation resolution, no fix needed
- `.bin` loading for all fonts — investigated, savings (~2MB) not worth the refactor
