/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// /dev/input: every keyboard and pointer, as a stream of events. One process
// at a time may have it open; while nobody does, the keyboard types at the
// console. read() gives whole events, or EAGAIN when there are none (poll()
// says when there are).

struct vulture_input_event {
    unsigned short type;
    unsigned short code;
    int value;
    unsigned long long time; // Microseconds since boot.
};

#define VULTURE_EV_SYN 0x00 // The events so far go together.
#define VULTURE_EV_KEY 0x01 // value: 1 pressed, 0 released, 2 repeating.
#define VULTURE_EV_REL 0x02
#define VULTURE_EV_ABS 0x03

// Pointers report positions from 0 to this, across the whole screen.
#define VULTURE_INPUT_ABSOLUTE_MAXIMUM 32767
#define VULTURE_ABS_X 0x00
#define VULTURE_ABS_Y 0x01
#define VULTURE_REL_X 0x00
#define VULTURE_REL_Y 0x01
#define VULTURE_REL_HWHEEL 0x06
#define VULTURE_REL_WHEEL 0x08

#define VULTURE_BTN_LEFT 0x110
#define VULTURE_BTN_RIGHT 0x111
#define VULTURE_BTN_MIDDLE 0x112

// Key codes, by position on a US keyboard.
#define VULTURE_KEY_ESC 1
#define VULTURE_KEY_1 2
#define VULTURE_KEY_0 11
#define VULTURE_KEY_MINUS 12
#define VULTURE_KEY_EQUAL 13
#define VULTURE_KEY_BACKSPACE 14
#define VULTURE_KEY_TAB 15
#define VULTURE_KEY_Q 16
#define VULTURE_KEY_P 25
#define VULTURE_KEY_LEFTBRACE 26
#define VULTURE_KEY_RIGHTBRACE 27
#define VULTURE_KEY_ENTER 28
#define VULTURE_KEY_LEFTCTRL 29
#define VULTURE_KEY_A 30
#define VULTURE_KEY_L 38
#define VULTURE_KEY_SEMICOLON 39
#define VULTURE_KEY_APOSTROPHE 40
#define VULTURE_KEY_GRAVE 41
#define VULTURE_KEY_LEFTSHIFT 42
#define VULTURE_KEY_BACKSLASH 43
#define VULTURE_KEY_Z 44
#define VULTURE_KEY_M 50
#define VULTURE_KEY_COMMA 51
#define VULTURE_KEY_DOT 52
#define VULTURE_KEY_SLASH 53
#define VULTURE_KEY_RIGHTSHIFT 54
#define VULTURE_KEY_KPASTERISK 55
#define VULTURE_KEY_LEFTALT 56
#define VULTURE_KEY_SPACE 57
#define VULTURE_KEY_CAPSLOCK 58
#define VULTURE_KEY_F1 59
#define VULTURE_KEY_F10 68
#define VULTURE_KEY_KP7 71
#define VULTURE_KEY_KPDOT 83
#define VULTURE_KEY_F11 87
#define VULTURE_KEY_F12 88
#define VULTURE_KEY_KPENTER 96
#define VULTURE_KEY_RIGHTCTRL 97
#define VULTURE_KEY_KPSLASH 98
#define VULTURE_KEY_RIGHTALT 100
#define VULTURE_KEY_HOME 102
#define VULTURE_KEY_UP 103
#define VULTURE_KEY_PAGEUP 104
#define VULTURE_KEY_LEFT 105
#define VULTURE_KEY_RIGHT 106
#define VULTURE_KEY_END 107
#define VULTURE_KEY_DOWN 108
#define VULTURE_KEY_PAGEDOWN 109
#define VULTURE_KEY_INSERT 110
#define VULTURE_KEY_DELETE 111
#define VULTURE_KEY_LEFTMETA 125
#define VULTURE_KEY_RIGHTMETA 126
