/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/StringView.h>

// One line as logd prints it:
//
//   2026-11-01 09:41:00.123456+0000 0x2a       Default     0x0       57     0    notifyd: [sub:cat] message
//
// date, time, thread, type, activity, process number, time to live, and the rest.

namespace Console {

struct LogLine {
    StringView time;
    StringView type;
    StringView pid;
    StringView process;
    StringView message;
};

// False if it is not one of those: a line can say anything after its columns, but the
// columns are logd's own.
[[nodiscard]] inline bool parse_log_line(StringView line, LogLine& result)
{
    size_t position = 0;
    auto next_word = [&]() -> StringView {
        while (position < line.length() && line[position] == ' ')
            ++position;
        size_t start = position;
        while (position < line.length() && line[position] != ' ')
            ++position;
        return line.substring_view(start, position - start);
    };
    auto date = next_word();
    auto time = next_word();
    auto thread = next_word();
    auto type = next_word();
    auto activity = next_word();
    auto pid = next_word();
    auto ttl = next_word();
    if (date.is_empty() || time.is_empty() || thread.is_empty() || type.is_empty() || activity.is_empty() || pid.is_empty() || ttl.is_empty())
        return false;
    if (date.length() != 10 || date[4] != '-' || !thread.starts_with("0x"_sv))
        return false;
    while (position < line.length() && line[position] == ' ')
        ++position;
    auto rest = line.substring_view(position, line.length() - position);
    // The process's name goes up to the first ": ".
    size_t colon = 0;
    while (colon + 1 < rest.length() && !(rest[colon] == ':' && rest[colon + 1] == ' '))
        ++colon;
    result.time = time.length() >= 15 ? time.substring_view(0, 15) : time;
    result.type = type;
    result.pid = pid;
    if (colon + 1 < rest.length()) {
        result.process = rest.substring_view(0, colon);
        result.message = rest.substring_view(colon + 2, rest.length() - colon - 2);
    } else {
        result.process = {};
        result.message = rest;
    }
    return true;
}

}
