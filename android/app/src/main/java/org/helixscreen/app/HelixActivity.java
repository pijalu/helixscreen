package org.helixscreen.app;

import android.graphics.Color;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Scanner;

import org.helixscreen.app.BuildConfig;
import org.libsdl.app.SDLActivity;

/**
 * HelixScreen Android activity.
 * Extends SDLActivity to provide the SDL2 + native code bridge.
 *
 * System UI behavior:
 *   - Default state: status bar AND navigation bar hidden, using legacy
 *     non-sticky IMMERSIVE flags so the hidden state persists across taps
 *     (plain HIDE_NAVIGATION without IMMERSIVE is only transient).
 *   - Reveal gesture: a swipe that starts within EDGE_ZONE_DP of the bottom
 *     edge and travels upward more than SWIPE_THRESHOLD_DP shows the nav bar
 *     as a translucent "ghost" overlay. A swipe is a drag — LVGL treats it as
 *     a scroll gesture and cancels any pending click, so bottom-row app
 *     buttons stay tappable without accidental reveals.
 *   - Inactivity auto-hide: while the nav bar is visible, each touch resets a
 *     NAV_HIDE_TIMEOUT_MS timer. When that timer fires, the nav bar hides.
 *   - Touch events are never consumed here; SDL / LVGL sees them all.
 *   - Android may also reveal the nav bar itself via its own edge-swipe
 *     detection (non-sticky IMMERSIVE). onSystemUiVisibilityChange keeps our
 *     internal state in sync when that happens and starts the hide timer.
 *
 * TODO: targetSdk 30+ will require migrating from setSystemUiVisibility /
 * OnSystemUiVisibilityChangeListener to WindowInsetsController. Both APIs
 * work alongside SDL2's current Android target, but the legacy ones are
 * deprecated.
 */
public class HelixActivity extends SDLActivity {

    /** A swipe must start within this many dp of the bottom edge to arm. */
    private static final int EDGE_ZONE_DP = 12;

    /** Vertical travel required to trigger the reveal, in dp. */
    private static final int SWIPE_THRESHOLD_DP = 24;

    /** Nav bar hides this many ms after the last touch while it is visible. */
    private static final long NAV_HIDE_TIMEOUT_MS = 3000;

    private boolean mNavBarVisible = false;
    private boolean mSwipeArmed = false;
    private float mSwipeStartY = 0f;

    private final Handler mHideHandler = new Handler(Looper.getMainLooper());
    private final Runnable mHideRunnable = new Runnable() {
        @Override
        public void run() {
            setNavBarVisible(false);
        }
    };
    private final Runnable mApplySystemUiRunnable = new Runnable() {
        @Override
        public void run() {
            applySystemUi();
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL2",
            "main"
        };
    }

    @Override
    protected String[] getArguments() {
        if (BuildConfig.DEBUG) {
            return new String[]{"-vv"};
        }
        return new String[]{};
    }

    private int commonLayoutFlags() {
        return View.SYSTEM_UI_FLAG_LAYOUT_STABLE
             | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
             | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
             | View.SYSTEM_UI_FLAG_FULLSCREEN;
    }

    private void applySystemUi() {
        if (Build.VERSION.SDK_INT < 19) {
            return;
        }
        Window window = getWindow();
        int flags = commonLayoutFlags();
        if (!mNavBarVisible) {
            // IMMERSIVE (non-sticky) is required for HIDE_NAVIGATION to persist
            // across user taps. Without it, Android shows the nav bar again on
            // any interaction. Non-sticky is preferred over IMMERSIVE_STICKY so
            // the system does not auto-rehide bars we have deliberately shown.
            flags |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                   | View.SYSTEM_UI_FLAG_IMMERSIVE;
        }
        window.getDecorView().setSystemUiVisibility(flags);

        if (Build.VERSION.SDK_INT >= 21) {
            window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
            window.setNavigationBarColor(Color.TRANSPARENT);
            window.setStatusBarColor(Color.TRANSPARENT);
        }
    }

    private void scheduleAutoHide() {
        mHideHandler.removeCallbacks(mHideRunnable);
        mHideHandler.postDelayed(mHideRunnable, NAV_HIDE_TIMEOUT_MS);
    }

    private void setNavBarVisible(boolean visible) {
        if (mNavBarVisible == visible) {
            return;
        }
        mNavBarVisible = visible;
        applySystemUi();
        if (visible) {
            scheduleAutoHide();
        } else {
            mHideHandler.removeCallbacks(mHideRunnable);
        }
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        final float density = getResources().getDisplayMetrics().density;
        final int edgeZonePx = (int) (EDGE_ZONE_DP * density + 0.5f);
        final int swipeThresholdPx = (int) (SWIPE_THRESHOLD_DP * density + 0.5f);
        final int decorHeight = getWindow().getDecorView().getHeight();

        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                if (!mNavBarVisible
                        && decorHeight > 0
                        && ev.getY() >= (decorHeight - edgeZonePx)) {
                    mSwipeArmed = true;
                    mSwipeStartY = ev.getY();
                } else {
                    mSwipeArmed = false;
                }
                if (mNavBarVisible) {
                    scheduleAutoHide();
                }
                break;
            case MotionEvent.ACTION_MOVE:
                if (mSwipeArmed && (mSwipeStartY - ev.getY()) >= swipeThresholdPx) {
                    setNavBarVisible(true);
                    mSwipeArmed = false;
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                mSwipeArmed = false;
                break;
        }
        return super.dispatchTouchEvent(ev);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applySystemUi();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // SDL's COMMAND_CHANGE_WINDOW_STYLE handler applies immersive-sticky
        // flags asynchronously on the main thread during startup. Post our
        // reapply to run after those messages drain so we win the race.
        View decor = getWindow().getDecorView();
        decor.removeCallbacks(mApplySystemUiRunnable);
        decor.post(mApplySystemUiRunnable);
    }

    @Override
    protected void onPause() {
        mHideHandler.removeCallbacks(mHideRunnable);
        getWindow().getDecorView().removeCallbacks(mApplySystemUiRunnable);
        super.onPause();
    }

    /**
     * Keep mNavBarVisible in sync when Android itself toggles the nav bar —
     * for example, its own edge-swipe gesture in non-sticky IMMERSIVE mode.
     * We never call setSystemUiVisibility() from here, so there is no feedback
     * loop with applySystemUi() (which always sets flags that match the state
     * we are transitioning to).
     */
    @Override
    public void onSystemUiVisibilityChange(int visibility) {
        boolean navVisibleNow = (visibility & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) == 0;
        if (navVisibleNow == mNavBarVisible) {
            return;
        }
        mNavBarVisible = navVisibleNow;
        if (navVisibleNow) {
            scheduleAutoHide();
        } else {
            mHideHandler.removeCallbacks(mHideRunnable);
        }
    }

    // =========================================================================
    // JNI HTTPS bridge — called from native crash reporter
    // =========================================================================

    /**
     * Perform an HTTPS POST using Android's built-in TLS stack.
     * Called from native code via JNI when libhv lacks SSL support.
     *
     * @param url         Full HTTPS URL
     * @param body        JSON request body
     * @param userAgent   User-Agent header value
     * @param apiKey      X-API-Key header value
     * @param timeoutSec  Connection + read timeout in seconds
     * @return            "STATUS_CODE\nRESPONSE_BODY" on success,
     *                    "0\nERROR_MESSAGE" on failure
     */
    public static String httpsPost(String url, String body, String userAgent,
                                   String apiKey, int timeoutSec) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setConnectTimeout(timeoutSec * 1000);
            conn.setReadTimeout(timeoutSec * 1000);
            conn.setRequestProperty("Content-Type", "application/json");
            conn.setRequestProperty("User-Agent", userAgent);
            conn.setRequestProperty("X-API-Key", apiKey);

            byte[] payload = body.getBytes(StandardCharsets.UTF_8);
            conn.setFixedLengthStreamingMode(payload.length);

            try (OutputStream os = conn.getOutputStream()) {
                os.write(payload);
            }

            int status = conn.getResponseCode();
            String responseBody = "";
            try (Scanner s = new Scanner(
                    status >= 200 && status < 400
                        ? conn.getInputStream() : conn.getErrorStream(),
                    "UTF-8")) {
                s.useDelimiter("\\A");
                if (s.hasNext()) responseBody = s.next();
            }
            return status + "\n" + responseBody;
        } catch (Exception e) {
            Log.w("HelixHTTPS", "POST failed: " + e.getMessage());
            return "0\n" + e.getMessage();
        } finally {
            if (conn != null) conn.disconnect();
        }
    }
}
