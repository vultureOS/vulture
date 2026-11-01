/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/Accessibility.h>
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
    }
    VERIFY_NOT_REACHED();
}

namespace {

void collect(View& view, Vector<Node>& into)
{
    if (view.is_hidden())
        return;
    auto label = view.accessibility_label();
    auto role = view.accessibility_role();
    Vector<Node> children;
    for (auto const& child : view.children())
        collect(*child, children);
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
    node.children = move(found);
    return node;
}

String describe(Node const& node)
{
    StringBuilder builder;
    describe_into(node, builder, 0);
    return MUST(builder.to_string());
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
