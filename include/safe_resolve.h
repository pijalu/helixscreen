// SPDX-License-Identifier: GPL-3.0-or-later
//
// Safe hostname resolution that avoids glibc __check_pf() SIGSEGV on
// statically-linked ARM/MIPS builds (AD5M, SonicPad, K1).
// Uses libhv's direct UDP DNS resolver first, falls back to getaddrinfo().

#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <string>

#if defined(__GLIBC__) && (defined(__arm__) || defined(__mips__) || defined(__aarch64__))
extern "C" {
#include "base/dns_resolv.h"  // libhv's direct UDP DNS resolver
}
#endif

namespace helix {

/// Resolve hostname + port to a connected-ready sockaddr_in.
/// Returns 0 on success, non-zero on failure.
inline int safe_resolve(const std::string& host, int port, struct sockaddr_in& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    // Try numeric address first (no DNS needed)
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) {
        return 0;
    }

#if defined(__GLIBC__) && (defined(__arm__) || defined(__mips__) || defined(__aarch64__))
    // On statically-linked embedded glibc, getaddrinfo() can SIGSEGV inside
    // __check_pf(). Use direct UDP DNS resolution to avoid the crash.
    {
        struct in_addr resolved;
        if (dns_resolv_resolve(host.c_str(), &resolved) == 0) {
            addr.sin_addr = resolved;
            return 0;
        }
    }
#endif

    // Standard resolution (safe on desktop/Pi, fallback on embedded)
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (err != 0 || !result) {
        return err ? err : -1;
    }
    auto* sin = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    addr.sin_addr = sin->sin_addr;
    freeaddrinfo(result);
    return 0;
}

} // namespace helix
