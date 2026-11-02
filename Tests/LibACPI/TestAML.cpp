/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibACPI/AML.h>
#include <LibTest/TestCase.h>

using namespace ACPI;

// Tables assembled by hand, from the specification's grammar.

namespace {

// Name (_S5_, Package (0x04) { 0x05, 0x06, Zero, Zero })
constexpr u8 s5_alone[] = {
    0x08, '_', 'S', '5', '_',
    0x12, 0x08, 0x04, 0x0a, 0x05, 0x0a, 0x06, 0x00, 0x00
};

// Scope (\_SB) { Device (PCI0) { Method (_FOO) { Return (5) } } } Method (\X) {...} Name (_S5_, ...)
constexpr u8 s5_after_scopes[] = {
    0x10, 0x15, '\\', '_', 'S', 'B', '_',
    0x5b, 0x82, 0x0d, 'P', 'C', 'I', '0',
    0x14, 0x07, '_', 'F', 'O', 'O', 0x00, 0xa4,
    0x14, 0x06, 'X', 'X', 'X', 'X', 0x00,
    0x08, '_', 'S', '5', '_',
    0x12, 0x05, 0x02, 0x0a, 0x05, 0x00
};

// With the short forms of numbers: Package (2) { One, Zero }
constexpr u8 s5_short[] = {
    0x08, '_', 'S', '5', '_',
    0x12, 0x04, 0x02, 0x01, 0x00
};

// Device (PCI0) { Name (_PRT, Package (2) {
//     Package (4) { 0x0001FFFF, 0, 0, 16 },
//     Package (4) { 0x0002FFFF, 1, LNKB, 0 } }) }
constexpr u8 routes[] = {
    0x5b, 0x82, 0x27, 'P', 'C', 'I', '0',
    0x08, '_', 'P', 'R', 'T',
    0x12, 0x1c, 0x02,
    0x12, 0x0b, 0x04, 0x0c, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x0a, 0x10,
    0x12, 0x0d, 0x04, 0x0c, 0xff, 0xff, 0x02, 0x00, 0x01, 'L', 'N', 'K', 'B', 0x00,
};

}

TEST_CASE(a_package_of_numbers_says_how_to_power_off)
{
    auto const found = find_sleep_type_s5({ s5_alone, sizeof(s5_alone) });
    EXPECT(found.has_value());
    if (!found.has_value())
        return;
    EXPECT_EQ(found->a, 5);
    EXPECT_EQ(found->b, 6);
}

TEST_CASE(it_is_found_past_scopes_devices_and_methods)
{
    auto const found = find_sleep_type_s5({ s5_after_scopes, sizeof(s5_after_scopes) });
    EXPECT(found.has_value());
    if (!found.has_value())
        return;
    EXPECT_EQ(found->a, 5);
}

TEST_CASE(the_short_forms_of_numbers_count)
{
    auto const found = find_sleep_type_s5({ s5_short, sizeof(s5_short) });
    EXPECT(found.has_value());
    if (!found.has_value())
        return;
    EXPECT_EQ(found->a, 1);
    EXPECT_EQ(found->b, 0);
}

TEST_CASE(a_table_without_it_says_nothing)
{
    constexpr u8 other[] = { 0x08, '_', 'S', '4', '_', 0x12, 0x04, 0x02, 0x01, 0x00 };
    EXPECT(!find_sleep_type_s5({ other, sizeof(other) }).has_value());
    EXPECT(!find_sleep_type_s5({ other, 0 }).has_value());
}

TEST_CASE(damaged_tables_end_without_reading_past_them)
{
    for (size_t size = 0; size < sizeof(s5_after_scopes); ++size)
        (void)find_sleep_type_s5({ s5_after_scopes, size });
    for (size_t size = 0; size < sizeof(routes); ++size) {
        Vector<InterruptRoute> found;
        find_interrupt_routes({ routes, size }, found);
    }
    // Lengths that say more than there is.
    constexpr u8 too_long[] = { 0x10, 0xff, 0xff, 0xff, 0xff };
    EXPECT(!find_sleep_type_s5({ too_long, sizeof(too_long) }).has_value());
}

TEST_CASE(interrupt_routes_direct_and_by_link_device)
{
    Vector<InterruptRoute> found;
    find_interrupt_routes({ routes, sizeof(routes) }, found);
    EXPECT_EQ(found.size(), 2u);
    if (found.size() != 2)
        return;
    EXPECT_EQ(found[0].slot, 1u);
    EXPECT_EQ(found[0].pin, 0);
    EXPECT(found[0].direct);
    EXPECT_EQ(found[0].line, 16u);
    EXPECT_EQ(found[1].slot, 2u);
    EXPECT_EQ(found[1].pin, 1);
    EXPECT(!found[1].direct);
}
