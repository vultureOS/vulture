/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
            {
                SpinLockGuard guard(m_lock);
                dropped = move(m_queue);
                m_count.store(0);
            }
            m_sendable.wake_all();
        }
    } else if (m_senders.fetch_sub(1) == 1) {
        m_receivable.wake_all();
    }
    notify_pollers();
}

ErrorOr<void> Port::send(NonnullOwnPtr<PortMessage> message, bool nonblocking, bool handoff)
{
    OwnPtr<PortMessage> pending = move(message);
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
    // An answer goes straight back to the caller, waiting for nothing else.
    if (handoff || m_is_reply_port)
        m_receivable.wake_one_for_handoff();
    else
        m_receivable.wake_one();
    notify_pollers();
    return {};
}

ErrorOr<NonnullOwnPtr<PortMessage>> Port::receive(bool nonblocking, size_t max_size, size_t max_rights, size_t max_regions, size_t& needed_size)
{
    OwnPtr<PortMessage> message;
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
    notify_pollers();
    return message.release_nonnull();
}

}
