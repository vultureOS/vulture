/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Atomic.h>
#include <LibTest/TestCase.h>

TEST_CASE(atomic_load_store_exchange)
{
    Atomic<u32> value { 5 };
    EXPECT_EQ(value.load(), 5u);
    value.store(7, MemoryOrder::Release);
    EXPECT_EQ(value.load(MemoryOrder::Acquire), 7u);
    EXPECT_EQ(value.exchange(9), 7u);
    EXPECT_EQ(value.load(MemoryOrder::Relaxed), 9u);
}

TEST_CASE(atomic_arithmetic_returns_previous_value)
{
    Atomic<u64> value { 10 };
    EXPECT_EQ(value.fetch_add(5), 10u);
    EXPECT_EQ(value.fetch_sub(3), 15u);
    EXPECT_EQ(value.fetch_or(0x100), 12u);
    EXPECT_EQ(value.fetch_and(0xff), 0x10cu);
    EXPECT_EQ(value.load(), 0x0cu);
}

TEST_CASE(atomic_compare_exchange)
{
    Atomic<int> value { 1 };
    int expected = 2;
    EXPECT(!value.compare_exchange_strong(expected, 3));
    EXPECT_EQ(expected, 1);
    EXPECT(value.compare_exchange_strong(expected, 3, MemoryOrder::AcquireRelease));
    EXPECT_EQ(value.load(), 3);
}

TEST_CASE(atomic_pointer_and_enum)
{
    enum class State : u8 {
        Idle,
        Busy,
    };
    Atomic<State> state { State::Idle };
    EXPECT(state.exchange(State::Busy) == State::Idle);
    EXPECT(state.load() == State::Busy);

    int a = 0;
    int b = 0;
    Atomic<int*> pointer { &a };
    int* expected = &a;
    EXPECT(pointer.compare_exchange_strong(expected, &b));
    EXPECT(pointer.load() == &b);
}
