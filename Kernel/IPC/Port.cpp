/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Tasks/Process.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/IPC/Port.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Tasks/Scheduler.h>

namespace Kernel {

PortMessage::~PortMessage()
{
    if (bytes)
        kfree(bytes);
}

ErrorOr<NonnullRefPtr<Port>> Port::create(size_t capacity, bool reply)
{
    auto* port = new (Base::nothrow) Port;
    if (!port)
        return Error::from_errno(ENOMEM);
    auto reference = adopt_ref(*port);
    reference->m_capacity = capacity;
    reference->m_is_reply_port = reply;
    TRY(reference->m_queue.try_ensure_capacity(capacity));
    return reference;
}

ErrorOr<NonnullRefPtr<Port>> Port::create()
{
    return create(queue_limit, false);
}

ErrorOr<NonnullRefPtr<Port>> Port::create_for_reply()
{
    return create(1, true);
}

void Port::attach(bool receive_right)
{
    (receive_right ? m_receivers : m_senders).fetch_add(1);
}

void Port::detach(bool receive_right)
{
    if (receive_right) {
        if (m_receivers.fetch_sub(1) == 1) {
            // Nobody will take what is queued: let the rights in it go
            // (which may end other conversations in turn).
            Vector<OwnPtr<PortMessage>> dropped;
            RefPtr<Process> last_receiver;
            {
                SpinLockGuard guard(m_lock);
                dropped = move(m_queue);
                m_count.store(0);
                last_receiver = move(m_last_receiver_process);
                m_last_receiver_thread = 0;
            }
            m_sendable.wake_all();
            // Senders now see the hang-up.
            wake_pollers(false, false);
        }
    } else if (m_senders.fetch_sub(1) == 1) {
        m_receivable.wake_all();
        // The receiver now sees the end.
        wake_pollers(true, false);
    }
}

void Port::note_receiver(Thread& thread)
{
    // (What it replaces goes once the lock's let go: it could be the last of that process.)
    RefPtr<Process> previous;
    SpinLockGuard guard(m_lock);
    if (m_last_receiver_thread == thread.id())
        return;
    previous = move(m_last_receiver_process);
    m_last_receiver_process = thread.process();
    m_last_receiver_thread = thread.id();
}

RefPtr<Thread> Port::last_receiver()
{
    RefPtr<Process> process;
    u64 id;
    {
        SpinLockGuard guard(m_lock);
        process = m_last_receiver_process;
        id = m_last_receiver_thread;
    }
    return process ? process->thread_with_id(id) : RefPtr<Thread> {};
}

void Port::add_poller(Thread& thread, bool receive_right)
{
    if (receive_right)
        note_receiver(thread);
    SpinLockGuard guard(m_poller_lock);
    auto& pollers = receive_right ? m_receive_pollers : m_send_pollers;
    for (auto& slot : pollers.threads) {
        if (!slot) {
            slot = thread;
            return;
        }
    }
    ++pollers.overflowed;
}

void Port::remove_poller(Thread& thread, bool receive_right)
{
    SpinLockGuard guard(m_poller_lock);
    auto& pollers = receive_right ? m_receive_pollers : m_send_pollers;
    for (auto& slot : pollers.threads) {
        if (slot.ptr() == &thread) {
            slot = nullptr;
            return;
        }
    }
    if (pollers.overflowed > 0)
        --pollers.overflowed;
}

void Port::wake_pollers(bool receivers, bool handoff)
{
    RefPtr<Thread> threads[max_pollers];
    bool overflow;
    {
        SpinLockGuard guard(m_poller_lock);
        auto& pollers = receivers ? m_receive_pollers : m_send_pollers;
        for (size_t i = 0; i < max_pollers; ++i)
            threads[i] = pollers.threads[i];
        overflow = pollers.overflowed > 0;
    }
    if (overflow) {
        notify_pollers();
        return;
    }
    note_poll_change();
    bool handed_off = false;
    for (auto& thread : threads) {
        if (!thread)
            continue;
        poll_queue().wake_thread(*thread, handoff && !handed_off);
        handed_off = true;
    }
}

ErrorOr<void> Port::send(NonnullOwnPtr<PortMessage> message, bool nonblocking, bool handoff)
{
    OwnPtr<PortMessage> pending = move(message);
    auto const sender_qos = pending->sender_qos;
    for (;;) {
        {
            SpinLockGuard guard(m_lock);
            if (m_receivers.load() == 0)
                return Error::from_errno(EPIPE);
            if (m_queue.size() < m_capacity && m_queue.capacity() > m_queue.size()) {
                MUST(m_queue.try_append(move(pending)));
                m_count.store(m_queue.size());
                break;
            }
        }
        if (nonblocking)
            return Error::from_errno(EAGAIN);
        if (!m_sendable.wait_until_interruptible([&] { return m_count.load() < m_capacity || m_receivers.load() == 0; }))
            return Error::from_errno(EINTR);
    }
    // Whoever takes messages from here, if anyone's yet: it's working for the sender now, as fast as that.
    if (auto receiver = last_receiver())
        receiver->raise_message_boost(sender_qos);
    // An answer goes straight back to the caller, waiting for nothing else.
    bool direct = handoff || m_is_reply_port;
    if (direct)
        m_receivable.wake_one_for_handoff();
    else
        m_receivable.wake_one();
    // Those polling for it: woken by name (the first, for a call, onto
    // this processor), not every poller in the system.
    wake_pollers(true, direct);
    return {};
}

ErrorOr<NonnullOwnPtr<PortMessage>> Port::receive(bool nonblocking, size_t max_size, size_t max_rights, size_t max_regions, size_t& needed_size)
{
    OwnPtr<PortMessage> message;
    note_receiver(Thread::current());
    for (;;) {
        {
            SpinLockGuard guard(m_lock);
            if (!m_queue.is_empty()) {
                auto& first = *m_queue[0];
                if (first.size > max_size || first.rights.size() > max_rights || first.regions.size() > max_regions) {
                    needed_size = first.size;
                    return Error::from_errno(EMSGSIZE);
                }
                message = move(m_queue[0]);
                m_queue.remove(0);
                m_count.store(m_queue.size());
                break;
            }
            if (m_senders.load() == 0)
                return Error::from_errno(EPIPE);
        }
        if (nonblocking)
            return Error::from_errno(EAGAIN);
        if (!m_receivable.wait_until_interruptible([&] { return m_count.load() > 0 || m_senders.load() == 0; }))
            return Error::from_errno(EINTR);
    }
    m_sendable.wake_one();
    // Room again, for whoever polls to send.
    wake_pollers(false, false);
    return message.release_nonnull();
}

}
