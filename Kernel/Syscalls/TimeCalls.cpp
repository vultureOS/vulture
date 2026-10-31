/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/time.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Time/Time.h>

namespace Kernel {

static constexpr u64 nanoseconds_per_second = 1'000'000'000;

ErrorOr<FlatPtr> Process::sys_clock_gettime(TrapFrame&, SyscallArguments const& arguments)
{
    int clock = static_cast<int>(arguments.values[0]);
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
        return Error::from_errno(EINVAL);
    // Real time comes from the real-time clock read at boot (counting from
    // 1970), monotonic time from boot.
    u64 now = clock == CLOCK_REALTIME ? Time::wall_clock_nanoseconds() : Time::nanoseconds_from_ticks(Time::now());
    vulture_timespec value { static_cast<long long>(now / nanoseconds_per_second), static_cast<long>(now % nanoseconds_per_second) };
    TRY(copy_to_user(arguments.values[1], value));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_nanosleep(TrapFrame&, SyscallArguments const& arguments)
{
    auto duration = TRY(copy_from_user<vulture_timespec>(arguments.values[0]));
    if (duration.tv_sec < 0 || duration.tv_nsec < 0 || duration.tv_nsec >= static_cast<long>(nanoseconds_per_second))
        return Error::from_errno(EINVAL);
    u64 nanoseconds = static_cast<u64>(duration.tv_sec) * nanoseconds_per_second + static_cast<u64>(duration.tv_nsec);
    if (Thread::sleep_until(Time::now() + Time::ticks_from_nanoseconds(nanoseconds)) == Thread::SleepResult::Interrupted)
        return Error::from_errno(EINTR);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sched_yield(TrapFrame&, SyscallArguments const&)
{
    Thread::yield();
    return 0;
}

}
