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
    // Above ordinary windows, and never active: menus, popovers, tool tips. Its
    // owner gets no keyboard through it; it keeps the keys of its active window.
    WindowPopup = 1 << 4,
    // A title bar the client draws itself, as a toolbar does: the server adds only
    // the traffic lights, over the client's pixels, and hands presses on to it (see
    // begin_move, for those that should move the window).
    WindowUnifiedTitle = 1 << 5,
    // Though bare, it may be active and take the keyboard: Spotlight, Launchpad, and the
    // desktop's own window.
    WindowCanBecomeKey = 1 << 6,
    // On every space.
    WindowSticky = 1 << 7,
};

// Where a window sits among others (set_level): higher is in front. Popups start
// at Popup; the rest, at Normal.
enum WindowLevel : i32 {
    LevelDesktop = -100,
    LevelNormal = 0,
    LevelDock = 50,
    LevelMenuBar = 60,
    LevelPopup = 100,
    LevelDragImage = 200,
};

enum class CursorShape : i32 {
    Arrow,
    IBeam,
    PointingHand,
    ResizeLeftRight,
    ResizeUpDown,
    ResizeDiagonalDown,
    ResizeDiagonalUp,
    OpenHand,
    ClosedHand,
    Crosshair,
    Count,
};

enum class DragPhase : u8 {
    Entered,
    Moved,
    Left,
    Dropped,
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
