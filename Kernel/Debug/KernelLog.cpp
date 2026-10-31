/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Serial/PL011.h>

namespace Kernel {

void KernelLog::initialize()
{
    m_start_counter = Arch::monotonic_counter();
}

void KernelLog::attach_serial(PL011* serial)
{
    m_serial = serial;
}

void KernelLog::attach_console(TextConsole* console)
{
    m_console = console;
}

void KernelLog::write(StringView text)
{
    if (m_serial) {
        for (char character : text) {
            if (character == '\n')
                m_serial->write('\r');
            m_serial->write(character);
        }
    }
    if (m_console)
        m_console->write(text);
}

void KernelLog::begin_line()
{
    u64 frequency = Arch::monotonic_counter_frequency();
    if (frequency == 0)
        return;
    u64 elapsed = Arch::monotonic_counter() - m_start_counter;
    u64 seconds = elapsed / frequency;
    u64 microseconds = (elapsed % frequency) * 1'000'000 / frequency;
    format_to(*this, "[{:>5}.{:06}] ", seconds, microseconds);
}

KernelLog& kernel_log()
{
    static KernelLog log;
    return log;
}

}
