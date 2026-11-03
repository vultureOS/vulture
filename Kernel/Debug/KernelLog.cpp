/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Debug/KernelLog.h>

namespace Kernel {

void KernelLog::initialize()
{
    m_start_counter = Arch::monotonic_counter();
}

void KernelLog::attach_serial(BootSerial* serial)
{
    m_serial = serial;
}

void KernelLog::attach_console(TextConsole* console)
{
    m_console = console;
}

// Text only: an ordinary array, so that it's there from the very first line.
static char s_history[KernelLog::history_size];

u64 KernelLog::history_total()
{
    auto state = lock();
    u64 const total = m_history_total;
    unlock(state);
    return total;
}

size_t KernelLog::read_history(u64 position, Bytes into)
{
    auto state = lock();
    u64 const oldest = m_history_total > history_size ? m_history_total - history_size : 0;
    size_t copied = 0;
    if (position >= oldest) {
        while (copied < into.size() && position + copied < m_history_total) {
            into[copied] = static_cast<u8>(s_history[(position + copied) % history_size]);
            ++copied;
        }
    }
    unlock(state);
    return copied;
}

void KernelLog::write(StringView text)
{
    for (char character : text) {
        s_history[m_history_total % history_size] = character;
        ++m_history_total;
    }
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

void KernelLog::write_raw(StringView text)
{
    auto state = lock();
    write(text);
    unlock(state);
}

Arch::InterruptState KernelLog::lock()
{
    if (m_panic_mode.load(MemoryOrder::Relaxed))
        return Arch::disable_interrupts();
    return m_lock.lock();
}

void KernelLog::unlock(Arch::InterruptState state)
{
    // Panic mode may have begun while this processor held the lock.
    if (m_lock.is_locked_by_current_processor())
        m_lock.unlock(state);
    else
        Arch::restore_interrupts(state);
}

KernelLog& kernel_log()
{
    static KernelLog log;
    return log;
}

}
