/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/StringView.h>
#include <LibGfx/Geometry.h>
#include <LibWindow/Protocol.h>

namespace UI {

// What the window server and its clients agree on (and a class of ours has its name).
namespace WS = ::Window;

struct MouseEvent {
    // In the view that gets it, in points.
    Gfx::FloatPoint position;
    // In the window.
    Gfx::FloatPoint window_position;
    // Which are down now: see Window::MouseButtons.
    u32 buttons { 0 };
    // Which one this press or release is about.
    u32 button { 0 };
    u32 modifiers { 0 };
    // 2 for the second press of a double click, and so on.
    int click_count { 1 };

    [[nodiscard]] bool is_right_click() const { return button == ::Window::ButtonRight || (button == ::Window::ButtonLeft && (modifiers & ::Window::ModifierControl)); }
};

struct ScrollEvent {
    Gfx::FloatPoint position;
    // In points: positive is toward the bottom and the right of the content.
    float delta_x { 0 };
    float delta_y { 0 };
    u32 modifiers { 0 };
};

struct KeyEvent {
    u16 code { 0 };
    u32 modifiers { 0 };
    // What it types, in UTF-8: empty for keys that type nothing.
    StringView text;
    bool repeat { false };

    [[nodiscard]] bool has_command() const { return modifiers & ::Window::ModifierCommand; }
    [[nodiscard]] bool has_shift() const { return modifiers & ::Window::ModifierShift; }
    [[nodiscard]] bool has_option() const { return modifiers & ::Window::ModifierOption; }
    [[nodiscard]] bool has_control() const { return modifiers & ::Window::ModifierControl; }
};

// Key codes, as the keyboard sends them (<linux/input.h>'s numbers, which is what
// /dev/input gives).
enum Key : u16 {
    KeyEscape = 1,
    Key1 = 2,
    KeyMinus = 12,
    KeyEqual = 13,
    KeyBackspace = 14,
    KeyTab = 15,
    KeyQ = 16,
    KeyW = 17,
    KeyE = 18,
    KeyR = 19,
    KeyT = 20,
    KeyY = 21,
    KeyU = 22,
    KeyI = 23,
    KeyO = 24,
    KeyP = 25,
    KeyEnter = 28,
    KeyA = 30,
    KeyS = 31,
    KeyD = 32,
    KeyF = 33,
    KeyG = 34,
    KeyH = 35,
    KeyJ = 36,
    KeyK = 37,
    KeyL = 38,
    KeyZ = 44,
    KeyX = 45,
    KeyC = 46,
    KeyV = 47,
    KeyB = 48,
    KeyN = 49,
    KeyM = 50,
    KeySpace = 57,
    KeyHome = 102,
    KeyUp = 103,
    KeyPageUp = 104,
    KeyLeft = 105,
    KeyRight = 106,
    KeyEnd = 107,
    KeyDown = 108,
    KeyPageDown = 109,
    KeyDelete = 111,
};

}
