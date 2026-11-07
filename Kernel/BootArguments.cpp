/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/BootArguments.h>

namespace Kernel {

bool BootArguments::has_flag(StringView flag) const
{
    bool found = false;
    m_command_line.for_each_split_view(' ', SplitBehavior::SkipEmpty, [&](StringView word) {
        if (word == flag)
            found = true;
    });
    return found;
}

namespace {

char s_remembered[256];
size_t s_remembered_length;

}

void BootArguments::remember(StringView command_line)
{
    s_remembered_length = min(command_line.length(), sizeof(s_remembered) - 1);
    __builtin_memcpy(s_remembered, command_line.characters_without_null_termination(), s_remembered_length);
    s_remembered[s_remembered_length] = '\0';
}

StringView BootArguments::remembered()
{
    return { s_remembered, s_remembered_length };
}

Optional<StringView> BootArguments::value(StringView key) const
{
    Optional<StringView> found;
    m_command_line.for_each_split_view(' ', SplitBehavior::SkipEmpty, [&](StringView word) {
        auto equals = word.find('=');
        if (equals.has_value() && word.substring_view(0, equals.value()) == key)
            found = word.substring_view(equals.value() + 1);
    });
    return found;
}

}
