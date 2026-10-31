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
