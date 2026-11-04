/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/Inode.h>
#include <Kernel/FileSystem/KQueue.h>
#include <Kernel/FileSystem/Pipe.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/IPC/Port.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>

namespace Kernel {

namespace {

// How often files being watched get looked at. Nothing tells a kqueue when a file changes yet: FSEvents, with M19, will.
constexpr u64 vnode_look_interval_microseconds = 20'000;

u64 timer_ticks(long data, unsigned int fflags)
{
    u64 const amount = data < 0 ? 0 : static_cast<u64>(data);
    if (fflags & NOTE_SECONDS)
        return Time::ticks_from_microseconds(amount * 1'000'000);
    if (fflags & NOTE_USECONDS)
        return Time::ticks_from_microseconds(amount);
    if (fflags & NOTE_NSECONDS)
        return Time::ticks_from_nanoseconds(amount);
    return Time::ticks_from_microseconds(amount * 1'000);
}

}

ErrorOr<NonnullRefPtr<KQueue>> KQueue::create()
{
    auto* queue = new (Base::nothrow) KQueue;
    if (!queue)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*queue);
}

ErrorOr<void> KQueue::apply(Process& process, vulture_kevent const& change)
{
    Knote* existing = nullptr;
    size_t existing_index = 0;
    for (size_t i = 0; i < m_knotes.size(); ++i) {
        if (m_knotes[i].ident == change.ident && m_knotes[i].filter == change.filter) {
            existing = &m_knotes[i];
            existing_index = i;
            break;
        }
    }
    if (change.flags & EV_DELETE) {
        if (!existing)
            return Error::from_errno(ENOENT);
        m_knotes.remove(existing_index);
        return {};
    }
    if (!(change.flags & EV_ADD) && !existing) {
        // EVFILT_USER can be triggered by a change with no EV_ADD, but it does have to have been added.
        return Error::from_errno(ENOENT);
    }
    if (!existing) {
        Knote knote;
        knote.ident = change.ident;
        knote.filter = change.filter;
        switch (change.filter) {
        case EVFILT_READ:
        case EVFILT_WRITE: {
            auto description = TRY(process.file(static_cast<int>(change.ident)));
            // A kqueue that watched itself, by however many others, would be asking itself forever.
            if (description->kqueue())
                return Error::from_errno(EINVAL);
            knote.description_serial = description->serial();
            break;
        }
        case EVFILT_VNODE: {
            auto description = TRY(process.file(static_cast<int>(change.ident)));
            if (!description->inode())
                return Error::from_errno(EINVAL);
            knote.description_serial = description->serial();
            auto const metadata = description->inode()->metadata();
            knote.last_size = static_cast<long long>(metadata.size);
            knote.last_modified = metadata.modification_time_ns ? metadata.modification_time_ns : metadata.modification_time;
            knote.last_changed = metadata.change_time_ns;
            knote.last_links = metadata.link_count;
            break;
        }
        case EVFILT_PROC: {
            auto target = Process::from_pid(static_cast<ProcessID>(change.ident));
            if (!target)
                return Error::from_errno(ESRCH);
            knote.last_forks = target->fork_count();
            knote.last_execs = target->exec_count();
            break;
        }
        case EVFILT_SIGNAL:
            if (change.ident == 0 || change.ident >= NSIG)
                return Error::from_errno(EINVAL);
            knote.last_signal_count = process.signal_count(static_cast<int>(change.ident));
            break;
        case EVFILT_TIMER:
            knote.period = timer_ticks(change.data, change.fflags);
            if (knote.period == 0)
                knote.period = 1;
            knote.next_fire = Time::now() + knote.period;
            break;
        case EVFILT_USER:
            break;
        default:
            return Error::from_errno(EINVAL);
        }
        TRY(m_knotes.try_append(move(knote)));
        existing = &m_knotes.last();
    }
    auto& knote = *existing;
    if (change.flags & EV_ADD) {
        knote.flags = change.flags & (EV_ONESHOT | EV_CLEAR | EV_DISPATCH);
        knote.udata = change.udata;
        if (change.filter == EVFILT_TIMER) {
            knote.period = max<u64>(timer_ticks(change.data, change.fflags), 1);
            knote.next_fire = Time::now() + knote.period;
            knote.data = 0;
        } else if (change.filter != EVFILT_USER) {
            knote.fflags = change.fflags;
        }
        knote.enabled = !(change.flags & EV_DISABLE);
    }
    if (change.flags & EV_ENABLE) {
        knote.enabled = true;
        knote.disabled_by_dispatch = false;
    }
    if (change.flags & EV_DISABLE)
        knote.enabled = false;
    if (change.filter == EVFILT_USER) {
        switch (change.fflags & NOTE_FFCTRLMASK) {
        case NOTE_FFAND:
            knote.fflags &= change.fflags & NOTE_FFLAGSMASK;
            break;
        case NOTE_FFOR:
            knote.fflags |= change.fflags & NOTE_FFLAGSMASK;
            break;
        case NOTE_FFCOPY:
            knote.fflags = change.fflags & NOTE_FFLAGSMASK;
            break;
        default:
            break;
        }
        if (change.fflags & NOTE_TRIGGER)
            knote.triggered = true;
        knote.data = change.data;
    }
    return {};
}

bool KQueue::check(Process& process, Knote& knote, vulture_kevent& event, bool& remove, u64 now, u64 generation)
{
    event = {};
    event.ident = knote.ident;
    event.filter = knote.filter;
    event.udata = knote.udata;
    remove = false;

    // A descriptor that's been closed takes what was watching it along with it.
    RefPtr<FileDescription> description;
    if (knote.description_serial) {
        auto current = process.file(static_cast<int>(knote.ident));
        if (current.is_error() || current.value()->serial() != knote.description_serial) {
            remove = true;
            return false;
        }
        description = current.release_value();
    }

    switch (knote.filter) {
    case EVFILT_READ:
    case EVFILT_WRITE: {
        bool const reading = knote.filter == EVFILT_READ;
        if (description->is_hung_up())
            event.flags |= EV_EOF;
        if (!(reading ? description->can_read() : description->can_write()) && !(event.flags & EV_EOF))
            return false;
        // (A port doesn't touch the generation when it changes: those it's told directly, and they say so again each time.)
        if ((knote.flags & EV_CLEAR) && !description->port()) {
            if (knote.reported && knote.reported_at_generation == generation)
                return false;
            knote.reported = true;
            knote.reported_at_generation = generation;
        }
        event.data = static_cast<long>(reading ? description->bytes_to_read() : description->room_to_write());
        break;
    }
    case EVFILT_VNODE: {
        auto const metadata = description->inode()->metadata();
        auto const modified = metadata.modification_time_ns ? metadata.modification_time_ns : metadata.modification_time;
        unsigned int what = 0;
        if (metadata.link_count == 0 && knote.last_links != 0)
            what |= NOTE_DELETE;
        else if (metadata.link_count != knote.last_links)
            what |= NOTE_LINK;
        if (static_cast<long long>(metadata.size) > knote.last_size)
            what |= NOTE_EXTEND;
        if (modified != knote.last_modified || static_cast<long long>(metadata.size) != knote.last_size)
            what |= NOTE_WRITE;
        if (metadata.change_time_ns != knote.last_changed && !(what & NOTE_WRITE))
            what |= NOTE_ATTRIB;
        knote.last_size = static_cast<long long>(metadata.size);
        knote.last_modified = modified;
        knote.last_changed = metadata.change_time_ns;
        knote.last_links = metadata.link_count;
        what &= knote.fflags;
        if (!what)
            return false;
        event.fflags = what;
        break;
    }
    case EVFILT_PROC: {
        auto target = Process::from_pid(static_cast<ProcessID>(knote.ident));
        unsigned int what = 0;
        if (target) {
            if (target->fork_count() != knote.last_forks)
                what |= NOTE_FORK;
            if (target->exec_count() != knote.last_execs)
                what |= NOTE_EXEC;
            knote.last_forks = target->fork_count();
            knote.last_execs = target->exec_count();
        }
        bool const ended = !target || target->has_exited();
        if (ended)
            what |= NOTE_EXIT;
        what &= knote.fflags;
        if (ended)
            remove = true;
        if (!what)
            return false;
        event.fflags = what;
        if (what & NOTE_EXIT) {
            event.flags |= EV_EOF;
            if (target && (knote.fflags & NOTE_EXITSTATUS)) {
                event.fflags |= NOTE_EXITSTATUS;
                event.data = target->exit_status();
            }
        }
        break;
    }
    case EVFILT_SIGNAL: {
        u32 const count = process.signal_count(static_cast<int>(knote.ident));
        if (count == knote.last_signal_count)
            return false;
        event.data = static_cast<long>(count - knote.last_signal_count);
        knote.last_signal_count = count;
        break;
    }
    case EVFILT_TIMER: {
        if (now < knote.next_fire)
            return false;
        long const fires = static_cast<long>((now - knote.next_fire) / knote.period) + 1;
        knote.next_fire += static_cast<u64>(fires) * knote.period;
        event.data = fires;
        break;
    }
    case EVFILT_USER:
        if (!knote.triggered)
            return false;
        event.fflags = knote.fflags;
        event.data = knote.data;
        if (knote.flags & EV_CLEAR) {
            knote.triggered = false;
            knote.fflags = 0;
        }
        break;
    default:
        return false;
    }
    if (knote.flags & EV_ONESHOT)
        remove = true;
    if (knote.flags & EV_DISPATCH) {
        knote.enabled = false;
        knote.disabled_by_dispatch = true;
    }
    return true;
}

u64 KQueue::next_timer_deadline() const
{
    u64 next = ~0ull;
    for (auto const& knote : m_knotes) {
        if (knote.filter == EVFILT_TIMER && knote.enabled)
            next = min(next, knote.next_fire);
    }
    return next;
}

bool KQueue::has_vnode_watches() const
{
    for (auto const& knote : m_knotes) {
        if (knote.filter == EVFILT_VNODE && knote.enabled)
            return true;
    }
    return false;
}

bool KQueue::has_pending(Process& process)
{
    MutexLocker locker(m_lock);
    u64 const now = Time::now();
    u64 const generation = poll_generation();
    for (size_t i = 0; i < m_knotes.size(); ++i) {
        // A copy: this is only a look, and it mustn't use anything up.
        Knote copy = m_knotes[i];
        if (!copy.enabled)
            continue;
        vulture_kevent event;
        bool remove = false;
        if (check(process, copy, event, remove, now, generation))
            return true;
    }
    return false;
}

ErrorOr<size_t> KQueue::process(Process& owner, vulture_kevent const* changes, size_t change_count, vulture_kevent* events, size_t event_count, u64 deadline)
{
    size_t produced = 0;
    {
        MutexLocker locker(m_lock);
        for (size_t i = 0; i < change_count; ++i) {
            auto const& change = changes[i];
            auto result = apply(owner, change);
            bool const wants_receipt = change.flags & EV_RECEIPT;
            if (result.is_error() || wants_receipt) {
                int const code = result.is_error() ? (result.error().is_errno() ? result.error().code() : EIO) : 0;
                if (produced >= event_count) {
                    // With no room to say so, the call itself fails, as a Mac's does.
                    if (code)
                        return Error::from_errno(code);
                    continue;
                }
                auto& report = events[produced++];
                report = change;
                report.flags = static_cast<unsigned short>(change.flags | EV_ERROR);
                report.data = code;
            }
        }
    }
    if (event_count == 0)
        return produced;

    for (;;) {
        // Read before looking, so that a change after the look wakes it.
        u64 const generation = poll_generation();
        u64 next_wake = deadline;
        {
            MutexLocker locker(m_lock);
            u64 const now = Time::now();
            for (size_t i = 0; i < m_knotes.size() && produced < event_count;) {
                auto& knote = m_knotes[i];
                if (!knote.enabled) {
                    ++i;
                    continue;
                }
                bool remove = false;
                if (check(owner, knote, events[produced], remove, now, generation))
                    ++produced;
                if (remove)
                    m_knotes.remove(i);
                else
                    ++i;
            }
            if (produced > 0 || deadline == 0)
                return produced;
            next_wake = min(next_wake, next_timer_deadline());
            if (has_vnode_watches())
                next_wake = min(next_wake, now + Time::ticks_from_microseconds(vnode_look_interval_microseconds));
        }
        if (Time::now() >= deadline)
            return produced;
        // Ports don't tell poll_queue, so being woken for one takes them being told who's waiting.
        struct WatchedPort {
            NonnullRefPtr<Port> port;
            bool receive_right;
        };
        Vector<WatchedPort> watched;
        {
            MutexLocker locker(m_lock);
            for (auto const& knote : m_knotes) {
                if (!knote.description_serial || knote.filter == EVFILT_VNODE || !knote.enabled)
                    continue;
                auto description = owner.file(static_cast<int>(knote.ident));
                if (description.is_error() || !description.value()->port())
                    continue;
                bool const receive_right = description.value()->is_port_receive_right();
                if (watched.try_append({ *description.value()->port(), receive_right }).is_error())
                    break;
                description.value()->port()->add_poller(Thread::current(), receive_right);
            }
        }
        auto const result = next_wake == ~0ull
            ? (poll_queue().wait_until_interruptible([&] { return poll_generation() != generation; }) ? WaitQueue::WaitResult::Satisfied : WaitQueue::WaitResult::Interrupted)
            : poll_queue().wait_until_interruptible([&] { return poll_generation() != generation; }, next_wake);
        for (auto& port : watched)
            port.port->remove_poller(Thread::current(), port.receive_right);
        if (result == WaitQueue::WaitResult::Interrupted)
            return Error::from_errno(EINTR);
    }
}

}
