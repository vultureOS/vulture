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
    // Though bare, it may be active and take the keyboard: Search, Launcher, and the
    // desktop's own window.
    WindowCanBecomeKey = 1 << 6,
    // On every space.
    WindowSticky = 1 << 7,
};

// Where a window sits among others (set_level): higher is in front. Popups start
// at Popup; the rest, at Normal.
enum WindowLevel : i32 {
    LevelDesktop = -100,
    // What sits on the desktop, over its icons: widgets. Part of the desktop, all the same.
    LevelDesktopItems = -90,
    LevelNormal = 0,
    LevelDock = 50,
    LevelMenuBar = 60,
    LevelPopup = 100,
    LevelDragImage = 200,
    // The login window and the lock screen: over everything a session has (CGShieldingWindowLevel's place).
    LevelShield = 1000,
    LevelScreenSaver = 1010,
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
    // Over a program that isn't answering. It's the server's to choose: asking for it does nothing.
    Wait,
    Count,
};

// Where a window can be put, in whatever's free of the display it's on.
enum class WindowArrangement : i32 {
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Fill,
    Center,
    // As it was before any of those.
    Previous,
    // This window and the next ones under it, together: the first named is this one's place.
    LeftAndRight,
    RightAndLeft,
    TopAndBottom,
    BottomAndTop,
    Quarters,
    LeftAndQuarters,
    RightAndQuarters,
    TopAndQuarters,
    BottomAndQuarters,
    Count,
};

// What the pointer staying in a corner of a display does.
enum class HotCornerAction : i32 {
    None,
    Overview,
    ApplicationWindows,
    Desktop,
    Launcher,
    NotificationCenter,
    StartScreenSaver,
    LockScreen,
    Count,
};

// A display, as `displays` says: the main one first, at 0,0, and the others around it. Points.
struct DisplayRecord {
    i32 output { 0 };
    i32 x { 0 };
    i32 y { 0 };
    i32 width { 0 };
    i32 height { 0 };
    // What it really has: two displays' worth of "looks like" can come to the same points.
    i32 panel_width { 0 };
    i32 panel_height { 0 };
};

// What a layer has. Each is changed by one LayerChange in a commit_layers. Those marked can be animated.
enum class LayerProperty : u32 {
    Frame,          // Animated. x, y, width, height: points, among what it's in.
    Translation,    // Animated. x, y: points, on top of that.
    Scale,          // Animated. x, y: about its middle.
    Rotation,       // Animated. Radians, about its middle.
    Opacity,        // Animated. 0 to 1, of all of it, with what's in it.
    CornerRadius,   // Animated. Points.
    BoundsOrigin,   // Animated. x, y: points: what's at its top left, as something scrolled.
    ContentsOrigin, // x, y: points: where its surface's top left goes, among what it holds.
    Background,     // Red, green, blue, alpha: 0 to 1, not premultiplied.
    Hidden,         // 0 or 1.
    Clips,          // 0 or 1: what's in it stops at its edges.
    ClipRect,       // x, y, width, height: points, among what it's in; none if the width is negative.
    Material,       // A Material, behind it; -1 for none.
    Blur,           // Points: of what it shows.
    Logged,         // 0 or 1: see layer_frame_log.
    Present,        // Buffer, then x, y, width, height: this buffer of its surface is now the one shown, and that part changed.
    PresentWindow,  // The same, of the window's own surface. (`layer` is 0.)
    Order,          // Its place among those it's with, from the bottom.
    Animate,        // See below.
    StopAnimation,  // `detail` is the property: it stays where it has got to.
    Destroy,        // It's gone, with all that's in it.
    Count,
};

enum class LayerCurve : u8 {
    Linear,
    Bezier,
    Spring,
};

// One change. For an Animate, `detail` says what to animate in its low byte, the LayerCurve in the next, in bit 16 whether it
// goes round for ever, and in the top byte how many steps it goes in (0 for smoothly). `values` are 4 where it starts, 4 where it
// ends, the duration in seconds, and the curve's: x1, y1, x2, y2 for a bezier, or natural frequency, damping ratio and starting
// velocity for a spring. It hears `layer_animation_finished` with `tag`, if that's not 0.
struct LayerChange {
    u32 layer { 0 };
    LayerProperty property { LayerProperty::Frame };
    u32 detail { 0 };
    u32 tag { 0 };
    float values[13] {};
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
    // As Wheel, but in points rather than notches: fingers.
    Scroll,
    // Fingers going apart or together: wheel_x is how much bigger, in thousandths (1000 is the same).
    Magnify,
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
    // Not a modifier that's held: this press is the key repeating by itself.
    ModifierRepeat = 1 << 5,
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
