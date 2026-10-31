/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibTest/TestCase.h>

namespace {

int s_alive = 0;

struct Counted : RefCounted<Counted> {
    explicit Counted(int v)
        : value(v)
    {
        ++s_alive;
    }
    ~Counted() { --s_alive; }
    int value;
};

struct Shape {
    virtual ~Shape() = default;
    virtual int kind() const { return 1; }
};

struct Circle final : Shape {
    Circle() { ++s_alive; }
    ~Circle() override { --s_alive; }
    int kind() const override { return 2; }
};

}

TEST_CASE(ref_ptr_counts_references_and_deletes_at_zero)
{
    s_alive = 0;
    {
        auto first = MUST(try_make_ref_counted<Counted>(7));
        EXPECT_EQ(first->ref_count(), 1u);
        EXPECT_EQ(s_alive, 1);
        {
            NonnullRefPtr<Counted> second = first;
            RefPtr<Counted> third = second;
            EXPECT_EQ(first->ref_count(), 3u);
            EXPECT_EQ(third->value, 7);
        }
        EXPECT_EQ(first->ref_count(), 1u);

        RefPtr<Counted> moved = move(first);
        EXPECT_EQ(moved->ref_count(), 1u);
        EXPECT_EQ(s_alive, 1);
        moved.clear();
        EXPECT(moved.is_null());
        EXPECT_EQ(s_alive, 0);
    }
    EXPECT_EQ(s_alive, 0);
}

TEST_CASE(ref_ptr_leak_and_adopt)
{
    s_alive = 0;
    auto object = MUST(try_make_ref_counted<Counted>(3));
    Counted* raw = object.leak_ref();
    EXPECT_EQ(raw->ref_count(), 1u);
    {
        auto adopted = adopt_ref(*raw);
        EXPECT_EQ(adopted->ref_count(), 1u);
    }
    EXPECT_EQ(s_alive, 0);

    RefPtr<Counted> maybe = MUST(try_make_ref_counted<Counted>(4));
    auto nonnull = maybe.release_nonnull();
    EXPECT(maybe.is_null());
    EXPECT_EQ(nonnull->value, 4);
}

TEST_CASE(own_ptr_deletes_with_owner)
{
    s_alive = 0;
    {
        NonnullOwnPtr<Shape> object = MUST(try_make<Circle>());
        EXPECT_EQ(object->kind(), 2);
        EXPECT_EQ(s_alive, 1);
        OwnPtr<Shape> maybe = move(object);
        EXPECT(maybe);
        OwnPtr<Shape> other;
        other = move(maybe);
        EXPECT(maybe.is_null());
        EXPECT_EQ(s_alive, 1);
    }
    EXPECT_EQ(s_alive, 0);

    OwnPtr<Shape> released = MUST(try_make<Circle>());
    auto nonnull = released.release_nonnull();
    EXPECT(released.is_null());
    EXPECT_EQ(nonnull->kind(), 2);
}
