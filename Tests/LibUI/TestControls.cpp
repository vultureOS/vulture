/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestPlatform.h"
#include <LibTest/TestCase.h>
#include <LibUI/Builder.h>
#include <LibUI/Containers.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/Menu.h>
#include <LibUI/Motion.h>
#include <LibUI/Panels.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TableView.h>
#include <LibUI/TextField.h>
#include <LibUI/TextView.h>

using namespace UI;
using namespace Gfx;

namespace {

// A window of the toolkit, active, with nothing moving on its own.
struct TestWindow {
    explicit TestWindow(int width = 600, int height = 400, int scale = 2, StringView appearance = "Light"_sv)
    {
        TestPlatform::the().set_scale(scale);
        set_motion_enabled(false);
        MUST(Appearance::use(appearance));
        window = adopt_own(*new UI::Window("Test"_sv, { 0, 0, width, height }, WS::WindowTitled | WS::WindowResizable));
        window->show();
        window->handle_activation(true);
    }

    View& content() { return window->content_view(); }

    // Views' own frames are given by the test: a place to put them.
    template<typename T, typename... Args>
    T& place(FloatRect frame, Args&&... arguments)
    {
        auto& view = window->root_view().add<T>(forward<Args>(arguments)...);
        view.set_frame(frame);
        return view;
    }

    void settle()
    {
        window->update_now();
        TestPlatform::the().run_deferred();
    }

    void move_to(float x, float y, u32 buttons = 0) { window->handle_mouse(WS::MouseKind::Moved, { x, y }, buttons, 0, 0, 0); }

    void press(float x, float y, u32 modifiers = 0)
    {
        move_to(x, y);
        window->handle_mouse(WS::MouseKind::Pressed, { x, y }, WS::ButtonLeft, modifiers, 0, 0);
    }

    void release(float x, float y, u32 modifiers = 0) { window->handle_mouse(WS::MouseKind::Released, { x, y }, 0, modifiers, 0, 0); }

    void click(float x, float y, u32 modifiers = 0)
    {
        press(x, y, modifiers);
        release(x, y, modifiers);
    }

    void key(u16 code, u32 modifiers = 0, StringView text = {})
    {
        window->handle_key(true, code, modifiers, text);
        window->handle_key(false, code, modifiers, {});
    }

    void type(StringView text)
    {
        for (size_t i = 0; i < text.length(); ++i)
            key(KeySpace + 1000, 0, text.substring_view(i, 1));
    }

    void wheel(float x, float y, int notches_y) { window->handle_mouse(WS::MouseKind::Wheel, { x, y }, 0, 0, 0, notches_y); }

    OwnPtr<UI::Window> window;
};

bool same(StringView a, StringView b)
{
    return a == b;
}

}

TEST_CASE(a_button_sizes_itself_around_its_title)
{
    TestWindow test;
    auto& button = test.place<Button>({ 0, 0, 100, 20 }, "Button"_sv);
    auto size = button.intrinsic_size();
    EXPECT_EQ(size.height, 20.0f);
    EXPECT(size.width >= 60 && size.width <= 66);
    EXPECT_EQ(Appearance::missing_tokens(), static_cast<size_t>(0));
}

TEST_CASE(a_click_presses_a_button_only_if_let_go_inside)
{
    TestWindow test;
    auto& button = test.place<Button>({ 50, 50, 100, 20 }, "Press"_sv);
    int pressed = 0;
    button.on_action = [&] { ++pressed; };
    test.click(60, 60);
    EXPECT_EQ(pressed, 1);
    test.press(60, 60);
    test.move_to(300, 300, WS::ButtonLeft);
    test.release(300, 300);
    EXPECT_EQ(pressed, 1);
    button.set_enabled(false);
    test.click(60, 60);
    EXPECT_EQ(pressed, 1);
}

TEST_CASE(a_press_goes_to_whatever_it_started_in)
{
    TestWindow test;
    auto& first = test.place<Button>({ 0, 0, 100, 20 }, "A"_sv);
    auto& second = test.place<Button>({ 200, 0, 100, 20 }, "B"_sv);
    int firsts = 0, seconds = 0;
    first.on_action = [&] { ++firsts; };
    second.on_action = [&] { ++seconds; };
    test.press(10, 10);
    test.move_to(250, 10, WS::ButtonLeft);
    test.move_to(10, 10, WS::ButtonLeft);
    test.release(10, 10);
    EXPECT_EQ(firsts, 1);
    EXPECT_EQ(seconds, 0);
}

TEST_CASE(checkboxes_and_switches_toggle)
{
    TestWindow test;
    auto& box = test.place<Checkbox>({ 0, 0, 100, 16 }, "Hello"_sv);
    test.click(5, 5);
    EXPECT(box.is_on());
    test.click(5, 5);
    EXPECT(!box.is_on());
    auto& toggle = test.place<Switch>({ 0, 100, 38, 22 });
    test.click(10, 110);
    EXPECT(toggle.is_on());
}

TEST_CASE(radio_buttons_of_a_group_exclude_each_other)
{
    TestWindow test;
    auto& one = test.place<RadioButton>({ 0, 0, 100, 16 }, "One"_sv, true);
    auto& two = test.place<RadioButton>({ 0, 30, 100, 16 }, "Two"_sv);
    auto& other_group = test.place<RadioButton>({ 0, 60, 100, 16 }, "Other"_sv, true, 1);
    test.click(5, 35);
    EXPECT(!one.is_on());
    EXPECT(two.is_on());
    EXPECT(other_group.is_on());
}

TEST_CASE(a_segmented_control_chooses_one_at_a_time)
{
    TestWindow test;
    auto& control = test.place<SegmentedControl>({ 0, 0, 300, 22 });
    control.add_segment("One"_sv);
    control.add_segment("Two"_sv);
    control.add_segment("Three"_sv);
    size_t last_change = 99;
    control.on_change = [&](size_t index) { last_change = index; };
    test.click(150, 10);
    EXPECT_EQ(control.selected_index(), 1);
    EXPECT_EQ(last_change, static_cast<size_t>(1));
    control.set_mode(SegmentedControl::Mode::SelectAny);
    test.click(10, 10);
    EXPECT(control.is_selected(0));
    EXPECT(control.is_selected(1));
}

TEST_CASE(a_slider_follows_the_pointer_and_stops_on_ticks)
{
    TestWindow test;
    auto& slider = test.place<Slider>({ 0, 0, 220, 20 }, 0.0f, 100.0f, 0.0f);
    test.press(110, 10);
    EXPECT(slider.value() > 45 && slider.value() < 55);
    test.move_to(500, 10, WS::ButtonLeft);
    EXPECT_EQ(slider.value(), 100.0f);
    test.release(500, 10);
    slider.set_tick_count(11);
    test.press(113, 10);
    float steps = slider.value() / 10;
    EXPECT_EQ(steps, static_cast<float>(static_cast<int>(steps)));
    test.release(113, 10);
}

TEST_CASE(a_stepper_steps_and_repeats_while_held)
{
    TestWindow test;
    auto& stepper = test.place<Stepper>({ 0, 0, 13, 20 }, 0, 100, 5);
    test.press(5, 4);
    EXPECT_EQ(stepper.value(), 6);
    TestPlatform::the().advance(1.0);
    EXPECT(stepper.value() > 8);
    test.release(5, 4);
    int held = stepper.value();
    TestPlatform::the().advance(1.0);
    EXPECT_EQ(stepper.value(), held);
    test.click(5, 16);
    EXPECT_EQ(stepper.value(), held - 1);
}

TEST_CASE(typing_edits_a_field_and_undo_takes_it_back_in_runs)
{
    TestWindow test;
    auto& field = test.place<TextField>({ 10, 10, 200, 21 });
    test.click(20, 15);
    EXPECT(test.window->first_responder() == &field);
    test.type("hello world"_sv);
    EXPECT(same(field.text(), "hello world"_sv));
    test.key(KeyBackspace);
    test.key(KeyBackspace);
    EXPECT(same(field.text(), "hello wor"_sv));
    test.key(KeyZ, WS::ModifierCommand);
    EXPECT(same(field.text(), "hello world"_sv));
    test.key(KeyZ, WS::ModifierCommand);
    EXPECT(same(field.text(), ""_sv));
    test.key(KeyZ, WS::ModifierCommand | WS::ModifierShift);
    EXPECT(same(field.text(), "hello world"_sv));
}

TEST_CASE(moving_and_selecting_in_a_field)
{
    TestWindow test;
    auto& field = test.place<TextField>({ 10, 10, 200, 21 }, "one two three"_sv);
    EXPECT(field.become_first_responder());
    // It starts with all of it selected.
    EXPECT_EQ(field.storage().selected_text().length(), static_cast<size_t>(13));
    test.key(KeyLeft);
    EXPECT_EQ(field.storage().caret(), static_cast<size_t>(0));
    test.key(KeyRight, WS::ModifierOption);
    EXPECT_EQ(field.storage().caret(), static_cast<size_t>(3));
    test.key(KeyRight, WS::ModifierOption | WS::ModifierShift);
    EXPECT(same(field.storage().selected_text(), " two"_sv));
    test.key(KeyC, WS::ModifierCommand);
    test.key(KeyEnd);
    test.key(KeyV, WS::ModifierCommand);
    EXPECT(same(field.text(), "one two three two"_sv));
    test.key(KeyBackspace, WS::ModifierOption);
    EXPECT(same(field.text(), "one two three "_sv));
    test.key(KeyA, WS::ModifierCommand);
    test.key(KeyX, WS::ModifierCommand);
    EXPECT(same(field.text(), ""_sv));
    test.key(KeyV, WS::ModifierCommand);
    EXPECT(same(field.text(), "one two three "_sv));
}

TEST_CASE(non_ascii_text_is_moved_through_by_characters)
{
    TestWindow test;
    auto& field = test.place<TextField>({ 10, 10, 200, 21 }, "né€"_sv);
    field.become_first_responder();
    test.key(KeyEnd);
    size_t end = field.storage().caret();
    test.key(KeyLeft);
    EXPECT_EQ(field.storage().caret(), end - 3);
    test.key(KeyLeft);
    EXPECT_EQ(field.storage().caret(), end - 5);
    test.key(KeyBackspace);
    EXPECT(same(field.text(), "é€"_sv));
}

TEST_CASE(a_secure_field_shows_dots_and_copies_nothing)
{
    TestWindow test;
    auto& field = test.place<TextField>({ 10, 10, 200, 21 }, "secret"_sv);
    field.set_secure(true);
    TestPlatform::the().copy_text("before"_sv);
    field.become_first_responder();
    test.key(KeyC, WS::ModifierCommand);
    EXPECT(same(TestPlatform::the().paste_text().release_value().view(), "before"_sv));
    EXPECT(field.accessibility_value().is_empty());
    test.settle();
}

TEST_CASE(tab_goes_through_what_takes_the_keyboard)
{
    TestWindow test;
    auto& first = test.place<TextField>({ 10, 10, 200, 21 });
    auto& second = test.place<TextField>({ 10, 40, 200, 21 });
    auto& button = test.place<Button>({ 10, 70, 80, 20 }, "Not by default"_sv);
    test.key(KeyTab);
    EXPECT(test.window->first_responder() == &first);
    test.key(KeyTab);
    EXPECT(test.window->first_responder() == &second);
    test.key(KeyTab);
    EXPECT(test.window->first_responder() == &first);
    test.key(KeyTab, WS::ModifierShift);
    EXPECT(test.window->first_responder() == &second);
    Control::set_full_keyboard_access(true);
    test.key(KeyTab);
    EXPECT(test.window->first_responder() == &button);
    int pressed = 0;
    button.on_action = [&] { ++pressed; };
    test.key(KeySpace, 0, " "_sv);
    EXPECT_EQ(pressed, 1);
    Control::set_full_keyboard_access(false);
}

TEST_CASE(enter_and_escape_go_to_the_window_when_nothing_takes_them)
{
    TestWindow test;
    auto& field = test.place<TextField>({ 10, 10, 200, 21 });
    field.become_first_responder();
    int defaults = 0, cancels = 0, commits = 0;
    test.window->set_default_action([&] { ++defaults; });
    test.window->set_cancel_action([&] { ++cancels; });
    field.on_commit = [&] { ++commits; };
    test.key(KeyEnter);
    test.key(KeyEscape);
    EXPECT_EQ(defaults, 1);
    EXPECT_EQ(cancels, 1);
    EXPECT_EQ(commits, 1);
}

TEST_CASE(a_stack_lays_out_with_stretching_and_alignment)
{
    TestWindow test;
    auto& stack = test.place<StackView>({ 0, 0, 300, 100 }, Axis::Horizontal, 10);
    auto& first = stack.add<View>();
    first.set_fixed_size({ 50, 20 });
    auto& second = stack.add<View>();
    second.set_fixed_size({ 0, 20 });
    second.set_stretch(1, 0);
    auto& third = stack.add<View>();
    third.set_fixed_size({ 30, 40 });
    test.window->update_layout_now();
    EXPECT_EQ(first.frame().x, 0.0f);
    EXPECT_EQ(second.frame().x, 60.0f);
    EXPECT_EQ(second.frame().width, 200.0f);
    EXPECT_EQ(third.frame().x, 270.0f);
    EXPECT_EQ(third.frame().height, 100.0f);
    stack.stack().set_alignment(Alignment::Center);
    stack.set_needs_layout();
    test.window->update_layout_now();
    EXPECT_EQ(third.frame().y, 30.0f);
    EXPECT_EQ(third.frame().height, 40.0f);
    auto preferred = stack.preferred_size();
    EXPECT_EQ(preferred.width, 100.0f);
    EXPECT_EQ(preferred.height, 40.0f);
}

TEST_CASE(a_stack_shares_room_equally_and_skips_the_hidden)
{
    TestWindow test;
    auto& stack = test.place<StackView>({ 0, 0, 310, 20 }, Axis::Horizontal, 10);
    stack.stack().set_distribution(Distribution::Equal);
    auto& a = stack.add<View>();
    auto& b = stack.add<View>();
    auto& gone = stack.add<View>();
    auto& c = stack.add<View>();
    gone.set_hidden(true);
    test.window->update_layout_now();
    EXPECT_EQ(a.frame().width, 96.5f);
    EXPECT(b.frame().x > a.frame().right());
    EXPECT_EQ(c.frame().right(), 310.0f);
}

TEST_CASE(a_grid_lines_up_its_columns)
{
    TestWindow test;
    auto& grid = test.place<View>({ 0, 0, 300, 100 });
    grid.set_layout<GridLayout>(2, 8, 4);
    auto& a = grid.add<View>();
    a.set_fixed_size({ 40, 20 });
    auto& b = grid.add<View>();
    b.set_fixed_size({ 100, 20 });
    auto& c = grid.add<View>();
    c.set_fixed_size({ 60, 30 });
    auto& d = grid.add<View>();
    d.set_fixed_size({ 100, 20 });
    test.window->update_layout_now();
    EXPECT_EQ(b.frame().x, 68.0f);
    EXPECT_EQ(d.frame().x, 68.0f);
    EXPECT_EQ(c.frame().y, 24.0f);
    EXPECT_EQ(d.frame().y, 29.0f);
}

TEST_CASE(constraints_place_views)
{
    TestWindow test;
    auto& parent = test.place<View>({ 0, 0, 400, 200 });
    auto& layout = parent.set_layout<ConstraintLayout>();
    auto& a = parent.add<View>();
    a.set_fixed_size({ 100, 50 });
    auto& b = parent.add<View>();
    b.set_fixed_size({ 80, 50 });
    layout.equal({ &a, Attribute::Left }, Anchor { nullptr, Attribute::Left } + 10);
    layout.equal({ &a, Attribute::Top }, Anchor { nullptr, Attribute::Top } + 20);
    layout.equal({ &b, Attribute::Left }, Anchor { &a, Attribute::Right } + 8);
    layout.equal({ &b, Attribute::CenterY }, Anchor { &a, Attribute::CenterY });
    test.window->update_layout_now();
    EXPECT_EQ(a.frame().x, 10.0f);
    EXPECT_EQ(a.frame().width, 100.0f);
    EXPECT_EQ(b.frame().x, 118.0f);
    EXPECT_EQ(b.frame().y, 20.0f);
    // Tied to the parent's edge: with a's width settled, b's gives way.
    layout.constant(a, Attribute::Width, 100);
    layout.equal({ &b, Attribute::Right }, Anchor { nullptr, Attribute::Right } - 10);
    parent.set_needs_layout();
    test.window->update_layout_now();
    EXPECT_EQ(b.frame().right(), 390.0f);
    EXPECT_EQ(b.frame().x, 118.0f);
}

TEST_CASE(damage_redraws_only_what_changed)
{
    TestWindow test(200, 100);
    class Counter final : public View {
    public:
        void draw(Graphics& graphics) override
        {
            ++count;
            graphics.fill({ 0, 0, width(), height() }, Color { 255, 0, 0, 255 });
        }
        int count { 0 };
    };
    auto& left = test.place<Counter>({ 0, 28, 40, 40 });
    auto& right = test.place<Counter>({ 100, 28, 40, 40 });
    test.settle();
    int left_before = left.count, right_before = right.count;
    left.set_needs_display();
    test.settle();
    EXPECT_EQ(left.count, left_before + 1);
    EXPECT_EQ(right.count, right_before);
    auto& pixels = shown_pixels(*test.window);
    EXPECT_EQ(pixels.color_at(10, 60), (Color { 255, 0, 0, 255 }));
}

TEST_CASE(a_scroll_view_scrolls_by_the_wheel_and_springs_back)
{
    TestWindow test(300, 300);
    auto& scroll = test.place<ScrollView>({ 0, 0, 300, 300 });
    auto& document = scroll.make_document<View>();
    document.set_fixed_size({ 300, 1000 });
    test.window->update_layout_now();
    EXPECT_EQ(document.frame().height, 1000.0f);
    test.wheel(100, 100, 5);
    float scrolled = scroll.offset().y;
    EXPECT(scrolled > 0);
    scroll.scroll_to({ 0, 700 });
    EXPECT_EQ(scroll.offset().y, 700.0f);
    scroll.scroll_to({ 0, 5000 });
    EXPECT_EQ(scroll.offset().y, 700.0f);
    // The child gets events where it now shows.
    class Spot final : public View {
    public:
        bool mouse_down(MouseEvent const& event) override
        {
            y = event.position.y;
            return true;
        }
        float y { -1 };
    };
    auto& spot = document.add<Spot>();
    spot.set_frame({ 0, 750, 300, 50 });
    test.click(10, 60);
    EXPECT_EQ(spot.y, 10.0f);
}

TEST_CASE(a_scroll_view_carries_on_and_comes_to_rest_with_motion_on)
{
    TestWindow test(300, 300);
    auto& scroll = test.place<ScrollView>({ 0, 0, 300, 300 });
    auto& document = scroll.make_document<View>();
    document.set_fixed_size({ 300, 5000 });
    test.window->update_layout_now();
    set_motion_enabled(true);
    for (int i = 0; i < 5; ++i) {
        test.wheel(100, 100, 3);
        TestPlatform::the().advance(0.016);
    }
    float at_release = scroll.offset().y;
    TestPlatform::the().advance(0.3);
    float later = scroll.offset().y;
    // Momentum.
    EXPECT(later > at_release);
    TestPlatform::the().advance(5);
    float rested = scroll.offset().y;
    TestPlatform::the().advance(1);
    EXPECT_EQ(scroll.offset().y, rested);

    // Past the top, it gives, then comes back.
    scroll.scroll_to({ 0, 0 });
    test.wheel(100, 100, -10);
    EXPECT(scroll.offset().y < 0);
    TestPlatform::the().advance(3);
    EXPECT_EQ(scroll.offset().y, 0.0f);
    set_motion_enabled(false);
}

TEST_CASE(a_text_view_wraps_edits_and_moves_by_lines)
{
    TestWindow test(300, 300);
    auto& scroll = test.place<ScrollView>({ 0, 0, 300, 300 });
    auto& view = scroll.make_document<TextView>();
    scroll.set_scrolls_horizontally(false);
    view.set_text("the quick brown fox jumps over the lazy dog and carries on for a good many words so that it must wrap\nsecond paragraph"_sv);
    test.window->update_layout_now();
    EXPECT(view.visual_line_count() >= 3);
    view.become_first_responder();
    test.key(KeyHome);
    test.key(KeyDown);
    EXPECT(view.storage().caret() > 0);
    size_t after_first_line = view.storage().caret();
    test.key(KeyUp);
    EXPECT_EQ(view.storage().caret(), static_cast<size_t>(0));
    test.key(KeyEnd);
    test.key(KeyEnter);
    test.type("x"_sv);
    EXPECT(view.text().ends_with("paragraph\nx"_sv));
    test.key(KeyZ, WS::ModifierCommand);
    EXPECT(view.text().ends_with("paragraph\n"_sv));
    (void)after_first_line;
}

TEST_CASE(a_long_text_view_scrolls_to_keep_the_caret_in_sight)
{
    TestWindow test(300, 200);
    auto& scroll = test.place<ScrollView>({ 0, 0, 300, 200 });
    auto& view = scroll.make_document<TextView>();
    scroll.set_scrolls_horizontally(false);
    StringBuilder text;
    for (int i = 0; i < 200; ++i)
        text.append("line\n"_sv);
    view.set_text(text.view());
    test.window->update_layout_now();
    view.become_first_responder();
    test.key(KeyEnd);
    test.window->update_layout_now();
    EXPECT(scroll.offset().y > 0);
    auto caret = view.caret_rect(view.storage().caret());
    EXPECT(caret.bottom() <= scroll.offset().y + scroll.clip_view().height() + 4);
    test.key(KeyHome);
    EXPECT_EQ(scroll.offset().y, 0.0f);
}

namespace {

class Names final : public TableModel {
public:
    [[nodiscard]] size_t row_count() const override { return count; }
    [[nodiscard]] String text_at(size_t row, size_t column) const override
    {
        StringBuilder builder;
        builder.appendff("r{}c{}", row, column);
        return MUST(builder.to_string());
    }
    void sort_by(size_t column, bool ascending) override
    {
        sorted_column = static_cast<int>(column);
        sorted_ascending = ascending;
    }
    size_t count { 100 };
    int sorted_column { -1 };
    bool sorted_ascending { false };
};

struct TreeNode {
    char const* name;
    Vector<TreeNode*> children;
};

class Tree final : public OutlineModel {
public:
    [[nodiscard]] size_t child_count(void* parent) const override { return parent ? static_cast<TreeNode*>(parent)->children.size() : roots.size(); }
    [[nodiscard]] void* child_at(void* parent, size_t index) const override { return parent ? static_cast<TreeNode*>(parent)->children[index] : roots[index]; }
    [[nodiscard]] String text_of(void* item, size_t) const override
    {
        auto const* name = static_cast<TreeNode*>(item)->name;
        return MUST(String::from_view(StringView(name, __builtin_strlen(name))));
    }
    Vector<TreeNode*> roots;
};

}

TEST_CASE(a_table_selects_by_clicks_and_keys)
{
    TestWindow test(400, 300);
    Names model;
    auto& table = test.place<TableView>({ 0, 0, 400, 300 }, &model);
    table.add_column("Name"_sv, 200);
    table.add_column("Other"_sv, 100);
    table.set_allows_multiple_selection(true);
    test.window->update_layout_now();
    float header = Appearance::the().metric("table.headerHeight"_sv);
    float row = Appearance::the().metric("table.rowHeight"_sv);
    test.click(50, header + row * 2.5f);
    EXPECT_EQ(table.selected_row(), 2);
    EXPECT(test.window->first_responder() == &table);
    test.click(50, header + row * 4.5f, WS::ModifierShift);
    EXPECT_EQ(table.selection().size(), static_cast<size_t>(3));
    test.click(50, header + row * 3.5f, WS::ModifierCommand);
    EXPECT_EQ(table.selection().size(), static_cast<size_t>(2));
    test.key(KeyDown);
    EXPECT_EQ(table.selection().size(), static_cast<size_t>(1));
    test.key(KeyEnd);
    EXPECT_EQ(table.selected_row(), 99);
    EXPECT(table.scroll_view().offset().y > 0);
    test.settle();
    // Clicks in the header sort.
    test.click(50, 5);
    EXPECT_EQ(model.sorted_column, 0);
    EXPECT(model.sorted_ascending);
    test.click(50, 5);
    EXPECT(!model.sorted_ascending);
    size_t activated = 999;
    table.on_activate = [&](size_t which) { activated = which; };
    test.key(KeyEnter);
    EXPECT_EQ(activated, static_cast<size_t>(99));
}

TEST_CASE(an_outline_opens_and_closes)
{
    TestWindow test(400, 300);
    TreeNode leaf { "leaf", {} };
    TreeNode folder { "folder", {} };
    folder.children.append(&leaf);
    TreeNode top { "top", {} };
    Tree model;
    model.roots.append(&folder);
    model.roots.append(&top);
    auto& outline = test.place<OutlineView>({ 0, 0, 400, 300 }, model);
    outline.add_column("Name"_sv, 300);
    outline.set_headers_visible(false);
    test.window->update_layout_now();
    EXPECT_EQ(outline.model()->row_count(), static_cast<size_t>(2));
    auto const& t = Appearance::the();
    float row = t.metric("table.rowHeight"_sv);
    // On the disclosure triangle.
    test.click(t.metric("table.selectionInset"_sv) + t.metric("table.cellPadding"_sv) + 3, row / 2);
    EXPECT_EQ(outline.model()->row_count(), static_cast<size_t>(3));
    EXPECT(outline.item_at_row(1) == &leaf);
    test.settle();
    test.click(100, row * 1.5f);
    EXPECT_EQ(outline.selected_row(), 1);
    test.key(KeyLeft);
    EXPECT_EQ(outline.selected_row(), 0);
    test.key(KeyLeft);
    EXPECT_EQ(outline.model()->row_count(), static_cast<size_t>(2));
    test.key(KeyRight);
    EXPECT_EQ(outline.model()->row_count(), static_cast<size_t>(3));
}

TEST_CASE(a_collection_flows_with_its_width)
{
    TestWindow test(400, 300);
    class Items final : public CollectionModel {
    public:
        [[nodiscard]] size_t item_count() const override { return 10; }
        [[nodiscard]] String title_of(size_t) const override { return MUST(String::from_view("Item"_sv)); }
    } model;
    auto& collection = test.place<CollectionView>({ 0, 0, 212, 300 }, model);
    test.settle();
    test.click(150, 10);
    EXPECT_EQ(collection.selected_item(), 1);
    test.key(KeyDown);
    EXPECT_EQ(collection.selected_item(), 3);
    collection.set_size({ 420, 300 });
    test.settle();
    test.key(KeyDown);
    EXPECT_EQ(collection.selected_item(), 7);
}

TEST_CASE(a_split_view_moves_its_divider_within_limits)
{
    TestWindow test(400, 200);
    auto& split = test.place<SplitView>({ 0, 0, 400, 200 });
    auto& left = split.add_pane<View>(100, 50);
    auto& right = split.add_pane<View>(0, 100);
    test.window->update_layout_now();
    EXPECT_EQ(left.frame().width, 100.0f);
    EXPECT_EQ(right.frame().x, 101.0f);
    EXPECT_EQ(right.frame().right(), 400.0f);
    test.press(100, 50);
    test.move_to(150, 50, WS::ButtonLeft);
    test.window->update_layout_now();
    EXPECT_EQ(left.frame().width, 150.0f);
    test.move_to(900, 50, WS::ButtonLeft);
    test.window->update_layout_now();
    EXPECT_EQ(right.frame().width, 100.0f);
    test.move_to(0, 50, WS::ButtonLeft);
    test.window->update_layout_now();
    EXPECT_EQ(left.frame().width, 50.0f);
    test.release(0, 50);
}

TEST_CASE(tabs_show_one_page)
{
    TestWindow test(400, 300);
    auto& tabs = test.place<TabView>({ 0, 0, 400, 300 });
    auto& first = tabs.add_tab<View>("First"_sv);
    auto& second = tabs.add_tab<View>("Second"_sv);
    test.window->update_layout_now();
    EXPECT(!first.is_hidden());
    EXPECT(second.is_hidden());
    size_t changed = 99;
    tabs.on_change = [&](size_t index) { changed = index; };
    // The right half of the strip.
    test.click(210, 10);
    EXPECT_EQ(changed, static_cast<size_t>(1));
    EXPECT(first.is_hidden());
    EXPECT(!second.is_hidden());
}

TEST_CASE(a_menu_opens_chooses_and_closes)
{
    TestWindow test;
    Menu menu;
    int chosen = 0;
    menu.add("First"_sv, [&] { chosen = 1; });
    menu.add_separator();
    auto& off = menu.add("Disabled"_sv, [&] { chosen = 2; });
    off.enabled = false;
    menu.add_with_shortcut("Third"_sv, KeyT, "T"_sv, [&] { chosen = 3; });
    menu.open(*test.window, { 100, 100 });
    EXPECT(menu.is_open());
    TestPlatform::the().run_deferred();
    auto& popup = *menu.window();
    auto const& t = Appearance::the();
    float padding = t.metric("menu.padding"_sv);
    float row = t.metric("menu.rowHeight"_sv);
    float separator = t.metric("menu.separatorHeight"_sv);
    // Hovering the disabled does nothing.
    popup.handle_mouse(WS::MouseKind::Moved, { 20, padding + row + separator + row / 2 }, 0, 0, 0, 0);
    EXPECT_EQ(menu.highlight(), -1);
    // Keys work it.
    test.key(KeyDown);
    EXPECT_EQ(menu.highlight(), 0);
    test.key(KeyDown);
    EXPECT_EQ(menu.highlight(), 3);
    test.key(KeyEnter);
    EXPECT_EQ(chosen, 3);
    EXPECT(!menu.is_open());
    TestPlatform::the().run_deferred();

    // A press elsewhere ends it, with no choice.
    chosen = 0;
    menu.open(*test.window, { 100, 100 });
    TestPlatform::the().run_deferred();
    menu.window()->handle_mouse(WS::MouseKind::Pressed, { -50, -50 }, WS::ButtonLeft, 0, 0, 0);
    TestPlatform::the().run_deferred();
    EXPECT(!menu.is_open());
    EXPECT_EQ(chosen, 0);
    TestPlatform::the().run_deferred();
}

TEST_CASE(a_submenu_opens_beside_its_item)
{
    TestWindow test;
    Menu menu;
    int chosen = 0;
    menu.add("First"_sv);
    auto& more = menu.add_submenu("More"_sv);
    more.add("Deep"_sv, [&] { chosen = 7; });
    menu.open(*test.window, { 100, 100 });
    TestPlatform::the().run_deferred();
    test.key(KeyDown);
    test.key(KeyDown);
    test.key(KeyRight);
    EXPECT(more.is_open());
    EXPECT_EQ(more.highlight(), 0);
    test.key(KeyEnter);
    EXPECT_EQ(chosen, 7);
    EXPECT(!more.is_open());
    EXPECT(!menu.is_open());
    TestPlatform::the().run_deferred();
}

TEST_CASE(a_popup_button_shows_what_is_chosen)
{
    TestWindow test;
    auto& popup = test.place<PopUpButton>({ 50, 50, 120, 20 });
    popup.add_item("Alpha"_sv);
    popup.add_item("Beta"_sv);
    EXPECT_EQ(popup.selected_index(), 0);
    EXPECT(same(popup.selected_title(), "Alpha"_sv));
    int changed = -1;
    popup.on_change = [&](int index) { changed = index; };
    test.press(60, 60);
    TestPlatform::the().run_deferred();
    EXPECT(popup.menu().is_open());
    EXPECT_EQ(popup.menu().highlight(), 0);
    test.release(60, 60);
    test.key(KeyDown);
    test.key(KeyEnter);
    EXPECT_EQ(popup.selected_index(), 1);
    EXPECT_EQ(changed, 1);
    TestPlatform::the().run_deferred();
    test.settle();
}

TEST_CASE(a_sheet_takes_over_and_gives_back)
{
    TestWindow test;
    auto& under = test.place<Button>({ 10, 40, 100, 20 }, "Under"_sv);
    auto& field = test.place<TextField>({ 10, 70, 100, 21 });
    field.become_first_responder();
    int under_pressed = 0;
    under.on_action = [&] { ++under_pressed; };
    auto* content = new StackView(Axis::Vertical);
    auto& inside = content->add<TextField>();
    Sheet::present(*test.window, adopt_own(*static_cast<View*>(content)), { 300, 200 });
    EXPECT(Sheet::is_presented(*test.window));
    test.settle();
    test.click(20, 50);
    EXPECT_EQ(under_pressed, 0);
    EXPECT(test.window->first_responder() == &inside);
    test.key(KeyTab);
    EXPECT(test.window->first_responder() == &inside);
    Sheet::dismiss(*test.window);
    test.settle();
    EXPECT(!Sheet::is_presented(*test.window));
    EXPECT(test.window->first_responder() == &field);
    test.click(20, 50);
    EXPECT_EQ(under_pressed, 1);
}

TEST_CASE(an_alert_answers_by_button_and_by_key)
{
    TestWindow test;
    size_t answer = 99;
    {
        Alert alert("Save it?"_sv, "You will lose your changes otherwise."_sv);
        alert.add_button("Save"_sv);
        alert.add_button("Cancel"_sv);
        alert.show(*test.window, [&](size_t which) { answer = which; });
    }
    test.settle();
    test.key(KeyEnter);
    EXPECT_EQ(answer, static_cast<size_t>(0));
    test.settle();
    answer = 99;
    {
        Alert alert("Again?"_sv);
        alert.add_button("Yes"_sv);
        alert.add_button("No"_sv);
        alert.show(*test.window, [&](size_t which) { answer = which; });
    }
    test.settle();
    test.key(KeyEscape);
    EXPECT_EQ(answer, static_cast<size_t>(1));
    test.settle();
}

TEST_CASE(a_file_panel_walks_the_tree_and_answers)
{
    TestWindow test(700, 500);
    String chosen = MUST(String::from_view("none"_sv));
    FilePanel::run(*test.window, FilePanel::Kind::Open, "/"_sv, {}, [&](StringView path) { chosen = MUST(String::from_view(path)); });
    test.settle();
    auto tree = Accessibility::tree_of(*test.window);
    auto const* table = Accessibility::find(tree, Accessibility::Role::Table, {});
    EXPECT(table != nullptr);
    auto rect = table->frame;
    float row = Appearance::the().metric("table.rowHeight"_sv);
    // Users is first, by name... after readme.txt? Alphabetical: readme, Users.
    test.click(rect.x + 30, rect.y + row * 1.5f);
    test.settle();
    test.key(KeyEnter);
    test.settle();
    // Inside /Users now: notes.txt, Pictures.
    test.click(rect.x + 30, rect.y + row * 0.5f);
    test.settle();
    test.key(KeyEnter);
    test.settle();
    EXPECT(same(chosen.view(), "/Users/notes.txt"_sv));
}

TEST_CASE(a_save_panel_takes_a_name)
{
    TestWindow test(700, 500);
    String chosen;
    FilePanel::run(*test.window, FilePanel::Kind::Save, "/Users"_sv, "new.txt"_sv, [&](StringView path) { chosen = MUST(String::from_view(path)); });
    test.settle();
    test.key(KeyEnter);
    test.settle();
    EXPECT(same(chosen.view(), "/Users/new.txt"_sv));
    FilePanel::run(*test.window, FilePanel::Kind::Save, "/Users"_sv, "x"_sv, [&](StringView path) { chosen = MUST(String::from_view(path)); });
    test.settle();
    test.key(KeyEscape);
    test.settle();
    EXPECT(chosen.is_empty());
}

TEST_CASE(a_popover_comes_and_goes)
{
    TestWindow test;
    Popover popover;
    bool closed = false;
    popover.on_close = [&] { closed = true; };
    auto* content = new TextField;
    popover.show(*test.window, adopt_own(*static_cast<View*>(content)), { 200, 100 }, { 100, 100, 20, 20 });
    EXPECT(popover.is_shown());
    TestPlatform::the().run_deferred();
    content->become_first_responder();
    test.type("hi"_sv);
    EXPECT(same(content->text(), "hi"_sv));
    test.key(KeyEscape);
    EXPECT(!popover.is_shown());
    EXPECT(closed);
    TestPlatform::the().run_deferred();
}

TEST_CASE(the_color_panel_turns_wheel_clicks_into_colors)
{
    TestWindow test;
    Color last { 0, 0, 0, 0 };
    ColorPanel::the().show(Color { 255, 0, 0, 255 }, [&](Color color) { last = color; });
    auto* panel = ColorPanel::the().window();
    EXPECT(panel != nullptr);
    panel->handle_activation(true);
    TestPlatform::the().run_deferred();
    auto& pixels = shown_pixels(*panel);
    EXPECT(pixels.width() > 0);
    auto const& t = Appearance::the();
    float title = panel->title_bar_height();
    float wheel = t.metric("colorPanel.wheel"_sv);
    float left = (panel->size().width - wheel) / 2;
    // In the middle: no color at all.
    panel->handle_mouse(WS::MouseKind::Pressed, { left + wheel / 2, title + 12 + wheel / 2 }, WS::ButtonLeft, 0, 0, 0);
    panel->handle_mouse(WS::MouseKind::Released, { left + wheel / 2, title + 12 + wheel / 2 }, 0, 0, 0, 0);
    EXPECT(last.red > 240 && last.green > 240 && last.blue > 240);
    ColorPanel::the().close();
    TestPlatform::the().run_deferred();
}

TEST_CASE(the_font_panel_reports_its_choice)
{
    TestWindow test;
    FontSpec last;
    FontPanel::the().show(FontSpec { 13, FontWeight::Regular, false }, [&](FontSpec spec) { last = spec; });
    auto* panel = FontPanel::the().window();
    EXPECT(panel != nullptr);
    panel->handle_activation(true);
    TestPlatform::the().run_deferred();
    (void)shown_pixels(*panel);
    panel->update_layout_now();
    auto tree = Accessibility::tree_of(*panel);
    (void)tree;
    FontPanel::the().close();
    TestPlatform::the().run_deferred();
}

TEST_CASE(appearance_changes_redraw_every_window)
{
    TestWindow test(100, 100);
    auto& pixels = shown_pixels(*test.window);
    auto light = pixels.color_at(10, 100);
    MUST(Appearance::use("Dark"_sv));
    auto& dark_pixels = shown_pixels(*test.window);
    auto dark = dark_pixels.color_at(10, 100);
    EXPECT(light.red > 200);
    EXPECT(dark.red < 80);
    Appearance::the().set_accent(Color { 255, 0, 0, 255 });
    EXPECT_EQ(Appearance::the().accent().red, static_cast<u8>(255));
    Appearance::the().set_accent(Color { 0, 122, 255, 255 });
    MUST(Appearance::use("Light"_sv));
    EXPECT_EQ(Appearance::missing_tokens(), static_cast<size_t>(0));
}

TEST_CASE(the_accent_moves_what_is_drawn_in_it)
{
    TestWindow test;
    auto& button = test.place<Button>({ 20, 60, 100, 20 }, "Go"_sv, ButtonStyle::Default);
    (void)button;
    auto blue = shown_pixels(*test.window).color_at(60, 140);
    EXPECT(blue.blue > 200 && blue.red < 100);
    Appearance::the().set_accent(Color { 255, 59, 48, 255 });
    auto red = shown_pixels(*test.window).color_at(60, 140);
    EXPECT(red.red > 200 && red.blue < 120);
    // Still its own lightness: the fill's top is lighter than the bottom.
    auto top = Appearance::the().color("bezelAccent.fillTop"_sv);
    auto bottom = Appearance::the().color("bezelAccent.fillBottom"_sv);
    EXPECT(top.green >= bottom.green);
    // It outlives a change of appearance.
    MUST(Appearance::use("Dark"_sv));
    EXPECT_EQ(Appearance::the().accent().red, static_cast<u8>(255));
    EXPECT(Appearance::the().color("switch.trackOn"_sv).red > Appearance::the().color("switch.trackOn"_sv).blue);
    Appearance::the().set_accent(Color { 0, 122, 255, 255 });
    MUST(Appearance::use("Light"_sv));
    EXPECT_EQ(Appearance::the().color("bezelAccent.fillTop"_sv), (Color { 0x27, 0x91, 0xff, 255 }));
}

TEST_CASE(indeterminate_progress_sweeps_and_spinners_turn_with_motion_on)
{
    TestWindow test;
    auto& bar = test.place<ProgressBar>({ 20, 60, 200, 18 });
    auto& spinner = test.place<Spinner>({ 20, 100, 32, 32 });
    set_motion_enabled(true);
    bar.set_indeterminate(true);
    spinner.set_running(true);
    TestPlatform::the().advance(0.5);
    test.settle();
    bar.set_indeterminate(false);
    spinner.set_running(false);
    TestPlatform::the().advance(0.5);
    set_motion_enabled(false);
    test.settle();
}

TEST_CASE(a_switch_springs_across_with_motion_on)
{
    TestWindow test;
    auto& toggle = test.place<Switch>({ 20, 60, 38, 22 });
    set_motion_enabled(true);
    toggle.set_on(true);
    TestPlatform::the().advance(1);
    EXPECT(toggle.is_on());
    set_motion_enabled(false);
    test.settle();
}

TEST_CASE(a_layer_is_drawn_once_and_faded_without_redrawing)
{
    TestWindow test(100, 100);
    class Counter final : public View {
    public:
        void draw(Graphics& graphics) override
        {
            ++count;
            graphics.fill({ 0, 0, width(), height() }, Color { 0, 0, 255, 255 });
        }
        int count { 0 };
    };
    auto& view = test.place<Counter>({ 0, 28, 40, 40 });
    view.set_layer_backed(true);
    test.settle();
    int drawn = view.count;
    view.set_opacity(0.5f);
    test.settle();
    EXPECT_EQ(view.count, drawn);
    auto& pixels = shown_pixels(*test.window);
    auto middle = pixels.color_at(20, 90);
    EXPECT(middle.blue > 100 && middle.blue < 255);
    view.set_translation({ 30, 0 });
    test.settle();
    EXPECT_EQ(view.count, drawn);
    view.set_needs_display();
    test.settle();
    EXPECT_EQ(view.count, drawn + 1);
}

TEST_CASE(the_accessibility_tree_names_what_is_there)
{
    TestWindow test;
    auto& stack = test.place<StackView>({ 0, 28, 400, 300 }, Axis::Vertical, 4);
    (void)stack.add<Label>("Title"_sv);
    auto& button = stack.add<Button>("Go"_sv);
    int pressed = 0;
    button.on_action = [&] { ++pressed; };
    auto& box = stack.add<Checkbox>("Check"_sv, ToggleState::On);
    auto& field = stack.add<TextField>("value"_sv);
    field.set_accessibility_label("Name"_sv);
    auto& hidden = stack.add<Button>("Hidden"_sv);
    hidden.set_hidden(true);
    auto& disabled = stack.add<Button>("Off"_sv);
    disabled.set_enabled(false);
    test.window->update_layout_now();
    auto tree = Accessibility::tree_of(*test.window);
    auto description = Accessibility::describe(tree);
    EXPECT(description.view().contains("text \"Title\""_sv));
    EXPECT(description.view().contains("button \"Go\""_sv));
    EXPECT(description.view().contains("checkbox \"Check\" = on"_sv));
    EXPECT(description.view().contains("textfield \"Name\" = value"_sv));
    EXPECT(description.view().contains("button \"Off\" (disabled)"_sv));
    EXPECT(!description.view().contains("Hidden"_sv));
    auto const* found = Accessibility::find(tree, Accessibility::Role::Button, "Go"_sv);
    EXPECT(found != nullptr);
    EXPECT(found->view->accessibility_press());
    EXPECT_EQ(pressed, 1);
    EXPECT(found->frame.width > 0);
    (void)box;
}

TEST_CASE(the_builder_makes_the_same_views)
{
    TestWindow test;
    using namespace Build;
    TextField* name = nullptr;
    Button* ok = nullptr;
    Checkbox* check = nullptr;
    int pressed = 0;
    StringView choices[2] = { "A"_sv, "B"_sv };
    test.window->root_view().adopt_child(vstack(8,
        label("Name:"_sv, "headline"_sv),
        field("Ada"_sv).bind(name),
        hstack(8, spacer(), button("Cancel"_sv).disabled(), default_button("OK"_sv).bind(ok).action([&] { ++pressed; })),
        checkbox("Remember"_sv, true).bind(check).accessibility("Keep"_sv),
        segments(Span<StringView const>(choices, 2)),
        popup(Span<StringView const>(choices, 2)),
        slider(0, 1, 0.5f).width(200),
        separator(),
        box("Group"_sv, label("Inside"_sv)))
                                             .release());
    auto& root = *test.window->root_view().children().last();
    root.set_frame({ 10, 40, 500, 340 });
    test.window->update_layout_now();
    EXPECT(same(name->text(), "Ada"_sv));
    EXPECT(ok->frame().width > 0);
    auto at = ok->to_window({ 5, 5 });
    test.click(at.x, at.y);
    EXPECT_EQ(pressed, 1);
    EXPECT(check->is_on());
    test.settle();
}

TEST_CASE(views_going_away_leave_nothing_behind)
{
    TestWindow test;
    auto& parent = test.place<View>({ 0, 0, 200, 200 });
    auto& field = parent.add<TextField>();
    field.set_frame({ 0, 0, 100, 21 });
    field.become_first_responder();
    test.press(10, 10);
    test.window->root_view().remove_child(parent);
    EXPECT(test.window->first_responder() == nullptr);
    test.release(10, 10);
    test.move_to(50, 50);
    test.settle();
}

TEST_CASE(a_tooltip_comes_after_a_while_over_the_view)
{
    TestWindow test;
    auto& button = test.place<Button>({ 50, 50, 100, 20 }, "Tip"_sv);
    button.set_tooltip("Help"_sv);
    test.move_to(60, 60);
    TestPlatform::the().advance(2);
    (void)test;
    test.move_to(400, 300);
    TestPlatform::the().advance(0.1);
}
