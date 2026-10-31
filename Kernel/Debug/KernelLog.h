/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Format.h>

namespace Kernel {

class PL011;
class TextConsole;

// Kernel messages: always to the serial port, and to the screen when a
// text console is attached (verbose boot, panics). Single-CPU and
// interrupts masked for now; it gains a lock when more CPUs start (M2).
class KernelLog final : public FormatSink {
public:
    void initialize();
    void attach_serial(PL011*);
    void attach_console(TextConsole*);

    void write(StringView) override;

    // Starts a line with the time since boot: "[    0.001234] ".
    void begin_line();

private:
    PL011* m_serial { nullptr };
    TextConsole* m_console { nullptr };
    u64 m_start_counter { 0 };
};

KernelLog& kernel_log();

template<typename... Args>
void dmesgln(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    auto& log = kernel_log();
    log.begin_line();
    format_to(log, format, args...);
    log.write("\n"_sv);
}

}
