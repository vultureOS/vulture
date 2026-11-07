/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Accessibility.h>
#include <LibUI/Platform.h>
#include <LibUI/View.h>
#include <LibUI/Window.h>

namespace UI::Accessibility {

StringView name_of(Role role)
{
    switch (role) {
    case Role::Group:
        return "group"_sv;
    case Role::Window:
        return "window"_sv;
    case Role::Button:
        return "button"_sv;
    case Role::Checkbox:
        return "checkbox"_sv;
    case Role::RadioButton:
        return "radio"_sv;
    case Role::Switch:
        return "switch"_sv;
    case Role::SegmentedControl:
        return "segmented"_sv;
    case Role::PopUpButton:
        return "popup"_sv;
    case Role::Slider:
        return "slider"_sv;
    case Role::Stepper:
        return "stepper"_sv;
    case Role::TextField:
        return "textfield"_sv;
    case Role::TextArea:
        return "textarea"_sv;
    case Role::StaticText:
        return "text"_sv;
    case Role::ProgressIndicator:
        return "progress"_sv;
    case Role::ColorWell:
        return "colorwell"_sv;
    case Role::Image:
        return "image"_sv;
    case Role::ScrollArea:
        return "scrollarea"_sv;
    case Role::Table:
        return "table"_sv;
    case Role::Outline:
        return "outline"_sv;
    case Role::Row:
        return "row"_sv;
    case Role::Tab:
        return "tab"_sv;
    case Role::TabGroup:
        return "tabgroup"_sv;
    case Role::Toolbar:
        return "toolbar"_sv;
    case Role::Menu:
        return "menu"_sv;
    case Role::MenuItem:
        return "menuitem"_sv;
    case Role::SplitGroup:
        return "splitgroup"_sv;
    case Role::Sheet:
        return "sheet"_sv;
    case Role::Cell:
        return "cell"_sv;
    }
    VERIFY_NOT_REACHED();
}

namespace {

void collect(View& view, Vector<Node>& into)
{
    if (view.is_hidden() || !view.has_room_to_be_seen())
        return;
    auto label = view.accessibility_label();
    auto role = view.accessibility_role();
    Vector<Node> children;
    for (auto const& child : view.children())
        collect(*child, children);
    view.accessibility_parts(children);
    if (role == Role::Group && label.is_empty()) {
        for (auto& child : children)
            into.append(move(child));
        return;
    }
    Node node;
    node.role = role;
    node.label = move(label);
    node.value = view.accessibility_value();
    node.frame = view.rect_in_window();
    node.enabled = view.is_enabled();
    node.focused = view.is_first_responder();
    node.view = &view;
    node.children = move(children);
    into.append(move(node));
}

void describe_into(Node const& node, StringBuilder& builder, size_t depth)
{
    for (size_t i = 0; i < depth; ++i)
        builder.append("  "_sv);
    builder.append(name_of(node.role));
    if (!node.label.is_empty()) {
        builder.append(" \""_sv);
        builder.append(node.label.view());
        builder.append("\""_sv);
    }
    if (!node.value.is_empty()) {
        builder.append(" = "_sv);
        builder.append(node.value.view());
    }
    if (!node.enabled)
        builder.append(" (disabled)"_sv);
    if (node.focused)
        builder.append(" (focused)"_sv);
    builder.append("\n"_sv);
    for (auto const& child : node.children)
        describe_into(child, builder, depth + 1);
}

}

Node part_of(View const& view, Role role, StringView label, Gfx::FloatRect frame, StringView value, bool enabled)
{
    auto const origin = view.rect_in_window();
    Node node;
    node.role = role;
    node.label = MUST(String::from_view(label));
    node.value = MUST(String::from_view(value));
    node.frame = { origin.x + frame.x, origin.y + frame.y, frame.width, frame.height };
    node.enabled = enabled;
    return node;
}

Node tree_of(View& view)
{
    Vector<Node> found;
    collect(view, found);
    if (found.size() == 1)
        return move(found[0]);
    Node group;
    group.frame = view.rect_in_window();
    group.view = &view;
    group.children = move(found);
    return group;
}

Node tree_of(Window& window)
{
    Node node;
    node.role = Role::Window;
    node.label = MUST(String::from_view(window.title()));
    node.view = &window.root_view();
    node.frame = window.root_view().frame();
    Vector<Node> found;
    collect(window.root_view(), found);
    // A sheet is in front of the rest of its window, and what's asked of now: it comes first.
    Vector<Node> ordered;
    for (auto& child : found) {
        if (child.role == Role::Sheet)
            ordered.append(move(child));
    }
    for (auto& child : found) {
        if (child.role != Role::Sheet)
            ordered.append(move(child));
    }
    node.children = move(ordered);
    return node;
}

String describe(Node const& node)
{
    StringBuilder builder;
    describe_into(node, builder, 0);
    return MUST(builder.to_string());
}

ErrorOr<JSON::Value> to_json(Node const& node)
{
    auto object = JSON::Value::object();
    TRY(object.set("role"_sv, TRY(JSON::Value::string(name_of(node.role)))));
    TRY(object.set("label"_sv, TRY(JSON::Value::string(node.label.view()))));
    TRY(object.set("value"_sv, TRY(JSON::Value::string(node.value.view()))));
    TRY(object.set("x"_sv, JSON::Value(static_cast<double>(node.frame.x))));
    TRY(object.set("y"_sv, JSON::Value(static_cast<double>(node.frame.y))));
    TRY(object.set("width"_sv, JSON::Value(static_cast<double>(node.frame.width))));
    TRY(object.set("height"_sv, JSON::Value(static_cast<double>(node.frame.height))));
    TRY(object.set("enabled"_sv, JSON::Value(node.enabled)));
    TRY(object.set("focused"_sv, JSON::Value(node.focused)));
    auto children = JSON::Value::array();
    for (auto const& child : node.children)
        TRY(children.append(TRY(to_json(child))));
    TRY(object.set("children"_sv, move(children)));
    return object;
}

ErrorOr<String> describe_windows(Span<Window* const> windows)
{
    auto list = JSON::Value::array();
    for (auto* window : windows) {
        if (!window->is_shown())
            continue;
        auto one = JSON::Value::object();
        TRY(one.set("window"_sv, JSON::Value(static_cast<i64>(window->backend().system_id()))));
        TRY(one.set("tree"_sv, TRY(to_json(tree_of(*window)))));
        TRY(list.append(move(one)));
    }
    return list.serialize();
}

Node const* find(Node const& node, Role role, StringView label)
{
    if (node.role == role && node.label.view() == label)
        return &node;
    for (auto const& child : node.children) {
        if (auto const* found = find(child, role, label))
            return found;
    }
    return nullptr;
}

}
