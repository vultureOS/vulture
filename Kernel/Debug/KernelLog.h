/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Devices/Serial/BootSerial.h>

#include <Kernel/Locking/SpinLock.h>
#include <LibBase/Format.h>

namespace Kernel {

class TextConsole;

// Kernel messages: always to the serial port, and to the screen when a
// text console is attached (verbose boot, panics). A lock keeps lines from
// different processors whole; a panic bypasses it, so a processor that
// stopped while holding it cannot silence the report.
class KernelLog final : public FormatSink {
public:
    void initialize();
    void attach_serial(BootSerial*);
    void attach_console(TextConsole*);

    void write(StringView) override;
    // Text from programs: as is (no timestamps), under the lock.
    void write_raw(StringView);

    // Starts a line with the time since boot: "[    0.001234] ".
    void begin_line();

    // Everything said since boot, as far as it's kept (the last history_size bytes): how much there's been, and a copy of
    // what's still there from `position`, as much as `into` holds. Returns how much that was: 0 at the end, or if that part
    // is gone.
    static constexpr size_t history_size = 256 * 1024;
    [[nodiscard]] u64 history_total();
    [[nodiscard]] size_t read_history(u64 position, Bytes into);

    [[nodiscard]] Arch::InterruptState lock();
    void unlock(Arch::InterruptState);
    void enter_panic_mode() { m_panic_mode.store(true); }

private:
    BootSerial* m_serial { nullptr };
    TextConsole* m_console { nullptr };
    u64 m_start_counter { 0 };
    SpinLock m_lock;
    Atomic<bool> m_panic_mode { false };
    u64 m_history_total { 0 };
};

KernelLog& kernel_log();

template<typename... Args>
void dmesgln(CheckedFormatString<Base::TypeIdentity<Args>...> format, Args const&... args)
{
    auto& log = kernel_log();
    auto state = log.lock();
    log.begin_line();
    format_to(log, format, args...);
    log.write("\n"_sv);
    log.unlock(state);
}

}
