/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/IntrusiveList.h>
#include <LibTest/TestCase.h>

namespace {

struct Item {
    explicit Item(int v)
        : value(v)
    {
    }

    int value;
    IntrusiveListNode first_node;
    IntrusiveListNode second_node;
};

using FirstList = IntrusiveList<Item, &Item::first_node>;
using SecondList = IntrusiveList<Item, &Item::second_node>;

int sum(FirstList const& list)
{
    int total = 0;
    for (auto& item : list)
        total = total * 10 + item.value;
    return total;
}

}

TEST_CASE(intrusive_list_append_prepend_and_iterate)
{
    Item one(1), two(2), three(3);
    FirstList list;
    EXPECT(list.is_empty());
    list.append(two);
    list.append(three);
    list.prepend(one);
    EXPECT_EQ(list.size(), 3u);
    EXPECT_EQ(sum(list), 123);
    EXPECT(list.first() == &one);
    EXPECT(list.last() == &three);
    EXPECT(list.next(one) == &two);
    EXPECT(list.next(three) == nullptr);

    list.remove(one);
    list.remove(two);
    list.remove(three);
}

TEST_CASE(intrusive_list_remove_from_middle_and_ends)
{
    Item one(1), two(2), three(3), four(4);
    FirstList list;
    list.append(one);
    list.append(two);
    list.append(three);
    list.append(four);

    two.first_node.remove();
    EXPECT_EQ(sum(list), 134);
    list.remove(one);
    EXPECT_EQ(sum(list), 34);
    list.remove(four);
    EXPECT_EQ(sum(list), 3);
    EXPECT(list.first() == &three && list.last() == &three);
    EXPECT(list.take_first() == &three);
    EXPECT(list.is_empty());
    EXPECT_EQ(list.size(), 0u);
    EXPECT(!three.first_node.is_in_list());
}

TEST_CASE(intrusive_list_insert_before)
{
    Item one(1), two(2), three(3);
    FirstList list;
    list.append(three);
    list.insert_before(one, three);
    list.insert_before(two, three);
    EXPECT_EQ(sum(list), 123);
    while (list.take_first()) {
    }
}

TEST_CASE(intrusive_list_element_on_two_lists)
{
    Item one(1), two(2);
    FirstList first;
    SecondList second;
    first.append(one);
    first.append(two);
    second.append(two);
    EXPECT(first.contains(two));
    EXPECT(second.contains(two));
    EXPECT(!second.contains(one));

    second.remove(two);
    EXPECT(first.contains(two));
    EXPECT_EQ(sum(first), 12);
    first.remove(one);
    first.remove(two);
}
