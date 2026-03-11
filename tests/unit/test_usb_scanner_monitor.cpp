// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_scanner_monitor.h"

#include "../catch_amalgamated.hpp"

// Define evdev keycodes for test builds (values match linux/input-event-codes.h)
#ifndef KEY_1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6
#define KEY_6 7
#define KEY_7 8
#define KEY_8 9
#define KEY_9 10
#define KEY_0 11
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_Q 16
#define KEY_W 17
#define KEY_E 18
#define KEY_R 19
#define KEY_T 20
#define KEY_Y 21
#define KEY_U 22
#define KEY_I 23
#define KEY_O 24
#define KEY_P 25
#define KEY_A 30
#define KEY_S 31
#define KEY_D 32
#define KEY_F 33
#define KEY_G 34
#define KEY_H 35
#define KEY_J 36
#define KEY_K 37
#define KEY_L 38
#define KEY_SEMICOLON 39
#define KEY_Z 44
#define KEY_X 45
#define KEY_C 46
#define KEY_V 47
#define KEY_B 48
#define KEY_N 49
#define KEY_M 50
#define KEY_COMMA 51
#define KEY_DOT 52
#define KEY_SLASH 53
#define KEY_LEFTSHIFT 42
#define KEY_LEFTCTRL 29
#define KEY_ENTER 28
#endif

using helix::UsbScannerMonitor;

TEST_CASE("keycode_to_char: digits KEY_0..KEY_9", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_1, false) == '1');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_2, false) == '2');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_3, false) == '3');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_4, false) == '4');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_5, false) == '5');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_6, false) == '6');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_7, false) == '7');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_8, false) == '8');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_9, false) == '9');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_0, false) == '0');
}

TEST_CASE("keycode_to_char: letters KEY_A..KEY_Z lowercase", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_A, false) == 'a');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_B, false) == 'b');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_C, false) == 'c');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_Z, false) == 'z');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_M, false) == 'm');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_Q, false) == 'q');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_P, false) == 'p');
}

TEST_CASE("keycode_to_char: letters with shift produce uppercase", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_A, true) == 'A');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_Z, true) == 'Z');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_M, true) == 'M');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_S, true) == 'S');
}

TEST_CASE("keycode_to_char: special keys", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_MINUS, false) == '-');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_DOT, false) == '.');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_SLASH, false) == '/');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_EQUAL, false) == '=');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_COMMA, false) == ',');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_SEMICOLON, false) == ';');
}

TEST_CASE("keycode_to_char: shift special keys", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_SEMICOLON, true) == ':');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_MINUS, true) == '_');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_EQUAL, true) == '+');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_SLASH, true) == '?');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_DOT, true) == '>');
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_COMMA, true) == '<');
}

TEST_CASE("keycode_to_char: unmapped key returns 0", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_LEFTCTRL, false) == 0);
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_LEFTSHIFT, false) == 0);
    CHECK(UsbScannerMonitor::keycode_to_char(KEY_ENTER, false) == 0);
    CHECK(UsbScannerMonitor::keycode_to_char(999, false) == 0);
}

TEST_CASE("check_spoolman_pattern: web+spoolman format", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::check_spoolman_pattern("web+spoolman:s-42") == 42);
    CHECK(UsbScannerMonitor::check_spoolman_pattern("web+spoolman:s-1") == 1);
    CHECK(UsbScannerMonitor::check_spoolman_pattern("web+spoolman:s-999") == 999);
}

TEST_CASE("check_spoolman_pattern: SM:SPOOL format", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::check_spoolman_pattern("SM:SPOOL=123") == 123);
    CHECK(UsbScannerMonitor::check_spoolman_pattern("SM:SPOOL=1") == 1);
}

TEST_CASE("check_spoolman_pattern: URL format", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::check_spoolman_pattern("https://spoolman.local/view/spool/7") == 7);
    CHECK(UsbScannerMonitor::check_spoolman_pattern("http://localhost:7912/spool/42") == 42);
    CHECK(UsbScannerMonitor::check_spoolman_pattern("https://example.com/spool/100/") == 100);
}

TEST_CASE("check_spoolman_pattern: invalid input returns -1", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::check_spoolman_pattern("random text") == -1);
    CHECK(UsbScannerMonitor::check_spoolman_pattern("not a qr code") == -1);
    CHECK(UsbScannerMonitor::check_spoolman_pattern("12345") == -1);
}

TEST_CASE("check_spoolman_pattern: empty input returns -1", "[usb_scanner]")
{
    CHECK(UsbScannerMonitor::check_spoolman_pattern("") == -1);
}

TEST_CASE("UsbScannerMonitor: construct and destroy without starting", "[usb_scanner]")
{
    // Should not crash
    UsbScannerMonitor monitor;
    CHECK_FALSE(monitor.is_running());
}
