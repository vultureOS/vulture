/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/BootArguments.h>
#include <LibTest/TestCase.h>

TEST_CASE(flags)
{
    Kernel::BootArguments arguments { "-v  fault=trap -s"_sv };
    EXPECT(arguments.has_flag("-v"_sv));
    EXPECT(arguments.has_flag("-s"_sv));
    EXPECT(!arguments.has_flag("-x"_sv));
    EXPECT(!arguments.has_flag("fault"_sv));
}

TEST_CASE(values)
{
    Kernel::BootArguments arguments { "fault=data-abort cpus=4 empty="_sv };
    EXPECT_EQ(arguments.value("fault"_sv), "data-abort"_sv);
    EXPECT_EQ(arguments.value("cpus"_sv), "4"_sv);
    EXPECT_EQ(arguments.value("empty"_sv), ""_sv);
    EXPECT(!arguments.value("missing"_sv).has_value());
    EXPECT(!arguments.value("faul"_sv).has_value());
}

TEST_CASE(empty_command_line)
{
    Kernel::BootArguments arguments { ""_sv };
    EXPECT(!arguments.has_flag("-v"_sv));
    EXPECT(!arguments.value("fault"_sv).has_value());
    EXPECT(arguments.text().is_empty());
}
