/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// What PranaWindowServer and its clients agree on, beyond the endpoints.

namespace Window {

// create_window's flags.
enum WindowFlags : u32 {
    // A title bar and traffic lights, drawn by the server over the client's
    // pixels. The frame includes them.
    WindowTitled = 1 << 0,
    // The window's edges drag to resize it.
    WindowResizable = 1 << 1,
    // Not shown until show().
    WindowHidden = 1 << 2,
    // No corners, shadow or chrome: a bare surface (the menu bar, the Dock).
    WindowBare = 1 << 3,
};

enum class MouseKind : u8 {
    Moved,
    Pressed,
    Released,
    Wheel,
    Entered,
    Left,
};

enum MouseButtons : u32 {
    ButtonLeft = 1 << 0,
    ButtonRight = 1 << 1,
    ButtonMiddle = 1 << 2,
};

enum Modifiers : u32 {
    ModifierShift = 1 << 0,
    ModifierControl = 1 << 1,
    ModifierOption = 1 << 2,
    ModifierCommand = 1 << 3,
    ModifierCapsLock = 1 << 4,
};

// The materials a region can have (their looks are in the theme).
enum class Material : i32 {
    MenuBar,
    Popover,
    Sidebar,
    HUD,
    UnderWindow,
    Count,
};

}
