// SPDX-License-Identifier: GPL-3.0-or-later
#include "qr_decoder.h"

#include <climits>
#include <cstdlib>
#include <cstring>

#include <spdlog/spdlog.h>

#include "quirc/lib/quirc.h"

namespace helix {

QrDecoder::QrDecoder()
{
    qr_ = quirc_new();
    if (!qr_) {
        spdlog::error("QrDecoder: failed to allocate quirc context");
    }
}

QrDecoder::~QrDecoder()
{
    if (qr_) {
        quirc_destroy(qr_);
    }
}

QrDecodeResult QrDecoder::decode(const uint8_t* gray_data, int width, int height)
{
    QrDecodeResult result;

    if (!qr_ || !gray_data || width <= 0 || height <= 0) {
        return result;
    }

    if (quirc_resize(qr_, width, height) < 0) {
        spdlog::error("QrDecoder: failed to resize quirc to {}x{}", width, height);
        return result;
    }

    // Copy grayscale data into quirc buffer
    int buf_w = 0;
    int buf_h = 0;
    uint8_t* buf = quirc_begin(qr_, &buf_w, &buf_h);
    if (!buf) {
        spdlog::error("QrDecoder: quirc_begin returned null");
        return result;
    }

    // Copy row by row (quirc buffer stride may differ from width)
    int copy_w = (width < buf_w) ? width : buf_w;
    int copy_h = (height < buf_h) ? height : buf_h;
    for (int y = 0; y < copy_h; ++y) {
        std::memcpy(buf + y * buf_w, gray_data + y * width, copy_w);
    }

    quirc_end(qr_);

    int count = quirc_count(qr_);
    spdlog::debug("QrDecoder: found {} QR code candidates in {}x{} image", count, width, height);

    for (int i = 0; i < count; ++i) {
        struct quirc_code code;
        struct quirc_data data;

        quirc_extract(qr_, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);

        if (err != QUIRC_SUCCESS) {
            spdlog::debug("QrDecoder: code {} decode error: {}", i, quirc_strerror(err));
            continue;
        }

        result.success = true;
        result.text = std::string(reinterpret_cast<const char*>(data.payload), data.payload_len);
        result.spool_id = parse_spoolman_id(result.text);

        spdlog::debug("QrDecoder: decoded QR text='{}' spool_id={}", result.text, result.spool_id);
        break;
    }

    return result;
}

// Safe string-to-int conversion. Returns -1 on overflow or invalid input.
static int safe_parse_int(const std::string& s)
{
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
        return -1;
    }
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || val < 0 || val > INT_MAX || end != s.c_str() + s.size()) {
        return -1;
    }
    return static_cast<int>(val);
}

int QrDecoder::parse_spoolman_id(const std::string& text)
{
    if (text.empty()) {
        return -1;
    }

    // Format: "web+spoolman:s-<id>"
    const std::string prefix_ws = "web+spoolman:s-";
    if (text.size() > prefix_ws.size() &&
        text.compare(0, prefix_ws.size(), prefix_ws) == 0) {
        return safe_parse_int(text.substr(prefix_ws.size()));
    }

    // Format: "SM:SPOOL=<id>"
    const std::string prefix_sm = "SM:SPOOL=";
    if (text.size() > prefix_sm.size() &&
        text.compare(0, prefix_sm.size(), prefix_sm) == 0) {
        return safe_parse_int(text.substr(prefix_sm.size()));
    }

    // URL format: ".../spool/<id>" or ".../spool/<id>/"
    const std::string spool_path = "/spool/";
    auto pos = text.rfind(spool_path);
    if (pos != std::string::npos) {
        auto id_start = pos + spool_path.size();
        if (id_start < text.size()) {
            auto id_end = text.find('/', id_start);
            std::string id_str;
            if (id_end != std::string::npos) {
                id_str = text.substr(id_start, id_end - id_start);
            } else {
                id_str = text.substr(id_start);
            }
            return safe_parse_int(id_str);
        }
    }

    return -1;
}

} // namespace helix
