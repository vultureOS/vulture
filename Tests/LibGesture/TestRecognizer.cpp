/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Vector.h>
#include <LibGesture/Recognizer.h>
#include <LibTest/TestCase.h>

using namespace Gesture;

namespace {

struct Heard {
    Recognizer recognizer;
    Vector<Event> events;
    double now { 1 };

    Heard()
    {
        recognizer.on_event = [this](Event const& event) { events.append(event); };
    }

    void later(double seconds) { now += seconds; }
    void down(int slot, float x, float y) { recognizer.finger_down(slot, x, y, now); }
    void move(int slot, float x, float y) { recognizer.finger_moved(slot, x, y, now); }
    void up(int slot) { recognizer.finger_up(slot, now); }

    [[nodiscard]] size_t count_of(Kind kind) const
    {
        size_t total = 0;
        for (auto const& event : events) {
            if (event.kind == kind)
                ++total;
        }
        return total;
    }
    [[nodiscard]] Event const* last_of(Kind kind) const
    {
        Event const* found = nullptr;
        for (auto const& event : events) {
            if (event.kind == kind)
                found = &event;
        }
        return found;
    }
};

}

TEST_CASE(a_tap_is_quick_and_stays_put)
{
    Heard heard;
    heard.down(0, 100, 100);
    heard.later(0.1);
    heard.up(0);
    EXPECT_EQ(heard.count_of(Kind::Tap), 1u);
    EXPECT_EQ(heard.last_of(Kind::Tap)->x, 100.0f);
}

TEST_CASE(a_slow_press_is_not_a_tap)
{
    Heard heard;
    heard.down(0, 100, 100);
    heard.later(0.6);
    heard.up(0);
    EXPECT_EQ(heard.count_of(Kind::Tap), 0u);
}

TEST_CASE(a_shaky_tap_is_still_a_tap)
{
    Heard heard;
    heard.down(0, 100, 100);
    heard.later(0.05);
    heard.move(0, 103, 102);
    heard.later(0.05);
    heard.up(0);
    EXPECT_EQ(heard.count_of(Kind::Tap), 1u);
    EXPECT_EQ(heard.count_of(Kind::PointerMoved), 0u);
}

TEST_CASE(one_finger_moves_the_pointer)
{
    Heard heard;
    heard.down(0, 100, 100);
    heard.later(0.02);
    heard.move(0, 130, 100);
    heard.later(0.02);
    heard.move(0, 160, 110);
    heard.up(0);
    EXPECT_EQ(heard.count_of(Kind::Tap), 0u);
    EXPECT_EQ(heard.last_of(Kind::PointerMoved)->x, 160.0f);
    EXPECT_EQ(heard.last_of(Kind::PointerMoved)->y, 110.0f);
}

TEST_CASE(two_fingers_tapping_is_the_other_button)
{
    Heard heard;
    heard.down(0, 100, 100);
    heard.down(1, 140, 100);
    heard.later(0.1);
    heard.up(0);
    heard.up(1);
    EXPECT_EQ(heard.count_of(Kind::SecondaryTap), 1u);
    EXPECT_EQ(heard.count_of(Kind::Tap), 0u);
    EXPECT_EQ(heard.last_of(Kind::SecondaryTap)->x, 120.0f);
}

TEST_CASE(two_fingers_together_scroll)
{
    Heard heard;
    heard.down(0, 100, 300);
    heard.down(1, 140, 300);
    for (int step = 1; step <= 5; ++step) {
        heard.later(0.016);
        heard.move(0, 100, 300 - static_cast<float>(step) * 10);
        heard.move(1, 140, 300 - static_cast<float>(step) * 10);
    }
    heard.up(0);
    heard.up(1);
    EXPECT(heard.count_of(Kind::Scrolled) > 0);
    float total = 0;
    for (auto const& event : heard.events) {
        if (event.kind == Kind::Scrolled)
            total += event.y;
    }
    // Most of the way: what went before it was said to be a scroll isn't scrolled.
    EXPECT(total < -30 && total >= -50);
    EXPECT_EQ(heard.count_of(Kind::ScrollEnded), 1u);
    EXPECT(heard.last_of(Kind::ScrollEnded)->y < 0);
    EXPECT_EQ(heard.count_of(Kind::Pinched), 0u);
}

TEST_CASE(two_fingers_apart_pinch)
{
    Heard heard;
    heard.down(0, 100, 300);
    heard.down(1, 200, 300);
    for (int step = 1; step <= 5; ++step) {
        heard.later(0.016);
        heard.move(0, 100 - static_cast<float>(step) * 10, 300);
        heard.move(1, 200 + static_cast<float>(step) * 10, 300);
    }
    heard.up(0);
    heard.up(1);
    EXPECT(heard.count_of(Kind::Pinched) > 0);
    EXPECT_EQ(heard.count_of(Kind::Scrolled), 0u);
    float scale = 1;
    for (auto const& event : heard.events) {
        if (event.kind == Kind::Pinched)
            scale *= event.scale;
    }
    EXPECT(scale > 1.2f);
}

TEST_CASE(three_fingers_across_swipe)
{
    Heard heard;
    heard.down(0, 300, 300);
    heard.down(1, 340, 300);
    heard.down(2, 380, 300);
    for (int step = 1; step <= 10; ++step) {
        heard.later(0.016);
        for (int finger = 0; finger < 3; ++finger)
            heard.move(finger, 300 + static_cast<float>(finger) * 40 - static_cast<float>(step) * 20, 300);
    }
    heard.up(0);
    heard.up(1);
    heard.up(2);
    EXPECT(heard.count_of(Kind::SwipeChanged) > 0);
    EXPECT_EQ(heard.count_of(Kind::SwipeEnded), 1u);
    auto const* ended = heard.last_of(Kind::SwipeEnded);
    EXPECT(ended->x < -150);
    // Going left, quickly.
    EXPECT(ended->y < -500);
}

TEST_CASE(three_fingers_up_and_down)
{
    Heard up;
    up.down(0, 300, 300);
    up.down(1, 340, 300);
    up.down(2, 380, 300);
    for (int step = 1; step <= 6; ++step) {
        up.later(0.016);
        for (int finger = 0; finger < 3; ++finger)
            up.move(finger, 300 + static_cast<float>(finger) * 40, 300 - static_cast<float>(step) * 10);
    }
    EXPECT_EQ(up.count_of(Kind::ThreeFingersUp), 1u);
    EXPECT_EQ(up.count_of(Kind::SwipeChanged), 0u);

    Heard down;
    down.down(0, 300, 300);
    down.down(1, 340, 300);
    down.down(2, 380, 300);
    for (int step = 1; step <= 6; ++step) {
        down.later(0.016);
        for (int finger = 0; finger < 3; ++finger)
            down.move(finger, 300 + static_cast<float>(finger) * 40, 300 + static_cast<float>(step) * 10);
    }
    EXPECT_EQ(down.count_of(Kind::ThreeFingersDown), 1u);
}

TEST_CASE(a_scroll_stays_a_scroll_until_they_have_all_gone)
{
    Heard heard;
    heard.down(0, 100, 300);
    heard.down(1, 140, 300);
    for (int step = 1; step <= 4; ++step) {
        heard.later(0.016);
        heard.move(0, 100, 300 - static_cast<float>(step) * 10);
        heard.move(1, 140, 300 - static_cast<float>(step) * 10);
    }
    size_t const scrolled = heard.count_of(Kind::Scrolled);
    // A third finger, part way through, doesn't start a swipe.
    heard.down(2, 180, 260);
    for (int step = 1; step <= 6; ++step) {
        heard.later(0.016);
        heard.move(0, 100 - static_cast<float>(step) * 20, 260);
        heard.move(1, 140 - static_cast<float>(step) * 20, 260);
        heard.move(2, 180 - static_cast<float>(step) * 20, 260);
    }
    EXPECT_EQ(heard.count_of(Kind::Scrolled), scrolled);
    EXPECT_EQ(heard.count_of(Kind::SwipeChanged), 0u);
    EXPECT_EQ(heard.count_of(Kind::ScrollEnded), 1u);
    heard.up(0);
    heard.up(1);
    heard.up(2);
    // And with them gone, it's a fresh start.
    heard.down(0, 100, 100);
    heard.later(0.05);
    heard.up(0);
    EXPECT_EQ(heard.count_of(Kind::Tap), 1u);
}

TEST_CASE(what_makes_no_sense_is_ignored)
{
    Heard heard;
    heard.up(3);
    heard.move(3, 1, 1);
    heard.down(99, 1, 1);
    heard.down(-1, 1, 1);
    EXPECT_EQ(heard.events.size(), 0u);
    EXPECT_EQ(heard.recognizer.fingers_down(), 0);
}

TEST_CASE(a_swipe_let_go_goes_on_or_goes_back)
{
    // Not far, and not fast: back.
    EXPECT(!swipe_carries_on(0.05f, 50));
    EXPECT(!swipe_carries_on(0.2f, 0));
    // Far enough.
    EXPECT(swipe_carries_on(0.3f, 0));
    // Not far, but a flick.
    EXPECT(swipe_carries_on(0.05f, 900));
    // Far, but going back quickly.
    EXPECT(!swipe_carries_on(0.8f, -900));
}
