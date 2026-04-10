// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_sdp.cpp
 * @brief SDP-based RFCOMM channel discovery.
 *
 * Implements helix_bt_sdp_find_rfcomm_channel() by querying the remote
 * device's SDP server for a service record matching the given UUID16, then
 * walking the protocol descriptor list to extract the RFCOMM channel.
 *
 * Used so we don't have to hardcode channel numbers per printer model —
 * the SPP profile on most label printers advertises the channel via SDP.
 */

#include "bluetooth_plugin.h"
#include "bt_context.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

extern "C" int helix_bt_sdp_find_rfcomm_channel(helix_bt_context* ctx, const char* mac,
                                                uint16_t uuid16, int* out_channel) {
    if (!ctx || !mac || !out_channel)
        return -EINVAL;

    bdaddr_t target;
    if (str2ba(mac, &target) < 0) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "invalid MAC address";
        return -EINVAL;
    }

    // NOTE: <bluetooth/bluetooth.h> defines BDADDR_ANY as a compound literal
    // (&(bdaddr_t){{0,0,0,0,0,0}}). g++ rejects taking the address of a
    // compound-literal rvalue outside -fpermissive, so we build a local
    // zero bdaddr_t instead. Value-init is cleaner than braced-array-of-zeros
    // and avoids -Wmany-braces-around-scalar-init on some libbluetooth layouts.
    bdaddr_t any_addr{};
    sdp_session_t* session = sdp_connect(&any_addr, &target, SDP_RETRY_IF_BUSY);
    if (!session) {
        int err = errno ? errno : EIO;
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = std::string("sdp_connect failed: ") + strerror(err);
        return -err;
    }

    // Build search (by service class UUID16) and attribute-range (all attrs) lists.
    uuid_t svc_uuid;
    sdp_uuid16_create(&svc_uuid, uuid16);
    sdp_list_t* search_list = sdp_list_append(nullptr, &svc_uuid);

    uint32_t attr_range = 0x0000ffff;
    sdp_list_t* attrid_list = sdp_list_append(nullptr, &attr_range);

    sdp_list_t* response_list = nullptr;
    int err = sdp_service_search_attr_req(session, search_list, SDP_ATTR_REQ_RANGE, attrid_list,
                                          &response_list);

    int channel = -1;
    if (err == 0 && response_list != nullptr) {
        for (sdp_list_t* r = response_list; r != nullptr && channel < 0; r = r->next) {
            sdp_record_t* rec = static_cast<sdp_record_t*>(r->data);
            sdp_list_t* proto_list = nullptr;
            if (sdp_get_access_protos(rec, &proto_list) == 0) {
                int ch = sdp_get_proto_port(proto_list, RFCOMM_UUID);
                if (ch > 0 && ch <= 30) {
                    channel = ch;
                }
                // sdp_get_access_protos returns a list whose elements are themselves
                // sdp_list_t* — free each inner list, then the outer list.
                for (sdp_list_t* p = proto_list; p != nullptr; p = p->next) {
                    sdp_list_free(static_cast<sdp_list_t*>(p->data), nullptr);
                }
                sdp_list_free(proto_list, nullptr);
            }
        }
    }

    // Free the response record list, releasing each sdp_record_t along the way.
    sdp_list_free(response_list, (sdp_free_func_t)sdp_record_free);
    sdp_list_free(search_list, nullptr);
    sdp_list_free(attrid_list, nullptr);
    sdp_close(session);

    if (channel < 0) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "no RFCOMM channel found in SDP record";
        return -ENOENT;
    }

    *out_channel = channel;
    fprintf(stderr, "[bt] SDP: %s uuid=0x%04x -> RFCOMM channel %d\n", mac, uuid16, channel);
    return 0;
}
