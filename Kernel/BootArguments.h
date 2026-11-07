/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/StringView.h>

namespace Kernel {

// The kernel command line (the boot-args file): words separated by spaces,
// either flags ("-v") or key=value pairs ("fault=data-abort").
class BootArguments {
public:
    explicit BootArguments(StringView command_line)
        : m_command_line(command_line)
    {
    }

    [[nodiscard]] StringView text() const { return m_command_line; }
    [[nodiscard]] bool has_flag(StringView flag) const;
    [[nodiscard]] Optional<StringView> value(StringView key) const;

    // Kept for the rest of the kernel's life, for sysctl's kern.bootargs, as a Mac has it.
    static void remember(StringView command_line);
    [[nodiscard]] static StringView remembered();

private:
    StringView m_command_line;
};

}
