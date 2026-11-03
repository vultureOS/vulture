/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibBase/Span.h>
#include <LibGfx/Geometry.h>
#include <LibJSON/JSON.h>

// What is on screen, as a tree of roles and names: for screen readers, and for tests
// that find things by what they say rather than by where they are.

namespace UI {

class View;
class Window;

namespace Accessibility {

enum class Role : u8 {
    Group,
    Window,
    Button,
    Checkbox,
    RadioButton,
    Switch,
    SegmentedControl,
    PopUpButton,
    Slider,
    Stepper,
    TextField,
    TextArea,
    StaticText,
    ProgressIndicator,
    ColorWell,
    Image,
    ScrollArea,
    Table,
    Outline,
    Row,
    Tab,
    TabGroup,
    Toolbar,
    Menu,
    MenuItem,
    SplitGroup,
    Sheet,
    Cell,
};

[[nodiscard]] StringView name_of(Role);

struct Node {
    Role role { Role::Group };
    String label;
    String value;
    Gfx::FloatRect frame;
    bool enabled { true };
    bool focused { false };
    View* view { nullptr };
    Vector<Node> children;
};

// A part of `view` that isn't a view of its own, as one, for accessibility_parts. `frame` is where the view draws it.
[[nodiscard]] Node part_of(View const& view, Role, StringView label, Gfx::FloatRect frame, StringView value = {}, bool enabled = true);

// Plain groups with no name are left out; their children move up.
[[nodiscard]] Node tree_of(Window&);
[[nodiscard]] Node tree_of(View&);
// As lines of text, indented: what a test or a screen reader would walk.
[[nodiscard]] String describe(Node const&);
// The first with this role and label, in reading order.
[[nodiscard]] Node const* find(Node const&, Role, StringView label);
// As JSON, for what isn't this program: "role", "label", "value", "x", "y", "width", "height" (points, from the window's
// top-left), "enabled", "focused" and "children".
[[nodiscard]] ErrorOr<JSON::Value> to_json(Node const&);
// All of a program's shown windows, as a JSON array of { "window": what the system knows it by, "tree": its tree }.
[[nodiscard]] ErrorOr<String> describe_windows(Span<Window* const>);

}

}
