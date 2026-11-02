/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Console/LogLine.h>
#include <LibTest/TestCase.h>

using Console::LogLine;
using Console::parse_log_line;

TEST_CASE(a_line_as_logd_prints_it)
{
    LogLine line;
    EXPECT(parse_log_line("2026-11-01 09:41:00.123456+0000 0x2a       Default     0x0                  57     0    notifyd: [com.vulture.notifyd:server] started"_sv, line));
    EXPECT(line.time == "09:41:00.123456"_sv);
    EXPECT(line.type == "Default"_sv);
    EXPECT(line.pid == "57"_sv);
    EXPECT(line.process == "notifyd"_sv);
    EXPECT(line.message == "[com.vulture.notifyd:server] started"_sv);
}

TEST_CASE(colons_later_stay_in_the_message)
{
    LogLine line;
    EXPECT(parse_log_line("2026-11-01 09:41:00.123456+0000 0x2a Error 0x0 7 0 logd: a: b: c"_sv, line));
    EXPECT(line.process == "logd"_sv);
    EXPECT(line.message == "a: b: c"_sv);
}

TEST_CASE(what_is_not_a_log_line)
{
    LogLine line;
    EXPECT(!parse_log_line(""_sv, line));
    EXPECT(!parse_log_line("Timestamp                       Thread     Type        Activity             PID    TTL"_sv, line));
    EXPECT(!parse_log_line("2026-11-01 only"_sv, line));
    EXPECT(!parse_log_line("short"_sv, line));
}
