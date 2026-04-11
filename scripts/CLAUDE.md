# scripts/

Build, asset generation, deployment, and developer tooling for HelixScreen.

## Categories

### Installation & Deployment
| Script | Purpose |
|--------|---------|
| `install.sh` | **Auto-generated** single-file installer for end users (`curl\|sh`). Do NOT edit directly |
| `install-dev.sh` | Modular dev installer — uses `lib/installer/` modules. Edit this one |
| `uninstall.sh` | **Auto-generated** single-file uninstaller. Do NOT edit directly |
| `bundle-installer.sh` | Bundles `install-dev.sh` + `lib/installer/*` → `install.sh` |
| `bundle-uninstaller.sh` | Bundles uninstall modules → `uninstall.sh` |
| `helix-launcher.sh` | Systemd-launched watchdog wrapper. Sources `helixscreen.env` for runtime config |
| `check-deps.sh` | Validates build dependencies. `--minimal` for cross-compile environments |

### Release & Packaging
| Script | Purpose |
|--------|---------|
| `package.sh` | Creates release archives for all platforms |
| `dev-release.sh` | Local dev release workflow (build + package + upload) |
| `generate-manifest.sh` | Generates `manifest.json` from release archives. Used by CI and dev-release |

### Asset Generation (`make regen-*`)
| Script | Purpose |
|--------|---------|
| `regen_mdi_fonts.sh` | Regenerate MDI icon fonts from codepoints. Run after adding icons |
| `regen_text_fonts.sh` | Regenerate Noto Sans text fonts for LVGL |
| `regen_images.sh` | Pre-render splash screen images to LVGL binary format |
| `regen_placeholder_images.sh` | Generate placeholder/fallback images |
| `regen_printer_images.sh` | Process printer model images for the printer database |
| `trim_printer_images.sh` | Crop/trim whitespace from printer images |
| `gen_splash_3d.py` | Composite 3D logo onto full-screen splash canvases |
| `generate_gradient_bg.py` | Pre-render gradient backgrounds for print file cards (perf optimization) |
| `LVGLImage.py` | Python library for LVGL binary image format conversion |
| `download_printer_images_headless.py` | Scrape printer images from web sources |

### Icon Pipeline
| Script | Purpose |
|--------|---------|
| `gen_icon_consts.py` | `codepoints.h` → `globals.xml` icon string constants |
| `generate_icon_header.py` | Embed PNG icon data into C headers |
| `validate_icon_fonts.sh` | Bidirectional check: codepoints ↔ fonts ↔ XML usage |

### Translation / i18n
| Script | Purpose |
|--------|---------|
| `generate_translations.py` | Main translation generator — YAML → C++ translation tables |
| `translation_sync.py` | Sync translation keys between XML/C++ sources and YAML files |
| `migrate_xml_translations.py` | Migration tool: inline XML text → translation key references |
| `xml_to_yaml_translations.py` | Extract inline XML strings to YAML format |
| `translations/` | Python package: extractor, YAML manager, coverage reporting, CLI |

### Telemetry & Crash Analysis
| Script | Purpose |
|--------|---------|
| `telemetry-pull.sh` | Pull events from telemetry worker API. Needs `HELIX_TELEMETRY_ADMIN_KEY` |
| `telemetry-analyze.py` | Adoption, reliability, crash metrics. Output: terminal/JSON/HTML |
| `telemetry-printer-profiles.py` | Printer detection analysis: model distribution, name clustering, candidate heuristics, DB validation |
| `telemetry-crashes.py` | Resolve ASLR crash addresses → function names, group by signature |
| `telemetry-backfill.sh` | Backfill Analytics Engine from R2 (90-day retention limit) |
| `resolve-backtrace.sh` | Resolve raw backtrace addresses using `.sym` files from R2 |
| `debug-bundle.sh` | Fetch and display debug bundles from `crash.helixscreen.org` |

### Quality & Auditing
| Script | Purpose |
|--------|---------|
| `quality-checks.sh` | Pre-commit and CI quality checks (single source of truth) |
| `audit_codebase.sh` | Check for coding standard violations. `--strict` for CI |
| `format-xml.py` | XML formatter: 2-space indent, attribute wrapping at ~120 chars |
| `verify_mdi_codepoints.py` | Verify MDI codepoint mappings are correct |

### Screenshots & Testing
| Script | Purpose |
|--------|---------|
| `screenshot.sh` | Capture screenshot from running HelixScreen (`./scripts/screenshot.sh helix-screen output [panel]`) |
| `screenshot-all.sh` | Capture screenshots of all panels |
| `ad5m-screenshot.sh` | Remote screenshot capture from AD5M printer |
| `generate-screenshots.sh` | Generate screenshots for documentation/marketing |
| `generate-test-data.py` | Generate mock test data for test suites |
| `test_clean_shutdown.sh` | Verify clean shutdown behavior |
| `afc-test.sh` | AFC live smoke test against a real printer |

### Developer Tools
| Script | Purpose |
|--------|---------|
| `setup-worktree.sh` | Create/configure git worktrees in `.worktrees/`. Symlinks deps, builds fast |
| `git-stats.sh` | Comprehensive repo statistics with effort estimation |
| `benchmark_hosts.sh` | Benchmark host performance for build optimization |
| `benchmark_neon.sh` | NEON SIMD performance benchmarks |
| `add-spdx-headers.sh` | Add SPDX license headers to source files |
| `add-copyright-headers.sh` | Add copyright headers to source files |
| `debug-ad5m-boot.sh` | AD5M boot diagnostics. `--boot` saves to persistent log |

### Subdirectories
| Path | Purpose |
|------|---------|
| `lib/installer/` | Modular installer components sourced by `install-dev.sh` |
| `lib/lvgl_image_lib.sh` | Shared LVGL image conversion helpers |
| `kiauh/` | KIAUH integration (Klipper installer plugin) |
| `translations/` | Python package for translation extraction, sync, and coverage |

## Key Patterns

- **Auto-generated files**: `install.sh` and `uninstall.sh` are bundled from modular sources. Edit the dev versions + `lib/installer/`, then re-bundle
- **Telemetry credentials**: Scripts auto-load from `.env.telemetry` in project root. Set `HELIX_TELEMETRY_ADMIN_KEY` env var
- **Asset regeneration**: Usually invoked via Makefile targets (`make regen-fonts`, `make regen-images`) rather than directly
- **Python deps**: Telemetry scripts need pandas — install from `telemetry-requirements.txt` into project `.venv/`
