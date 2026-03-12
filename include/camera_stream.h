// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

// Camera features available on Pi/desktop. JPEG decoding tries libturbojpeg
// (SIMD-accelerated, loaded via dlopen) at runtime, falling back to stb_image.
#if HELIX_HAS_CAMERA

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class HttpRequest;

namespace helix {

/**
 * @brief MJPEG camera stream decoder with snapshot fallback
 *
 * Connects to an MJPEG stream URL, parses multipart boundaries, decodes
 * JPEG frames via libturbojpeg (SIMD-accelerated, runtime-loaded) or
 * stb_image (fallback), and delivers decoded RGB888 frames via callback.
 * Falls back to periodic snapshot polling if streaming fails.
 *
 * Threading: decode happens on a background thread. The on_frame callback is
 * called from that thread — callers must use ui_queue_update() to marshal to
 * the LVGL thread.
 */
class CameraStream {
  public:
    using FrameCallback = std::function<void(lv_draw_buf_t* frame)>;
    using ErrorCallback = std::function<void(const char* message)>;

    CameraStream();
    ~CameraStream();

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    void set_flip(bool horizontal, bool vertical) {
        flip_h_.store(horizontal);
        flip_v_.store(vertical);
    }

    /**
     * @brief Configure stream URLs and flip from printer state.
     *
     * Reads webcam URLs from PrinterState, resolves relative URLs via
     * MoonrakerAPI, and applies flip settings. Call before start().
     *
     * @param[out] stream_url Resolved stream URL (empty if no webcam)
     * @param[out] snapshot_url Resolved snapshot URL (empty if no webcam)
     * @return true if at least one URL is available
     */
    bool configure_from_printer(std::string& stream_url, std::string& snapshot_url);

    /**
     * @brief Copy pixels to LVGL BGR format with optional flip.
     *
     * When using stb_image (fallback), source is RGB and R↔B swap is needed.
     * When using turbojpeg, source is already BGR — only flip is applied.
     * Exposed as public static for unit testing.
     */
    static void copy_pixels_to_lvgl(const uint8_t* src, uint8_t* dst, int width, int height,
                                    int src_stride, int dst_stride, bool flip_h, bool flip_v,
                                    bool swap_rb);

    // Legacy name kept for test compatibility
    static void copy_pixels_rgb_to_lvgl(const uint8_t* src, uint8_t* dst, int width, int height,
                                        int src_stride, int dst_stride, bool flip_h, bool flip_v) {
        copy_pixels_to_lvgl(src, dst, width, height, src_stride, dst_stride, flip_h, flip_v, true);
    }

    /**
     * @brief Parse a boundary string from a Content-Type header value.
     *
     * Extracts the boundary parameter, strips quotes, and prepends "--" if
     * needed. Returns empty string if no boundary found. Exposed for testing.
     */
    static std::string parse_boundary(const std::string& content_type);

    void start(const std::string& stream_url, const std::string& snapshot_url,
               FrameCallback on_frame, ErrorCallback on_error = nullptr);
    void stop();
    bool is_running() const;

    /// Called by widget when it has consumed the current frame
    void frame_consumed();

  private:
    int process_stream_data();
    void stream_thread_func();
    void snapshot_poll_loop();
    void fetch_snapshot();
    bool decode_jpeg(const uint8_t* data, size_t len);
    bool decode_jpeg_turbojpeg(const uint8_t* data, size_t len);
    bool decode_jpeg_stb(const uint8_t* data, size_t len);
    void deliver_frame();
    void ensure_buffers(int width, int height);
    void free_buffers();

    std::string stream_url_;
    std::string snapshot_url_;
    FrameCallback on_frame_;
    ErrorCallback on_error_;
    std::atomic<bool> flip_h_{false};
    std::atomic<bool> flip_v_{false};

    // Draw buffer helpers — use system malloc (thread-safe) instead of
    // lv_draw_buf_create which calls lv_malloc (NOT thread-safe)
    static lv_draw_buf_t* create_draw_buf(uint32_t w, uint32_t h, lv_color_format_t cf);
    static void destroy_draw_buf(lv_draw_buf_t* buf);

    // Double buffer — decode into back, swap to front on delivery
    lv_draw_buf_t* front_buf_ = nullptr;
    lv_draw_buf_t* back_buf_ = nullptr;
    int frame_width_ = 0;
    int frame_height_ = 0;
    std::mutex buf_mutex_;

    // Old front buffers awaiting safe cleanup — LVGL may still reference
    // them via lv_image_set_src until the widget clears the source
    std::vector<lv_draw_buf_t*> retired_bufs_;

    // MJPEG parser state
    std::vector<uint8_t> recv_buf_;
    std::string boundary_;

    // Active HTTP request — weak_ptr avoids refcount races between the stream
    // thread and libhv's internal thread pool.  stop() locks the weak_ptr
    // temporarily to call Cancel().
    std::weak_ptr<HttpRequest> active_req_;
    std::mutex req_mutex_;

    // State — alive_ is captured as weak_ptr by http_cb closures so they
    // can detect object destruction and bail out before touching members
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> running_{false};
    std::atomic<bool> frame_pending_{false};
    std::atomic<bool> got_stream_data_{false}; // Set by http_cb when data arrives
    int stream_fail_count_ = 0;
    bool thread_detached_ = false; // Set by stop() if thread join times out
    std::thread stream_thread_;

    static constexpr int kMaxStreamFailures = 3;
    static constexpr int kSnapshotIntervalMs = 2000;
    static constexpr int kStreamConnectTimeoutSec = 5;   // Initial connection attempt
    static constexpr int kStreamTimeoutSec = 300;       // Active stream — reconnects on timeout

    // libturbojpeg runtime loading (dlopen) — nullptr if unavailable
    void* tj_lib_ = nullptr;   // dlopen handle
    void* tj_ = nullptr;       // tjhandle (decompressor instance)
    // Function pointers loaded via dlsym
    using TjInitDecompress_t = void* (*)();
    using TjDecompressHeader3_t = int (*)(void*, const unsigned char*, unsigned long,
                                          int*, int*, int*, int*);
    using TjDecompress2_t = int (*)(void*, const unsigned char*, unsigned long,
                                    unsigned char*, int, int, int, int, int);
    using TjDestroy_t = int (*)(void*);
    using TjGetErrorStr2_t = char* (*)(void*);
    TjDecompressHeader3_t fn_decompress_header_ = nullptr;
    TjDecompress2_t fn_decompress_ = nullptr;
    TjDestroy_t fn_destroy_ = nullptr;
    TjGetErrorStr2_t fn_get_error_ = nullptr;
};

} // namespace helix

#endif // HELIX_HAS_CAMERA
