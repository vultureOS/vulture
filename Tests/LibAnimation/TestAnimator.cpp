/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibAnimation/Animator.h>
#include <LibTest/TestCase.h>

using namespace Animation;

TEST_CASE(runs_to_exactly_one_and_finishes)
{
    Animator animator;
    float seen = -1;
    int finished = 0;
    animator.start(Timing::linear(1), [&](float progress) { seen = progress; }, [&] { ++finished; });
    EXPECT(animator.is_active());
    animator.tick(10);
    EXPECT_EQ(seen, 0.0f);
    animator.tick(10.5);
    EXPECT_EQ(seen, 0.5f);
    EXPECT_EQ(finished, 0);
    // Well past the end, in one jump: still lands exactly.
    animator.tick(20);
    EXPECT_EQ(seen, 1.0f);
    EXPECT_EQ(finished, 1);
    EXPECT(!animator.is_active());
}

TEST_CASE(a_cancelled_one_stops_without_finishing)
{
    Animator animator;
    int calls = 0;
    int finished = 0;
    auto id = animator.start(Timing::linear(1), [&](float) { ++calls; }, [&] { ++finished; });
    animator.tick(0);
    EXPECT(animator.is_running(id));
    animator.cancel(id);
    EXPECT(!animator.is_running(id));
    animator.tick(5);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(finished, 0);
    EXPECT(!animator.is_active());
}

TEST_CASE(callbacks_may_start_and_cancel_others)
{
    Animator animator;
    float second = -1;
    Animator::Id third = 0;
    int third_calls = 0;
    third = animator.start(Timing::linear(1), [&](float) { ++third_calls; });
    animator.start(Timing::linear(1), [&](float progress) {
        if (progress > 0.4f)
            animator.cancel(third);
    });
    animator.start(Timing::linear(1), [&](float) {}, [&] {
        animator.start(Timing::linear(1), [&](float progress) { second = progress; });
    });
    animator.tick(0);
    animator.tick(0.5);
    int after_cancel = third_calls;
    animator.tick(1);
    // Started when the first ended: it begins at its first tick.
    EXPECT_EQ(second, -1.0f);
    animator.tick(1.25);
    EXPECT_EQ(second, 0.0f);
    animator.tick(1.75);
    EXPECT_EQ(second, 0.5f);
    EXPECT_EQ(third_calls, after_cancel);
}

TEST_CASE(tells_when_it_needs_ticking)
{
    Animator animator;
    int woke = 0;
    animator.on_became_active = [&] { ++woke; };
    animator.start(Timing::linear(1), [](float) {});
    animator.start(Timing::linear(1), [](float) {});
    EXPECT_EQ(woke, 1);
    animator.tick(0);
    animator.tick(2);
    animator.start(Timing::linear(1), [](float) {});
    EXPECT_EQ(woke, 2);
}
