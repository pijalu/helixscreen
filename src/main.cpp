// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file main.cpp
 * @brief Application entry point
 *
 * This file is intentionally minimal. All application logic is implemented
 * in the Application class (src/application/application.cpp).
 *
 * @see Application
 */

#include "application.h"
#include "data_root_resolver.h"
#include "helix_version.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <unistd.h>

// execinfo.h: glibc Linux and macOS. Missing on Android NDK (bionic) and
// musl libc (Creality K1/K2).
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__))
#include <execinfo.h>
#define HELIX_HAS_BACKTRACE 1
#endif

// link.h / dl_iterate_phdr: glibc Linux only. macOS SDK doesn't ship link.h.
#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
#include <link.h>
#define HELIX_HAS_DL_ITERATE_PHDR 1
#endif

// SDL2 redefines main → SDL_main via this header.
// On Android, the SDL Java activity loads libmain.so and calls SDL_main().
// Without this include, the symbol is missing and the app crashes on launch.
#ifdef HELIX_PLATFORM_ANDROID
#include <SDL.h>
#endif

// Log to stderr using only async-signal-safe-ish functions.
// spdlog may not be initialized yet or may be in a broken state.
static void log_fatal(const char* msg) {
    fprintf(stderr, "[FATAL] %s\n", msg);
    fflush(stderr);
}

#ifdef HELIX_HAS_DL_ITERATE_PHDR
static int find_load_base_cb(struct dl_phdr_info* info, size_t /*size*/, void* data) {
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
        *static_cast<uintptr_t*>(data) = static_cast<uintptr_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}
#endif

// Write a minimal crash.txt for telemetry when an exception is caught.
// Uses the same key:value format as crash_handler's signal handler so
// CrashReporter can parse it on next startup.
static void write_exception_crash_file(const char* what) {
    const std::string crash_path = helix::writable_path("crash.txt");
    int fd = open(crash_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }

    // Build a crash record. Not a signal handler — heap/stdio/backtrace safe.
    time_t now = time(nullptr);
    dprintf(fd, "signal:0\n");
    dprintf(fd, "name:EXCEPTION\n");
    dprintf(fd, "version:%s\n", HELIX_VERSION);
    dprintf(fd, "timestamp:%ld\n", static_cast<long>(now));
    dprintf(fd, "uptime:0\n");
    if (what) {
        dprintf(fd, "exception:%s\n", what);
    }

#ifdef HELIX_HAS_DL_ITERATE_PHDR
    uintptr_t load_base = 0;
    dl_iterate_phdr(find_load_base_cb, &load_base);
    dprintf(fd, "load_base:0x%lx\n", static_cast<unsigned long>(load_base));
#endif

#ifdef HELIX_HAS_BACKTRACE
    void* frames[64];
    int n = backtrace(frames, 64);
    for (int i = 0; i < n; ++i) {
        dprintf(fd, "bt:0x%lx\n", reinterpret_cast<unsigned long>(frames[i]));
    }
#endif

    close(fd);
}

// Called by std::terminate() — covers uncaught exceptions, joinable thread
// destruction, and other fatal C++ runtime errors. Logs what we can before
// the default terminate handler calls abort() (which triggers crash_handler).
static void terminate_handler() {
    // Guard against re-entrance (e.g. exception::what() throws)
    static bool entered = false;
    if (entered) {
        abort();
    }
    entered = true;

    // Check if there's a current exception we can inspect
    const char* what = nullptr;
    if (auto eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            what = e.what();
            fprintf(stderr, "[FATAL] Uncaught exception: %s\n", what);
            fflush(stderr);
        } catch (...) {
            log_fatal("Uncaught non-std::exception");
            what = "non-std::exception";
        }
    } else {
        log_fatal("std::terminate() called without active exception "
                  "(joinable thread destroyed? noexcept violation?)");
        what = "std::terminate without active exception";
    }

    // Write crash file BEFORE abort — abort triggers the signal handler which
    // would overwrite it without the exception message.
    write_exception_crash_file(what);
    _exit(1);
}

int main(int argc, char** argv) {
    std::set_terminate(terminate_handler);

    try {
        Application app;
        return app.run(argc, argv);
    } catch (const std::exception& e) {
        fprintf(stderr, "[FATAL] Unhandled exception in Application: %s\n", e.what());
        fflush(stderr);
        write_exception_crash_file(e.what());
        return 1;
    } catch (...) {
        log_fatal("Unhandled non-std::exception in Application");
        write_exception_crash_file("non-std::exception");
        return 1;
    }
}
