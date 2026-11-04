/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/event.h>
#include <Kernel/Locking/Mutex.h>
#include <LibBase/Error.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Vector.h>

namespace Kernel {

class FileDescription;
class Process;

// kqueue: the things a program is waiting for, in one place, that it asks about all at once. What's registered is looked at
// when something might have changed, the same as poll does, so a knote costs a look each time. That's plenty for what a desktop
// program watches; it's the interface that programs need, and the way it's built can get cleverer behind it.
class KQueue final : public RefCounted<KQueue> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<KQueue>> create();

    // Applies the changes, then waits for up to `event_count` events: `deadline` is a Time::now() value, or ~0 for ever, or 0
    // for not waiting at all. An error with a change that asks to hear of it goes in the events.
    ErrorOr<size_t> process(Process&, vulture_kevent const* changes, size_t change_count, vulture_kevent* events, size_t event_count, u64 deadline);

    // poll() on a kqueue: is there anything to hear about?
    [[nodiscard]] bool has_pending(Process&);

private:
    KQueue() = default;

    struct Knote {
        unsigned long ident { 0 };
        short filter { 0 };
        unsigned short flags { 0 };
        unsigned int fflags { 0 };
        long data { 0 };
        void* udata { nullptr };
        bool enabled { true };
        bool disabled_by_dispatch { false };
        // Which open file it's about: not a reference, or it would never close. The descriptor's looked up each time, and if
        // it's something else now, what was being watched has gone.
        u64 description_serial { 0 };
        // EVFILT_VNODE: what it was like at the last look.
        long long last_size { 0 };
        long long last_modified { 0 };
        long long last_changed { 0 };
        // EVFILT_PROC
        u32 last_forks { 0 };
        u32 last_execs { 0 };
        u32 last_links { 0 };
        // EVFILT_SIGNAL: how many had been sent at the last look.
        u32 last_signal_count { 0 };
        // EVFILT_TIMER
        u64 period { 0 };
        u64 next_fire { 0 };
        // EVFILT_USER
        bool triggered { false };
        // EV_CLEAR on a read or write: having said so, it says so again only once something's changed. "Something" is anything in
        // the system, which means saying it now and then when nothing has here; never not saying it when it has.
        u64 reported_at_generation { 0 };
        bool reported { false };
    };

    ErrorOr<void> apply(Process&, vulture_kevent const&);
    // True if it should be reported, and what to say.
    bool check(Process&, Knote&, vulture_kevent& event, bool& remove, u64 now, u64 generation);
    [[nodiscard]] u64 next_timer_deadline() const;
    [[nodiscard]] bool has_vnode_watches() const;

    Mutex m_lock;
    Vector<Knote> m_knotes;
};

}
