/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/Menu.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TextField.h>
#include <LibUI/TextView.h>

// Saying what a window holds, as a value: nested calls in the shape of the result.
//
//     using namespace UI::Build;
//     window.content_view().adopt_child(vstack(8,
//         label("Name:"),
//         field("Ada").bind(name_field).on_commit([&] { ... }),
//         hstack(8, spacer(), button("Cancel"), default_button("OK").action([&] { ... }))));
//
// Nothing here that cannot be done with the classes themselves: it only reads better.

namespace UI::Build {

// A made view, until it is given to a parent. Any setter can be reached with .with().
template<typename T>
class Node {
public:
    explicit Node(T* view)
        : m_view(view)
    {
    }
    Node(Node const&) = delete;
    Node(Node&& other)
        : m_view(exchange(other.m_view, nullptr))
    {
    }
    ~Node() { delete m_view; }

    // Keeps a pointer to it, to use once it is in place.
    Node&& bind(T*& pointer) &&
    {
        pointer = m_view;
        return move(*this);
    }
    template<typename Configure>
    Node&& with(Configure configure) &&
    {
        configure(*m_view);
        return move(*this);
    }
    Node&& action(Function<void()> callback) &&
    {
        m_view->on_action = move(callback);
        return move(*this);
    }
    Node&& on_commit(Function<void()> callback) &&
    {
        m_view->on_commit = move(callback);
        return move(*this);
    }
    Node&& on_change(Function<void()> callback) &&
    {
        m_view->on_change = move(callback);
        return move(*this);
    }
    Node&& size(ControlSize control_size) &&
    {
        m_view->set_control_size(control_size);
        return move(*this);
    }
    Node&& width(float value) &&
    {
        m_view->set_fixed_width(value);
        return move(*this);
    }
    Node&& height(float value) &&
    {
        m_view->set_fixed_height(value);
        return move(*this);
    }
    Node&& stretch(float horizontal = 1, float vertical = 0) &&
    {
        m_view->set_stretch(horizontal, vertical);
        return move(*this);
    }
    Node&& disabled(bool is_disabled = true) &&
    {
        m_view->set_enabled(!is_disabled);
        return move(*this);
    }
    Node&& accessibility(StringView text) &&
    {
        m_view->set_accessibility_label(text);
        return move(*this);
    }

    [[nodiscard]] NonnullOwnPtr<View> release()
    {
        return adopt_own(*static_cast<View*>(exchange(m_view, nullptr)));
    }
    operator NonnullOwnPtr<View>() && { return release(); }

private:
    T* m_view;
};

namespace Detail {

template<typename Stack>
void add_children(Stack&) { }

template<typename Stack, typename First, typename... Rest>
void add_children(Stack& stack, First&& first, Rest&&... rest)
{
    stack.adopt_child(forward<First>(first).release());
    add_children(stack, forward<Rest>(rest)...);
}

}

template<typename... Children>
[[nodiscard]] Node<StackView> vstack(float spacing, Children&&... children)
{
    auto* stack = new StackView(Axis::Vertical, spacing);
    Detail::add_children(*stack, forward<Children>(children)...);
    return Node<StackView>(stack);
}

template<typename... Children>
[[nodiscard]] Node<StackView> hstack(float spacing, Children&&... children)
{
    auto* stack = new StackView(Axis::Horizontal, spacing, Alignment::Center);
    Detail::add_children(*stack, forward<Children>(children)...);
    return Node<StackView>(stack);
}

[[nodiscard]] inline Node<View> spacer()
{
    auto* view = new View;
    view->set_stretch(1, 1);
    return Node<View>(view);
}

[[nodiscard]] inline Node<Label> label(StringView text, StringView font = "body"_sv)
{
    return Node<Label>(new Label(text, Appearance::the().named_font(font)));
}

[[nodiscard]] inline Node<Button> button(StringView title)
{
    return Node<Button>(new Button(title));
}

[[nodiscard]] inline Node<Button> default_button(StringView title)
{
    return Node<Button>(new Button(title, ButtonStyle::Default));
}

[[nodiscard]] inline Node<Checkbox> checkbox(StringView title, bool on = false)
{
    return Node<Checkbox>(new Checkbox(title, on ? ToggleState::On : ToggleState::Off));
}

[[nodiscard]] inline Node<RadioButton> radio(StringView title, bool on = false, int group = 0)
{
    return Node<RadioButton>(new RadioButton(title, on, group));
}

[[nodiscard]] inline Node<Switch> toggle(bool on = false)
{
    return Node<Switch>(new Switch(on));
}

[[nodiscard]] inline Node<Slider> slider(float minimum = 0, float maximum = 1, float value = 0)
{
    return Node<Slider>(new Slider(minimum, maximum, value));
}

[[nodiscard]] inline Node<Stepper> stepper(int minimum = 0, int maximum = 100, int value = 0)
{
    return Node<Stepper>(new Stepper(minimum, maximum, value));
}

[[nodiscard]] inline Node<TextField> field(StringView text = {}, StringView placeholder = {})
{
    auto* control = new TextField(text);
    control->set_placeholder(placeholder);
    return Node<TextField>(control);
}

[[nodiscard]] inline Node<SearchField> search()
{
    return Node<SearchField>(new SearchField);
}

[[nodiscard]] inline Node<ProgressBar> progress(float value = 0)
{
    return Node<ProgressBar>(new ProgressBar(value));
}

[[nodiscard]] inline Node<Separator> separator(bool vertical = false)
{
    return Node<Separator>(new Separator(vertical));
}

[[nodiscard]] inline Node<SegmentedControl> segments(Span<StringView const> titles, size_t selected = 0)
{
    auto* control = new SegmentedControl;
    for (auto title : titles)
        control->add_segment(title);
    control->set_selected(selected);
    return Node<SegmentedControl>(control);
}

[[nodiscard]] inline Node<PopUpButton> popup(Span<StringView const> titles, int selected = 0)
{
    auto* control = new PopUpButton;
    for (auto title : titles)
        control->add_item(title);
    control->set_selected_index(selected);
    return Node<PopUpButton>(control);
}

template<typename Child>
[[nodiscard]] Node<ScrollView> scroll(Child&& child)
{
    auto* view = new ScrollView;
    view->set_document(forward<Child>(child).release());
    return Node<ScrollView>(view);
}

template<typename... Children>
[[nodiscard]] Node<Box> box(StringView title, Children&&... children)
{
    auto* view = new Box(title);
    auto& content = view->content();
    content.set_layout<StackLayout>(Axis::Vertical, 8);
    Detail::add_children(content, forward<Children>(children)...);
    return Node<Box>(view);
}

}
