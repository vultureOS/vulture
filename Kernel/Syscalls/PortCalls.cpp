/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/files.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/API/POSIX/port.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/IPC/Port.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>

// Ports (see Kernel/API/POSIX/port.h and Kernel/IPC/Port.h).

namespace Kernel {

namespace {

int as_int(u64 value)
{
    return static_cast<int>(static_cast<i32>(value));
}

}

// Closes `fd`, dropping the description only after the table's lock is
// let go (its last reference may detach a port, which wakes waiters).
void Process::release_fd(int fd)
{
    FileDescriptor closing;
    {
        SpinLockGuard guard(m_files_lock);
        if (fd >= 0 && static_cast<size_t>(fd) < max_open_files)
            closing = move(m_files[fd]);
    }
}

ErrorOr<NonnullOwnPtr<PortMessage>> Process::build_port_message(FlatPtr user_message, bool& move_rights, Vector<int>& sent_fds,
    Vector<vulture_port_region>& sent_regions)
{
    auto header = TRY(copy_from_user<vulture_port_message>(user_message));
    if (header.size > VULTURE_PORT_MAX_INLINE || header.right_count > VULTURE_PORT_MAX_RIGHTS || header.region_count > VULTURE_PORT_MAX_REGIONS)
        return Error::from_errno(EMSGSIZE);
    auto* message = new (Base::nothrow) PortMessage;
    if (!message)
        return Error::from_errno(ENOMEM);
    auto owned = adopt_own(*message);
    message->id = header.id;
    message->flags = header.flags & ~VULTURE_PORT_MOVE;
    move_rights = header.flags & VULTURE_PORT_MOVE;
    message->sender_pid = m_pid;
    message->sender_uid = m_credentials.euid;
    message->sender_gid = m_credentials.egid;

    if (header.size > 0) {
        message->bytes = static_cast<u8*>(kmalloc(header.size));
        if (!message->bytes)
            return Error::from_errno(ENOMEM);
        message->size = header.size;
        TRY(copy_from_user(message->bytes, reinterpret_cast<FlatPtr>(header.bytes), header.size));
    }
    if (header.right_count > 0) {
        int fds[VULTURE_PORT_MAX_RIGHTS];
        TRY(copy_from_user(fds, reinterpret_cast<FlatPtr>(header.rights), header.right_count * sizeof(int)));
        for (u32 i = 0; i < header.right_count; ++i) {
            TRY(message->rights.try_append(TRY(file(fds[i]))));
            TRY(sent_fds.try_append(fds[i]));
        }
    }
    if (header.reply >= 0) {
        auto reply = TRY(file(header.reply));
        if (!reply->is_port_send_right())
            return Error::from_errno(EINVAL);
        message->reply = move(reply);
    }
    if (header.region_count > 0) {
        vulture_port_region regions[VULTURE_PORT_MAX_REGIONS];
        TRY(copy_from_user(regions, reinterpret_cast<FlatPtr>(header.regions), header.region_count * sizeof(vulture_port_region)));
        for (u32 i = 0; i < header.region_count; ++i) {
            auto address = reinterpret_cast<FlatPtr>(regions[i].address);
            auto object = TRY(m_address_space->copy_range(address, regions[i].size));
            TRY(message->regions.try_append({ move(object), regions[i].size }));
            TRY(sent_regions.try_append(regions[i]));
        }
    }
    return owned;
}

// What a successful send takes from the sender with VULTURE_PORT_MOVE.
void Process::finish_move(bool move_rights, Vector<int> const& sent_fds, Vector<vulture_port_region> const& sent_regions)
{
    if (!move_rights)
        return;
    for (int fd : sent_fds)
        release_fd(fd);
    for (auto const& region : sent_regions)
        (void)m_address_space->unmap(reinterpret_cast<FlatPtr>(region.address), region.size);
}

ErrorOr<void> Process::deliver_port_message(Port& port, bool nonblocking, FlatPtr user_received)
{
    auto received = TRY(copy_from_user<vulture_port_received>(user_received));
    size_t needed = 0;
    auto taken = port.receive(nonblocking, received.capacity, received.right_capacity, received.region_capacity, needed);
    if (taken.is_error()) {
        if (taken.error().code() == EMSGSIZE) {
            received.size = needed;
            TRY(copy_to_user(user_received, received));
        }
        return taken.release_error();
    }
    auto message = taken.release_value();

    if (message->size)
        TRY(copy_to_user(reinterpret_cast<FlatPtr>(received.bytes), message->bytes, message->size));
    received.id = message->id;
    received.flags = message->flags;
    received.size = message->size;
    received.sender_pid = message->sender_pid;
    received.sender_uid = message->sender_uid;
    received.sender_gid = message->sender_gid;

    // Rights become our descriptors (closed on exec, as ports should be).
    int installed[VULTURE_PORT_MAX_RIGHTS + 1];
    size_t installed_count = 0;
    auto undo = [&] {
        for (size_t i = 0; i < installed_count; ++i)
            release_fd(installed[i]);
    };
    for (auto& right : message->rights) {
        auto fd = allocate_fd(right, true);
        if (fd.is_error()) {
            undo();
            return fd.release_error();
        }
        installed[installed_count++] = fd.value();
    }
    received.right_count = static_cast<unsigned>(message->rights.size());
    if (received.right_count) {
        if (auto result = copy_to_user(reinterpret_cast<FlatPtr>(received.rights), installed, received.right_count * sizeof(int)); result.is_error()) {
            undo();
            return result.release_error();
        }
    }
    received.reply = -1;
    if (message->reply) {
        auto fd = allocate_fd(*message->reply, true);
        if (fd.is_error()) {
            undo();
            return fd.release_error();
        }
        installed[installed_count++] = fd.value();
        received.reply = fd.value();
    }
    // Out-of-line memory: mapped here, sharing the sender's pages until
    // either side writes.
    vulture_port_region regions[VULTURE_PORT_MAX_REGIONS];
    received.region_count = static_cast<unsigned>(message->regions.size());
    for (size_t i = 0; i < message->regions.size(); ++i) {
        auto& region = message->regions[i];
        auto address = m_address_space->map_object(region.object, region.size, PROT_READ | PROT_WRITE, "port memory"_sv);
        if (address.is_error()) {
            undo();
            return address.release_error();
        }
        regions[i] = { reinterpret_cast<void*>(address.value()), region.size };
    }
    if (received.region_count)
        TRY(copy_to_user(reinterpret_cast<FlatPtr>(received.regions), regions, received.region_count * sizeof(vulture_port_region)));
    TRY(copy_to_user(user_received, received));
    return {};
}

ErrorOr<FlatPtr> Process::sys_port_create(TrapFrame&, SyscallArguments const& arguments)
{
    int flags = as_int(arguments.values[1]);
    auto port = TRY(Port::create());
    auto receive = TRY(FileDescription::create_for_port(port, true));
    auto send = TRY(FileDescription::create_for_port(port, false));
    int fds[2];
    fds[0] = TRY(allocate_fd(receive, flags & O_CLOEXEC));
    auto second = allocate_fd(send, flags & O_CLOEXEC);
    if (second.is_error()) {
        release_fd(fds[0]);
        return second.release_error();
    }
    fds[1] = second.value();
    if (auto result = copy_to_user(arguments.values[0], fds); result.is_error()) {
        release_fd(fds[0]);
        release_fd(fds[1]);
        return result.release_error();
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_port_make_send(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (!description->is_port_receive_right())
        return Error::from_errno(EINVAL);
    auto send = TRY(FileDescription::create_for_port(*description->port(), false));
    return static_cast<FlatPtr>(TRY(allocate_fd(send, as_int(arguments.values[1]) & O_CLOEXEC)));
}

ErrorOr<FlatPtr> Process::sys_port_send(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (!description->is_port_send_right())
        return Error::from_errno(description->port() ? EINVAL : ENOTSOCK);
    bool move_rights = false;
    Vector<int> sent_fds;
    Vector<vulture_port_region> sent_regions;
    auto message = TRY(build_port_message(arguments.values[1], move_rights, sent_fds, sent_regions));
    bool nonblocking = (as_int(arguments.values[2]) & VULTURE_PORT_NONBLOCK) || (description->flags() & O_NONBLOCK);
    TRY(description->port()->send(move(message), nonblocking, false));
    finish_move(move_rights, sent_fds, sent_regions);
    return 0;
}

ErrorOr<FlatPtr> Process::sys_port_receive(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (!description->is_port_receive_right())
        return Error::from_errno(description->port() ? EINVAL : ENOTSOCK);
    bool nonblocking = (as_int(arguments.values[2]) & VULTURE_PORT_NONBLOCK) || (description->flags() & O_NONBLOCK);
    TRY(deliver_port_message(*description->port(), nonblocking, arguments.values[1]));
    return 0;
}

// A request, and its answer: the message goes with a send right to a port
// of its own, the receiver runs next on this processor (direct handoff),
// and the call waits for the answer there. EPIPE if the receiver lets the
// right go without answering.
ErrorOr<FlatPtr> Process::sys_port_call(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (!description->is_port_send_right())
        return Error::from_errno(description->port() ? EINVAL : ENOTSOCK);
    auto header = TRY(copy_from_user<vulture_port_message>(arguments.values[1]));
    if (header.reply >= 0)
        return Error::from_errno(EINVAL);
    bool move_rights = false;
    Vector<int> sent_fds;
    Vector<vulture_port_region> sent_regions;
    auto message = TRY(build_port_message(arguments.values[1], move_rights, sent_fds, sent_regions));
    auto answers = TRY(Port::create_for_reply());
    // Ours, outside any descriptor table: only the call receives on it.
    auto answers_receive = TRY(FileDescription::create_for_port(answers, true));
    message->reply = TRY(FileDescription::create_for_port(answers, false));
    bool nonblocking = as_int(arguments.values[3]) & VULTURE_PORT_NONBLOCK;
    TRY(description->port()->send(move(message), nonblocking, true));
    finish_move(move_rights, sent_fds, sent_regions);
    TRY(deliver_port_message(*answers, false, arguments.values[2]));
    return 0;
}

}
