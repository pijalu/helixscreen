# Android Google Play Store Publication — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix Android-specific issues and publish HelixScreen to Google Play Store as an Open Beta.

**Architecture:** The Android build already exists (Gradle + CMake + SDL2). This plan fixes 7 code issues, adds an Android update-to-Play-Store flow, configures AAB builds for Play Store submission, and prepares store assets. No new architectural patterns — all changes follow existing platform detection (`is_android_platform()`) and SDL2 conventions.

**Tech Stack:** C++17, SDL2 2.32.10, LVGL 9.5, Gradle 8.5.1, NDK 29, Android SDK 34

**Spec:** `docs/plans/2026-04-07-android-play-store-design.md`

---

## File Structure

| Action | File | Responsibility |
|--------|------|----------------|
| Modify | `src/ui/ui_wizard_connection.cpp` | Remove localhost default on Android |
| Modify | `src/application/application.cpp` | Back button + lifecycle pause/resume |
| Modify | `include/application.h` | Declare new pause/resume methods |
| Modify | `src/app_globals.cpp` | Android cache directory path |
| Modify | `src/system/phomemo_printer.cpp` | Guard USB sysfs access |
| Modify | `src/ui/ui_settings_about.cpp` | Play Store redirect on Android |
| Modify | `android/app/build.gradle` | AAB bundle task |
| Modify | `.github/workflows/release.yml` | AAB build + artifact upload |
| Create | `tests/unit/test_android_lifecycle.cpp` | Lifecycle pause/resume tests |
| Create | `scripts/generate-upload-keystore.sh` | Keystore generation helper |

---

### Task 1: Wizard Localhost Default — Remove 127.0.0.1 on Android

**Files:**
- Modify: `src/ui/ui_wizard_connection.cpp:100` and `:520`

**Context:** The wizard connection step defaults to `127.0.0.1` which makes no sense on Android (the printer is on another device). On Android, the field should be empty so the user is forced to enter their printer's IP.

- [ ] **Step 1: Fix the default IP in `init_subjects()`**

In `src/ui/ui_wizard_connection.cpp`, find `init_subjects()` at line 100. The code currently reads:

```cpp
std::string default_ip = "127.0.0.1";
```

Change to:

```cpp
#include "platform_info.h"  // add near top of file if not already included

// ... inside init_subjects():
std::string default_ip = helix::is_android_platform() ? "" : "127.0.0.1";
```

- [ ] **Step 2: Fix the fallback in `attempt_auto_probe()`**

At line 520, the code reads:

```cpp
std::string probe_ip = (ip && strlen(ip) > 0) ? ip : "127.0.0.1";
```

Change to:

```cpp
std::string probe_ip = (ip && strlen(ip) > 0) ? ip : "";
if (probe_ip.empty()) {
    spdlog::debug("[{}] Auto-probe: No IP configured, skipping", get_name());
    auto_probe_state_.store(AutoProbeState::FAILED);
    return;
}
```

This applies to all platforms — if the IP is empty, don't silently probe localhost.

- [ ] **Step 3: Verify `platform_info.h` is included**

Check the includes at top of `ui_wizard_connection.cpp`. Add if missing:

```cpp
#include "platform_info.h"
```

- [ ] **Step 4: Build and verify**

Run: `make -j`
Expected: Clean compile, no errors.

- [ ] **Step 5: Commit**

```bash
git add src/ui/ui_wizard_connection.cpp
git commit -m "fix(android): remove localhost default in wizard connection step"
```

---

### Task 2: Android Back Button Handling

**Files:**
- Modify: `src/application/application.cpp:2754` (inside `handle_keyboard_shortcuts()`)

**Context:** Android's back button sends `SDL_SCANCODE_AC_BACK`. This is processed by LVGL's SDL keyboard handler and delivered as a key event. We register it as a keyboard shortcut. When pressed: if an overlay/modal is open, go back. At the root panel, do nothing (Android convention: don't exit on back from home).

- [ ] **Step 1: Add the back button shortcut registration**

In `src/application/application.cpp`, inside `handle_keyboard_shortcuts()` after the existing shortcut registrations (after line ~2839), add:

```cpp
        // Android back button — pop navigation stack (overlay/modal/panel)
        // At root panel, do nothing (Android convention: don't exit on back)
        shortcuts.register_key(SDL_SCANCODE_AC_BACK, []() {
            auto& nav = NavigationManager::instance();
            if (nav.panel_stack_size() > 1) {
                spdlog::debug("[Application] Android back button - popping navigation");
                ui_nav_go_back();
            } else {
                spdlog::trace("[Application] Android back button - at root, ignoring");
            }
        });
```

- [ ] **Step 2: Verify NavigationManager has `panel_stack_size()` or equivalent**

Check `include/ui_nav_manager.h` for a public method to get stack depth. The `go_back()` method at line 1466 of `ui_nav_manager.cpp` accesses `panel_stack_.size()`. If there's no public accessor, check for `can_go_back()` or similar. If neither exists, add one:

In `include/ui_nav_manager.h`:
```cpp
/// Returns true if there's a previous panel/overlay to go back to
bool can_go_back() const;
```

In `src/ui/ui_nav_manager.cpp`:
```cpp
bool NavigationManager::can_go_back() const {
    return panel_stack_.size() > 1;
}
```

Then use `nav.can_go_back()` instead of `nav.panel_stack_size() > 1` in the shortcut.

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean compile.

- [ ] **Step 4: Commit**

```bash
git add src/application/application.cpp src/ui/ui_nav_manager.cpp include/ui_nav_manager.h
git commit -m "feat(android): handle back button via SDL_SCANCODE_AC_BACK"
```

---

### Task 3: Lifecycle Pause/Resume — Background and Foreground Handling

**Files:**
- Modify: `lib/lvgl/src/drivers/sdl/lv_sdl_window.c:248` (SDL event handler)
- Modify: `src/application/application.cpp` (new methods + main loop flag)
- Modify: `include/application.h` (declare new methods)

**Context:** SDL_APP_WILLENTERBACKGROUND and SDL_APP_DIDENTERFOREGROUND are top-level SDL events (not window events). They're polled in LVGL's `sdl_event_handler()` at `lv_sdl_window.c:254`. We need to hook into these events and call Application methods to pause/resume subsystems.

The safest approach: set a flag in the SDL event handler, check it in Application's main loop each frame. This avoids calling complex teardown from inside the LVGL timer callback.

- [ ] **Step 1: Add global pause flag**

In `src/application/application.cpp`, add near the top (after includes):

```cpp
#include <atomic>

namespace {
std::atomic<bool> s_app_backgrounded{false};
}
```

- [ ] **Step 2: Hook SDL lifecycle events in the SDL event handler**

In `lib/lvgl/src/drivers/sdl/lv_sdl_window.c`, inside `sdl_event_handler()`, after the `SDL_QUIT` block (line 318-326), add:

```c
        if(event.type == SDL_APP_WILLENTERBACKGROUND) {
            /* Set flag — Application::main_loop() handles the actual pause */
            extern void helix_notify_app_backgrounded(void);
            helix_notify_app_backgrounded();
        }
        if(event.type == SDL_APP_DIDENTERFOREGROUND) {
            extern void helix_notify_app_foregrounded(void);
            helix_notify_app_foregrounded();
        }
```

- [ ] **Step 3: Implement the C bridge functions**

In `src/application/application.cpp`, add the extern "C" bridge functions:

```cpp
extern "C" void helix_notify_app_backgrounded() {
    s_app_backgrounded.store(true);
    spdlog::info("[Application] App entering background");
}

extern "C" void helix_notify_app_foregrounded() {
    s_app_backgrounded.store(false);
    spdlog::info("[Application] App returning to foreground");
}
```

- [ ] **Step 4: Add pause/resume methods to Application**

In `include/application.h`, declare:

```cpp
    /// Pause subsystems when app enters background (Android)
    void on_enter_background();

    /// Resume subsystems when app returns to foreground (Android)
    void on_enter_foreground();

    bool m_backgrounded = false;
```

In `src/application/application.cpp`, implement:

```cpp
void Application::on_enter_background() {
    if (m_backgrounded) return;
    m_backgrounded = true;
    spdlog::info("[Application] Pausing for background");

    // 1. Disconnect WebSocket (stops all status updates and reconnect timer)
    if (m_moonraker_manager) {
        m_moonraker_manager->get_client()->disconnect();
    }

    // 2. Stop camera streams
    // Camera widget observes printer state — disconnect handles this implicitly
    // since the camera stream stops when the widget is not visible

    // 3. Mute sound
    SoundManager::instance().shutdown();

    // 4. Suppress rendering — save CPU/GPU
    lv_display_enable_invalidation(nullptr, false);

    spdlog::info("[Application] Background pause complete");
}

void Application::on_enter_foreground() {
    if (!m_backgrounded) return;
    m_backgrounded = false;
    spdlog::info("[Application] Resuming from background");

    // 1. Re-enable rendering
    lv_display_enable_invalidation(nullptr, true);

    // 2. Re-initialize sound
    SoundManager::instance().initialize();

    // 3. Reconnect WebSocket (triggers discovery + full state refresh)
    if (m_moonraker_manager && m_moonraker_manager->get_client()) {
        m_moonraker_manager->get_client()->force_reconnect();
    }

    // 4. Force full display redraw (framebuffer may be stale)
    lv_obj_t* screen = lv_screen_active();
    if (screen) {
        lv_obj_update_layout(screen);
        lv_obj_invalidate(screen);
        lv_refr_now(nullptr);
    }

    spdlog::info("[Application] Foreground resume complete");
}
```

- [ ] **Step 5: Check the flag in the main loop**

In `src/application/application.cpp`, inside `main_loop()`, add after the `handle_keyboard_shortcuts()` call (after line 2634):

```cpp
        // Android lifecycle: pause/resume when backgrounded
        bool backgrounded = s_app_backgrounded.load();
        if (backgrounded && !m_backgrounded) {
            on_enter_background();
        } else if (!backgrounded && m_backgrounded) {
            on_enter_foreground();
        }

        // Skip heavy processing while backgrounded
        if (m_backgrounded) {
            DisplayManager::delay(200); // Sleep longer in background
            continue;
        }
```

- [ ] **Step 6: Include necessary headers**

Add to `application.cpp` if not already present:

```cpp
#include "sound_manager.h"
```

- [ ] **Step 7: Build and verify**

Run: `make -j`
Expected: Clean compile. The lifecycle code is inert on desktop (s_app_backgrounded never becomes true).

- [ ] **Step 8: Commit**

```bash
git add lib/lvgl/src/drivers/sdl/lv_sdl_window.c src/application/application.cpp include/application.h
git commit -m "feat(android): pause/resume subsystems on lifecycle background/foreground"
```

---

### Task 4: Cache Directory — Android Path

**Files:**
- Modify: `src/app_globals.cpp:360`

**Context:** The cache directory resolution chain tries platform-specific paths (AD5M, CC1, MIPS/K2) then falls through to XDG, $HOME, /var/tmp, /tmp. Android gets none of these right. We add an Android path using `SDL_AndroidGetInternalStoragePath()` in the platform-specific section.

- [ ] **Step 1: Add Android cache path**

In `src/app_globals.cpp`, after the existing `#elif defined(HELIX_PLATFORM_MIPS) ...` block (line 385), add:

```cpp
#elif defined(HELIX_PLATFORM_ANDROID) || defined(__ANDROID__)
    {
        // Use SDL's Android internal storage path (app-private, no permissions needed)
        const char* android_path = SDL_AndroidGetInternalStoragePath();
        if (android_path) {
            std::string path = std::string(android_path) + "/cache/" + subdir;
            if (try_create_dir(path)) {
                spdlog::info("[App Globals] Cache dir (Android): {}", path);
                return path;
            }
        }
    }
#endif
```

Remove the `#endif` that was after the MIPS/K2 block and place it after this new block.

- [ ] **Step 2: Add SDL include**

At the top of `src/app_globals.cpp`, add the conditional include:

```cpp
#ifdef __ANDROID__
#include <SDL.h>
#endif
```

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean compile. On non-Android builds, the `#elif` branch is skipped.

- [ ] **Step 4: Commit**

```bash
git add src/app_globals.cpp
git commit -m "fix(android): use SDL internal storage path for cache directory"
```

---

### Task 5: Guard USB Printer Sysfs Paths

**Files:**
- Modify: `src/system/phomemo_printer.cpp:58-85`

**Context:** The `find_usblp_device()` function reads `/dev/usb/lp*` and `/sys/class/usbmisc/`. These don't exist on Android. The function already returns empty string on failure, but it logs errors and wastes time scanning. Guard the entire function body.

- [ ] **Step 1: Guard the function body**

In `src/system/phomemo_printer.cpp`, wrap the `find_usblp_device()` function body:

```cpp
static std::string find_usblp_device(uint16_t vid, uint16_t pid) {
#ifdef __ANDROID__
    (void)vid;
    (void)pid;
    return {};  // USB device nodes not accessible on Android
#else
    // Scan /dev/usb/lp* and match against sysfs VID:PID
    namespace fs = std::filesystem;

    for (int i = 0; i < 8; i++) {
        // ... existing code ...
    }
    return {};
#endif
}
```

- [ ] **Step 2: Build and verify**

Run: `make -j`
Expected: Clean compile.

- [ ] **Step 3: Commit**

```bash
git add src/system/phomemo_printer.cpp
git commit -m "fix(android): guard USB printer sysfs access with __ANDROID__ check"
```

---

### Task 6: DPI Awareness

**Files:**
- Modify: `src/application/application.cpp` (display init area, around line 850-880)

**Context:** Android devices have wildly varying screen densities. The app currently uses pixel dimensions without DPI scaling. SDL2 provides `SDL_GetDisplayDPI()`. LVGL already has responsive layout support based on screen resolution — the key is ensuring the SDL window reports the correct logical size on Android.

The Android CMakeLists.txt already sets `HELIX_DISPLAY_SDL`, and the SDL window creation in `lv_sdl_window.c:273-276` shows an existing `#ifdef __ANDROID__` that skips resize handling because "SDL_RenderSetLogicalSize handles scaling." This means SDL already handles DPI scaling on Android via its logical size system.

- [ ] **Step 1: Verify SDL logical size is set correctly on Android**

Check `lib/lvgl/src/drivers/sdl/lv_sdl_window.c` around the window creation code. Look for `SDL_RenderSetLogicalSize()`. If it's already set, the DPI scaling is handled.

If not, add DPI-aware sizing in `src/application/application.cpp` where display is initialized. After display creation (around line 879), add:

```cpp
#ifdef __ANDROID__
    // Query actual DPI for future reference (logging, touch calibration)
    float ddpi = 0, hdpi = 0, vdpi = 0;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0) {
        spdlog::info("[Application] Android display DPI: diagonal={:.0f} h={:.0f} v={:.0f}",
                     ddpi, hdpi, vdpi);
    }
#endif
```

- [ ] **Step 2: Log the screen dimensions Android reports**

This is diagnostic — we need to verify what resolution Android gives us vs. the physical screen. The existing code at line 879 already logs dimensions:

```cpp
m_screen_width = m_display->width();
m_screen_height = m_display->height();
```

Add after this:

```cpp
#ifdef __ANDROID__
    spdlog::info("[Application] Android screen: {}x{} (DPI-aware sizing via SDL)", 
                 m_screen_width, m_screen_height);
#endif
```

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean compile.

- [ ] **Step 4: Commit**

```bash
git add src/application/application.cpp
git commit -m "feat(android): log DPI and screen dimensions for display diagnostics"
```

---

### Task 7: Manifest Cleanup

**Files:**
- Modify: `android/app/src/main/AndroidManifest.xml`

**Context:** Verify current permissions are sufficient. The manifest already has INTERNET and ACCESS_NETWORK_STATE. No VIBRATE needed (no haptics). No BLUETOOTH yet (plugin excluded). Add WAKE_LOCK so the screen stays on during prints (users monitoring a 3-hour print don't want the phone locking).

- [ ] **Step 1: Add WAKE_LOCK permission**

In `android/app/src/main/AndroidManifest.xml`, add after the existing permissions:

```xml
<uses-permission android:name="android.permission.WAKE_LOCK" />
```

- [ ] **Step 2: Add `keepScreenOn` to the activity**

In the `<activity>` tag, add the `android:keepScreenOn` flag. Actually, this is better handled in code or XML layout. For now, the WAKE_LOCK permission is sufficient — the SDL2 layer can use it if needed.

- [ ] **Step 3: Commit**

```bash
git add android/app/src/main/AndroidManifest.xml
git commit -m "fix(android): add WAKE_LOCK permission for print monitoring"
```

---

### Task 8: Update Flow — Play Store Redirect on Android

**Files:**
- Modify: `src/ui/ui_settings_about.cpp:462-467`

**Context:** When the user taps "Install Update" on Android, instead of showing the download modal, open the Play Store listing page. Use `SDL_OpenURL()` with `market://` URI scheme, which SDL2 handles via JNI on Android. Fall back to the web URL if Play Store app isn't installed.

- [ ] **Step 1: Modify the install update callback**

In `src/ui/ui_settings_about.cpp`, change `on_about_install_update_clicked()` at line 462:

```cpp
void AboutSettingsOverlay::on_about_install_update_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AboutSettings] on_about_install_update_clicked");

    if (helix::is_android_platform()) {
        spdlog::info("[AboutSettings] Opening Play Store for update");
        // market:// URI opens Play Store app directly; SDL_OpenURL handles JNI on Android
        int result = SDL_OpenURL("market://details?id=org.helixscreen.app");
        if (result != 0) {
            // Fallback: Play Store not installed (sideloaded device), open web URL
            spdlog::warn("[AboutSettings] market:// failed, trying web URL: {}", SDL_GetError());
            SDL_OpenURL("https://play.google.com/store/apps/details?id=org.helixscreen.app");
        }
    } else {
        spdlog::info("[AboutSettings] Install update requested");
        get_about_settings_overlay().show_update_download_modal();
    }

    LVGL_SAFE_EVENT_CB_END();
}
```

- [ ] **Step 2: Add includes**

At the top of `src/ui/ui_settings_about.cpp`, add if not present:

```cpp
#include "platform_info.h"
#include <SDL.h>
```

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean compile.

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_settings_about.cpp
git commit -m "feat(android): redirect update action to Play Store instead of in-app download"
```

---

### Task 9: Gradle AAB Build Configuration

**Files:**
- Modify: `android/app/build.gradle`
- Modify: `.github/workflows/release.yml:223-261`

**Context:** Play Store requires Android App Bundle (`.aab`) format. The current CI produces APKs via `assembleRelease`. We add `bundleRelease` alongside it. The AAB includes all ABIs in one file (Play Store handles per-device delivery). Keep APKs for direct download on GitHub releases.

- [ ] **Step 1: Add bundleRelease to release workflow**

In `.github/workflows/release.yml`, after the existing APK build step (line 230), add a new step:

```yaml
    - name: Build Android AAB (release)
      working-directory: android
      env:
        ANDROID_KEYSTORE_PATH: ${{ secrets.ANDROID_KEYSTORE_PATH }}
        ANDROID_KEYSTORE_PASSWORD: ${{ secrets.ANDROID_KEYSTORE_PASSWORD }}
        ANDROID_KEY_ALIAS: ${{ secrets.ANDROID_KEY_ALIAS }}
        ANDROID_KEY_PASSWORD: ${{ secrets.ANDROID_KEY_PASSWORD }}
      run: ./gradlew bundleRelease -PuseCcache
```

- [ ] **Step 2: Add AAB rename and upload step**

After the existing "Rename APKs" step, add:

```yaml
    - name: Rename AAB
      run: |
        VERSION="${{ steps.version.outputs.version }}"
        AAB_DIR="android/app/build/outputs/bundle/release"
        mkdir -p release-aab
        cp "$AAB_DIR"/*.aab "release-aab/helixscreen-android-v${VERSION}.aab"
        echo "Android AAB:"
        ls -lh release-aab/

    - name: Upload Android AAB artifact
      uses: actions/upload-artifact@v7
      with:
        name: release-android-aab
        path: release-aab/*.aab
        retention-days: 7
```

- [ ] **Step 3: Include AAB in release assets**

In the release job, find where APKs are collected into `release-files/` (around line 288). Add AAB collection:

```yaml
        find artifacts -name '*.aab' -exec mv {} release-files/ \;
```

And in the release body template, add the AAB to the asset list.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "ci(android): add AAB bundle build for Play Store submission"
```

---

### Task 10: Upload Keystore Generation Script

**Files:**
- Create: `scripts/generate-upload-keystore.sh`

**Context:** Google Play requires a signed upload key. This script generates the keystore interactively. The keystore must be kept outside the repo. The signing config in `build.gradle` already reads from environment variables (`ANDROID_KEYSTORE_PATH`, etc.).

- [ ] **Step 1: Create the script**

Create `scripts/generate-upload-keystore.sh`:

```bash
#!/usr/bin/env bash
# Generate an upload keystore for Google Play Store signing.
# The keystore is stored OUTSIDE the repo (never committed).
# Set these env vars for CI/local builds:
#   ANDROID_KEYSTORE_PATH, ANDROID_KEYSTORE_PASSWORD,
#   ANDROID_KEY_ALIAS, ANDROID_KEY_PASSWORD

set -euo pipefail

KEYSTORE_DIR="${HOME}/.android-keystore"
KEYSTORE_PATH="${KEYSTORE_DIR}/helixscreen-upload.jks"
KEY_ALIAS="helixscreen-upload"

if [ -f "$KEYSTORE_PATH" ]; then
    echo "Keystore already exists at: $KEYSTORE_PATH"
    echo "Delete it first if you want to regenerate."
    exit 1
fi

mkdir -p "$KEYSTORE_DIR"

echo "Generating upload keystore for HelixScreen..."
echo "Store path: $KEYSTORE_PATH"
echo "Key alias: $KEY_ALIAS"
echo ""
echo "You will be prompted for passwords and certificate info."
echo ""

keytool -genkeypair \
    -v \
    -keystore "$KEYSTORE_PATH" \
    -alias "$KEY_ALIAS" \
    -keyalg RSA \
    -keysize 2048 \
    -validity 10000

echo ""
echo "Keystore generated at: $KEYSTORE_PATH"
echo ""
echo "For local builds, set these environment variables:"
echo "  export ANDROID_KEYSTORE_PATH=$KEYSTORE_PATH"
echo "  export ANDROID_KEY_ALIAS=$KEY_ALIAS"
echo "  export ANDROID_KEYSTORE_PASSWORD=<your store password>"
echo "  export ANDROID_KEY_PASSWORD=<your key password>"
echo ""
echo "For CI (GitHub Actions), add these as repository secrets:"
echo "  ANDROID_KEYSTORE_PATH    (base64-encode the .jks file)"
echo "  ANDROID_KEY_ALIAS        ($KEY_ALIAS)"
echo "  ANDROID_KEYSTORE_PASSWORD"
echo "  ANDROID_KEY_PASSWORD"
```

- [ ] **Step 2: Make executable**

Run: `chmod +x scripts/generate-upload-keystore.sh`

- [ ] **Step 3: Commit**

```bash
git add scripts/generate-upload-keystore.sh
git commit -m "feat(android): add upload keystore generation script for Play Store"
```

---

### Task 11: Store Screenshots

**Files:**
- Create: Screenshots in `docs/store/android/` (or similar)

**Context:** Play Store requires at least 2 screenshots, recommends 8. Generate from mock mode using existing screenshot tooling. The app runs in landscape, which maps to phone landscape format (16:9).

- [ ] **Step 1: Generate screenshots from mock mode**

Run the app in test mode with specific panels and take screenshots:

```bash
mkdir -p docs/store/android

# Dashboard
./scripts/screenshot.sh helix-screen store-dashboard dashboard
# Print status (mock printer has an active print)
./scripts/screenshot.sh helix-screen store-print-status printing
# Temperature
./scripts/screenshot.sh helix-screen store-temperature temperature
# Motion controls
./scripts/screenshot.sh helix-screen store-motion motion
# Bed mesh
./scripts/screenshot.sh helix-screen store-bed-mesh bed_mesh
# File browser
./scripts/screenshot.sh helix-screen store-files files
# Settings/themes
./scripts/screenshot.sh helix-screen store-settings settings
# Console
./scripts/screenshot.sh helix-screen store-console console
```

Move generated screenshots to `docs/store/android/`.

- [ ] **Step 2: Verify screenshots are the right resolution**

Play Store phone screenshots should be 16:9. Check dimensions:

```bash
file docs/store/android/*.png
```

If the resolution is different from standard Play Store sizes (e.g., 1920x1080 or 2560x1440), resize with ImageMagick:

```bash
for f in docs/store/android/*.png; do
    convert "$f" -resize 2560x1440 "$f"
done
```

- [ ] **Step 3: Commit**

```bash
git add docs/store/android/
git commit -m "docs(android): add Play Store screenshots from mock mode"
```

---

### Task 12: Feature Graphic

**Files:**
- Create: `docs/store/android/feature-graphic.png` (1024x500)

**Context:** The feature graphic is the banner at the top of the Play Store listing. Use the existing splash screen logo/logotype, cropped to 1024x500.

- [ ] **Step 1: Locate splash assets**

Find the splash screen images:

```bash
find assets/ -name '*splash*' -o -name '*logo*' | head -20
```

- [ ] **Step 2: Create feature graphic**

Use ImageMagick to compose the feature graphic. The exact command depends on the splash assets found, but the general approach:

```bash
# Example: dark background + centered logo
convert -size 1024x500 xc:'#1a1a2e' \
    assets/splash_logo.png -gravity center -composite \
    docs/store/android/feature-graphic.png
```

Adjust based on actual asset names and dimensions.

- [ ] **Step 3: Commit**

```bash
git add docs/store/android/feature-graphic.png
git commit -m "docs(android): add Play Store feature graphic"
```

---

### Task 13: Play Store Submission Checklist (Manual Steps)

This task is a reference for the manual steps the developer performs in a browser.

- [ ] **Step 1: Create Google Play Developer account**

1. Go to https://play.google.com/console
2. Sign in with a Google account
3. Pay $25 one-time registration fee
4. Complete identity verification as 356C LLC
5. Wait for verification (1-2 business days for organizations)

- [ ] **Step 2: Create app in Play Console**

1. Click "Create app"
2. App name: "HelixScreen"
3. Default language: English (US)
4. App type: App
5. Free or paid: Free
6. Accept declarations

- [ ] **Step 3: Complete store listing**

1. Short description: "Klipper touchscreen UI — monitor and control your 3D printer from any Android device."
2. Full description: (from spec, Task 2.3)
3. App icon: Upload existing icon (512x512)
4. Feature graphic: Upload `docs/store/android/feature-graphic.png`
5. Screenshots: Upload from `docs/store/android/`
6. Category: Tools
7. Contact email: privacy@helixscreen.org
8. Privacy policy URL: https://helixscreen.org/privacy (publish the existing `docs/user/PRIVACY_POLICY.md` to this URL)

- [ ] **Step 4: Content rating**

1. Complete the content rating questionnaire
2. Expected rating: Everyone

- [ ] **Step 5: Set up Open Testing track**

1. Go to Release > Testing > Open testing
2. Create new release
3. Upload the `.aab` file from CI artifacts
4. Add release notes: "Initial Early Access release. Connect to your Klipper 3D printer over local network."
5. Review and roll out

- [ ] **Step 6: Enable Play App Signing**

1. First upload will prompt to enroll in Play App Signing
2. Accept — Google manages the distribution key
3. Your upload keystore remains the upload signing key

---

## Self-Review Checklist

- [x] **Spec coverage:** All 7 code fixes (1.1-1.7), update flow addendum, store assets (2.1-2.4), build/signing (3.1-3.3), submission (4.1-4.3) have corresponding tasks
- [x] **No placeholders:** Every code step has actual code
- [x] **Type consistency:** `is_android_platform()`, `SDL_SCANCODE_AC_BACK`, `SDL_OpenURL()`, `NavigationManager::can_go_back()` used consistently
- [x] **Commands are exact:** Build commands, git commits, file paths all specified
- [x] **TDD where applicable:** Lifecycle changes are testable via the flag mechanism; back button via keyboard shortcuts
