/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/HashMap.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibTest/TestCase.h>

TEST_CASE(vector_grows_inserts_and_removes)
{
    Vector<int> numbers;
    EXPECT(numbers.is_empty());
    for (int i = 0; i < 100; ++i)
        numbers.append(i);
    EXPECT_EQ(numbers.size(), 100u);
    EXPECT_EQ(numbers[57], 57);
    EXPECT_EQ(numbers.first(), 0);
    EXPECT_EQ(numbers.last(), 99);

    numbers.insert(0, -1);
    numbers.insert(50, 1000);
    EXPECT_EQ(numbers[0], -1);
    EXPECT_EQ(numbers[50], 1000);
    numbers.remove(50);
    numbers.remove(0);
    EXPECT_EQ(numbers[50], 50);
    EXPECT_EQ(numbers.take_last(), 99);

    EXPECT_EQ(numbers.remove_all_matching([](int n) { return n % 2 == 1; }), 49u);
    EXPECT_EQ(numbers.size(), 50u);
    EXPECT(numbers.contains_slow(42) && !numbers.contains_slow(43));

    Vector<int> copy = numbers;
    Vector<int> moved = move(numbers);
    EXPECT(numbers.is_empty());
    EXPECT_EQ(copy.size(), 50u);
    EXPECT_EQ(moved[10], 20);
    MUST(moved.try_resize(3));
    EXPECT_EQ(moved.size(), 3u);
}

TEST_CASE(vector_holds_move_only_objects)
{
    Vector<OwnPtr<int>> owners;
    for (int i = 0; i < 20; ++i)
        owners.append(OwnPtr<int>(MUST(try_make<int>(i))));
    EXPECT_EQ(*owners[19], 19);
    owners.remove(0);
    EXPECT_EQ(*owners[0], 1);
}

TEST_CASE(string_and_builder)
{
    auto hello = MUST(String::from_view("hello"_sv));
    EXPECT_EQ(hello.length(), 5u);
    EXPECT(hello == "hello"_sv);
    EXPECT_EQ(__builtin_strlen(hello.characters()), 5u);
    String copy = hello;
    EXPECT(copy == hello);
    String empty;
    EXPECT(empty.is_empty() && empty.characters()[0] == '\0');

    StringBuilder builder;
    builder.append("path"_sv);
    builder.append('/');
    builder.appendff("{}-{:03}", "item"_sv, 7);
    EXPECT(builder.view() == "path/item-007"_sv);
    auto built = MUST(builder.to_string());
    EXPECT(built == "path/item-007"_sv);
}

TEST_CASE(hash_map_set_get_remove)
{
    HashMap<int, int> squares;
    for (int i = 0; i < 1000; ++i)
        squares.set(i, i * i);
    EXPECT_EQ(squares.size(), 1000u);
    EXPECT(squares.get(31) && *squares.get(31) == 961);
    EXPECT(!squares.get(1000));
    squares.set(31, -1);
    EXPECT_EQ(*squares.get(31), -1);
    EXPECT_EQ(squares.size(), 1000u);

    for (int i = 0; i < 1000; i += 2)
        EXPECT(squares.remove(i));
    EXPECT(!squares.remove(0));
    EXPECT_EQ(squares.size(), 500u);
    EXPECT(!squares.contains(10) && squares.contains(11));
    // Re-inserting reuses deleted slots.
    for (int i = 0; i < 1000; i += 2)
        squares.set(i, i);
    EXPECT_EQ(squares.size(), 1000u);

    size_t visited = 0;
    squares.for_each([&](int, int) { ++visited; });
    EXPECT_EQ(visited, 1000u);
}

TEST_CASE(hash_map_with_string_keys)
{
    HashMap<String, int> ages;
    ages.set(MUST(String::from_view("ada"_sv)), 36);
    ages.set(MUST(String::from_view("alan"_sv)), 41);
    EXPECT(ages.get("ada"_sv) && *ages.get("ada"_sv) == 36);
    auto key = MUST(String::from_view("alan"_sv));
    EXPECT(ages.get(key) && *ages.get(key) == 41);
    EXPECT(ages.contains(MUST(String::from_view("ada"_sv))));
    EXPECT(!ages.contains(MUST(String::from_view("grace"_sv))));

    HashMap<StringView, int> views;
    views.set("one"_sv, 1);
    views.set("two"_sv, 2);
    EXPECT(views.get("two"_sv) && *views.get("two"_sv) == 2);
}
