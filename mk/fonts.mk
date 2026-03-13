# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Font & Icon Generation Module
# Handles font generation and Material Design icons

# Generate MDI icon fonts using the authoritative regen script
# Triggered when regen_mdi_fonts.sh changes (single source of truth for icon codepoints)
.fonts.stamp: scripts/regen_mdi_fonts.sh
	$(ECHO) "$(CYAN)Checking font generation...$(RESET)"
	$(Q)if ! command -v lv_font_conv >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ lv_font_conv not found - skipping font generation$(RESET)"; \
		echo "$(YELLOW)  Run 'npm install' to enable font regeneration$(RESET)"; \
		touch $@; \
	else \
		echo "$(YELLOW)→ Regenerating MDI icon fonts from regen_mdi_fonts.sh...$(RESET)"; \
		./scripts/regen_mdi_fonts.sh && touch $@ && echo "$(GREEN)✓ Fonts regenerated successfully$(RESET)"; \
	fi

# Fonts depend on stamp file to ensure they're regenerated when needed
$(FONT_SRCS): .fonts.stamp

generate-fonts: .fonts.stamp

# Validate that all icons in ui_icon_codepoints.h are present in compiled fonts
# This prevents the bug where icons are added to code but fonts aren't regenerated
validate-fonts:
	$(ECHO) "$(CYAN)Validating icon font codepoints...$(RESET)"
	$(Q)if [ -f scripts/validate_icon_fonts.sh ]; then \
		if ./scripts/validate_icon_fonts.sh; then \
			echo "$(GREEN)✓ All icon codepoints present in fonts$(RESET)"; \
		else \
			echo "$(RED)✗ Missing icon codepoints - run 'make regen-fonts' to fix$(RESET)"; \
			exit 1; \
		fi; \
	else \
		echo "$(YELLOW)⚠ validate_icon_fonts.sh not found - skipping$(RESET)"; \
	fi

# Regenerate MDI icon fonts from scratch using the regen script
# Use this when adding new icons to include/ui_icon_codepoints.h
regen-fonts:
	$(ECHO) "$(CYAN)Regenerating MDI icon fonts...$(RESET)"
	$(Q)if [ -f scripts/regen_mdi_fonts.sh ]; then \
		./scripts/regen_mdi_fonts.sh; \
		echo "$(GREEN)✓ Fonts regenerated - rebuild required$(RESET)"; \
	else \
		echo "$(RED)✗ regen_mdi_fonts.sh not found$(RESET)"; \
		exit 1; \
	fi

# Regenerate Noto Sans text fonts with extended Unicode (Latin, Cyrillic, CJK)
# Use this for internationalization support
regen-text-fonts:
	$(ECHO) "$(CYAN)Regenerating Noto Sans text fonts...$(RESET)"
	$(Q)if [ -f scripts/regen_text_fonts.sh ]; then \
		./scripts/regen_text_fonts.sh; \
		echo "$(GREEN)✓ Text fonts regenerated - rebuild required$(RESET)"; \
	else \
		echo "$(RED)✗ regen_text_fonts.sh not found$(RESET)"; \
		exit 1; \
	fi

# Auto-regenerate text fonts when CJK translation files change
# Only triggers if CJK source fonts are present (they're gitignored)
CJK_FONT_SC := assets/fonts/NotoSansCJKsc-Regular.otf
CJK_FONT_JP := assets/fonts/NotoSansCJKjp-Regular.otf
CJK_TRANS_YAML := translations/zh.yml translations/ja.yml
TEXT_FONT_STAMP := .text-fonts.stamp

# Check if any CJK translation YAML is newer than the text font stamp
$(TEXT_FONT_STAMP): $(CJK_TRANS_YAML) scripts/regen_text_fonts.sh
	$(Q)if command -v lv_font_conv >/dev/null 2>&1; then \
		echo "$(YELLOW)→ CJK translations changed - regenerating text fonts (will auto-download CJK fonts if needed)...$(RESET)"; \
		./scripts/regen_text_fonts.sh && touch $@ && echo "$(GREEN)✓ Text fonts regenerated with CJK$(RESET)"; \
	else \
		echo "$(YELLOW)⚠ lv_font_conv not found - skipping CJK font regeneration$(RESET)"; \
		touch $@; \
	fi

# Regenerate icon constants in globals.xml from ui_icon_codepoints.h
# Single source of truth: C++ header -> XML constants
regen-icon-consts:
	$(ECHO) "$(CYAN)Regenerating icon constants in globals.xml...$(RESET)"
	$(Q)python3 scripts/gen_icon_consts.py
	$(ECHO) "$(GREEN)✓ Icon constants regenerated$(RESET)"

# Update MDI icon metadata cache from Pictogrammers GitHub
# Run periodically when MDI library updates, or when adding new icons
update-mdi-cache:
	$(ECHO) "$(CYAN)Updating MDI metadata cache...$(RESET)"
	$(Q)curl -sL "https://raw.githubusercontent.com/Templarian/MaterialDesign/master/meta.json" | gzip > assets/mdi-icon-metadata.json.gz
	$(ECHO) "$(GREEN)✓ Updated assets/mdi-icon-metadata.json.gz$(RESET)"
	@ls -lh assets/mdi-icon-metadata.json.gz | awk '{print "$(CYAN)  Size: " $$5 "$(RESET)"}'

# Verify MDI codepoint labels match official metadata
verify-mdi-codepoints:
	$(ECHO) "$(CYAN)Verifying MDI codepoint labels...$(RESET)"
	$(Q)python3 scripts/verify_mdi_codepoints.py

# Generate macOS .icns icon from source logo
# Requires: ImageMagick (magick) for image processing
# Source: assets/images/helixscreen-logo.png
# Output: assets/images/helix-icon.icns (macOS), assets/images/helix-icon.png (Linux)
icon:
ifeq ($(UNAME_S),Darwin)
	$(ECHO) "$(CYAN)Generating macOS icon from logo...$(RESET)"
	@if ! command -v magick >/dev/null 2>&1; then \
		echo "$(RED)✗ ImageMagick (magick) not found$(RESET)"; \
		echo "$(YELLOW)Install with: brew install imagemagick$(RESET)"; \
		exit 1; \
	fi
	@if ! command -v iconutil >/dev/null 2>&1; then \
		echo "$(RED)✗ iconutil not found (should be built-in on macOS)$(RESET)"; \
		exit 1; \
	fi
else
	$(ECHO) "$(CYAN)Generating icon from logo (Linux - PNG only)...$(RESET)"
	@if ! command -v magick >/dev/null 2>&1; then \
		echo "$(RED)✗ ImageMagick (magick) not found$(RESET)"; \
		echo "$(YELLOW)Install with: sudo apt install imagemagick$(RESET)"; \
		exit 1; \
	fi
endif
	$(ECHO) "$(CYAN)  [1/6] Cropping logo to circular icon...$(RESET)"
	$(Q)magick assets/images/helixscreen-logo.png \
		-crop 700x580+162+100 +repage \
		-gravity center -background none -extent 680x680 \
		assets/images/helix-icon.png
	$(ECHO) "$(CYAN)  [2/6] Generating 128x128 icon for window...$(RESET)"
	$(Q)magick assets/images/helix-icon.png -resize 128x128 assets/images/helix-icon-128.png
	$(ECHO) "$(CYAN)  [3/6] Generating C header file for embedded icon...$(RESET)"
	$(Q)python3 scripts/generate_icon_header.py assets/images/helix-icon-128.png include/helix_icon_data.h
ifeq ($(UNAME_S),Darwin)
	$(ECHO) "$(CYAN)  [4/6] Generating icon sizes (16px to 1024px)...$(RESET)"
	$(Q)mkdir -p assets/images/icon.iconset
	$(Q)for size in 16 32 64 128 256 512; do \
		magick assets/images/helix-icon.png -resize $${size}x$${size} \
			assets/images/icon.iconset/icon_$${size}x$${size}.png; \
		magick assets/images/helix-icon.png -resize $$((size*2))x$$((size*2)) \
			assets/images/icon.iconset/icon_$${size}x$${size}@2x.png; \
	done
	$(ECHO) "$(CYAN)  [5/6] Creating .icns bundle...$(RESET)"
	$(Q)iconutil -c icns assets/images/icon.iconset -o assets/images/helix-icon.icns
	$(ECHO) "$(CYAN)  [6/6] Cleaning up temporary files...$(RESET)"
	$(Q)rm -rf assets/images/icon.iconset
	$(ECHO) "$(GREEN)✓ Icon generated: assets/images/helix-icon.icns + helix-icon-128.png + header$(RESET)"
	@ls -lh assets/images/helix-icon.icns assets/images/helix-icon-128.png include/helix_icon_data.h | awk '{print "$(CYAN)  " $$9 ": " $$5 "$(RESET)"}'
else
	$(ECHO) "$(CYAN)  [4/4] Icon generated (PNG format)...$(RESET)"
	$(ECHO) "$(GREEN)✓ Icon generated: assets/images/helix-icon.png + helix-icon-128.png + header$(RESET)"
	@ls -lh assets/images/helix-icon.png assets/images/helix-icon-128.png include/helix_icon_data.h | awk '{print "$(CYAN)  " $$9 ": " $$5 "$(RESET)"}'
	$(ECHO) "$(YELLOW)Note: .icns format requires macOS. PNG icons can be used for Linux apps.$(RESET)"
endif
