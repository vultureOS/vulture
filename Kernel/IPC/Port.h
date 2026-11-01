/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/AnonymousVMObject.h>
#include <Kernel/Tasks/Thread.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/RefPtr.h>
#include <LibBase/Vector.h>

namespace Kernel {

class FileDescription;

// One message on its way: what the sender gave, held by the kernel until
// the receiver takes it.
struct PortMessage {
    u32 id { 0 };
    u32 flags { 0 };
    u8* bytes { nullptr };
    size_t size { 0 };
    Vector<NonnullRefPtr<FileDescription>> rights;
    RefPtr<FileDescription> reply;
    struct Region {
        NonnullRefPtr<AnonymousVMObject> object;
        size_t size;
    };
    Vector<Region> regions;
    i32 sender_pid { 0 };
    u32 sender_uid { 0 };
    u32 sender_gid { 0 };

    ~PortMessage();
};

// A port's message queue (see Kernel/API/POSIX/port.h). Senders wait while
// it is full, receivers while it is empty. With no receive right left,
// sending fails with EPIPE; with no send right left and nothing queued,
// receiving does too (the conversation is over).
class Port final : public RefCounted<Port> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Port>> create();
    // The one-answer port of a port_call: room for one message, and the
    // answer hands the processor straight back to the waiting caller.
    [[nodiscard]] static ErrorOr<NonnullRefPtr<Port>> create_for_reply();
    [[nodiscard]] bool is_reply_port() const { return m_is_reply_port; }

    // `handoff`: the woken receiver runs next on this processor (port_call,
    // which waits for the answer right after).
    ErrorOr<void> send(NonnullOwnPtr<PortMessage>, bool nonblocking, bool handoff);
    // The first message, if it fits in `max_size` bytes, `max_rights`
    // rights and `max_regions` regions; EMSGSIZE (with `needed_size` set,
    // and the message left queued) if not.
    ErrorOr<NonnullOwnPtr<PortMessage>> receive(bool nonblocking, size_t max_size, size_t max_rights, size_t max_regions, size_t& needed_size);

    void attach(bool receive_right);
    void detach(bool receive_right);

    // Threads in poll() on a right to this port: what changes wakes them
    // by name (a message, onto the sender's processor for a call), not
    // every poller in the system. `receive_right`: polling the receive
    // right (else a send right).
    void add_poller(Thread&, bool receive_right);
    void remove_poller(Thread&, bool receive_right);

    // For poll().
    [[nodiscard]] bool can_receive() const { return m_count.load() > 0 || m_senders.load() == 0; }
    [[nodiscard]] bool can_send() const { return m_count.load() < m_capacity || m_receivers.load() == 0; }
    [[nodiscard]] bool has_senders() const { return m_senders.load() > 0; }
    [[nodiscard]] bool has_receiver() const { return m_receivers.load() > 0; }

    static constexpr size_t queue_limit = 64;

private:
    Port() = default;
    static ErrorOr<NonnullRefPtr<Port>> create(size_t capacity, bool reply);

    // Short holds only, never sleeping: rights are released (and ports
    // detached) from places that hold spinlocks. The queue's room is
    // reserved when the port is made, so queueing never allocates.
    SpinLock m_lock;
    Vector<OwnPtr<PortMessage>> m_queue;
    Atomic<size_t> m_count { 0 };
    Atomic<u32> m_senders { 0 };
    Atomic<u32> m_receivers { 0 };
    WaitQueue m_receivable;
    WaitQueue m_sendable;
    size_t m_capacity { queue_limit };
    bool m_is_reply_port { false };

    // Wakes the pollers of the receive right (`receivers`) or of send
    // rights; the first onto this processor with `handoff`.
    void wake_pollers(bool receivers, bool handoff);

    static constexpr size_t max_pollers = 4;
    struct Pollers {
        RefPtr<Thread> threads[max_pollers];
        // Those that did not fit (everyone is woken then, as for files).
        size_t overflowed { 0 };
    };
    SpinLock m_poller_lock;
    Pollers m_receive_pollers;
    Pollers m_send_pollers;
};

}
