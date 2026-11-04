/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/time.h>
#include <Kernel/FileSystem/KQueue.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Vector.h>

namespace Kernel {

ErrorOr<FlatPtr> Process::sys_kqueue(TrapFrame&, SyscallArguments const&)
{
    auto description = TRY(FileDescription::create_for_kqueue(TRY(KQueue::create())));
    return static_cast<FlatPtr>(TRY(allocate_fd(move(description), false)));
}

ErrorOr<FlatPtr> Process::sys_kevent(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(static_cast<i32>(arguments.values[0])));
    auto* queue = description->kqueue();
    if (!queue)
        return Error::from_errno(EBADF);
    auto const change_count = static_cast<i32>(arguments.values[2]);
    auto const event_count = static_cast<i32>(arguments.values[4]);
    if (change_count < 0 || event_count < 0)
        return Error::from_errno(EINVAL);

    Vector<vulture_kevent> changes;
    TRY(changes.try_resize(static_cast<size_t>(change_count)));
    if (change_count)
        TRY(copy_from_user(changes.data(), arguments.values[1], changes.size() * sizeof(vulture_kevent)));

    // No time given is for ever; none at all (a time of zero) is not waiting.
    u64 deadline = ~0ull;
    if (arguments.values[5]) {
        auto const timeout = TRY(copy_from_user<vulture_timespec>(arguments.values[5]));
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1'000'000'000)
            return Error::from_errno(EINVAL);
        u64 const nanoseconds = static_cast<u64>(timeout.tv_sec) * 1'000'000'000ull + static_cast<u64>(timeout.tv_nsec);
        deadline = nanoseconds == 0 ? 0 : Time::now() + Time::ticks_from_nanoseconds(nanoseconds);
    }

    constexpr size_t most_events_at_once = 4096;
    Vector<vulture_kevent> events;
    TRY(events.try_resize(min(static_cast<size_t>(event_count), most_events_at_once)));
    size_t const produced = TRY(queue->process(*this, changes.data(), changes.size(), events.data(), events.size(), deadline));
    if (produced)
        TRY(copy_to_user(arguments.values[3], events.data(), produced * sizeof(vulture_kevent)));
    return produced;
}

}
