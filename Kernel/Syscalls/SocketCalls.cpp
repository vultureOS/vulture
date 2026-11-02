/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/files.h>
#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/API/POSIX/socket.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Net/Socket.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Thread.h>

namespace Kernel {

namespace {

int as_int(u64 value)
{
    return static_cast<int>(static_cast<i32>(value));
}

ErrorOr<Net::SocketAddress> read_address(FlatPtr address, size_t size)
{
    if (size < sizeof(sockaddr_in)) {
        // AF_UNSPEC, for undoing a connect, may be as little as this much.
        if (size >= 2) {
            auto family = TRY(copy_from_user<u8>(address + 1));
            if (family == AF_UNSPEC)
                return Net::SocketAddress {};
        }
        return Error::from_errno(EINVAL);
    }
    auto raw = TRY(copy_from_user<sockaddr_in>(address));
    if (raw.sin_family != AF_INET && raw.sin_family != AF_UNSPEC)
        return Error::from_errno(EAFNOSUPPORT);
    if (raw.sin_family == AF_UNSPEC)
        return Net::SocketAddress {};
    return Net::SocketAddress { { __builtin_bswap32(raw.sin_addr.s_addr) }, __builtin_bswap16(raw.sin_port) };
}

// As much of it as the room the caller says it has, and how long it really is. (socklen_t, 32
// bits, as on macOS.)
ErrorOr<void> write_address(Net::SocketAddress const& address, FlatPtr buffer, FlatPtr size_address)
{
    if (!buffer || !size_address)
        return {};
    sockaddr_in raw {};
    raw.sin_len = sizeof(raw);
    raw.sin_family = AF_INET;
    raw.sin_port = __builtin_bswap16(address.port);
    raw.sin_addr.s_addr = __builtin_bswap32(address.address.value);
    auto room = TRY(copy_from_user<u32>(size_address));
    size_t size = min<size_t>(room, sizeof(raw));
    TRY(copy_to_user(buffer, &raw, size));
    TRY(copy_to_user(size_address, static_cast<u32>(sizeof(raw))));
    return {};
}

}

ErrorOr<NonnullRefPtr<Net::Socket>> Process::socket_for(int fd)
{
    auto description = TRY(file(fd));
    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    return NonnullRefPtr<Net::Socket>(*socket);
}

ErrorOr<FlatPtr> Process::sys_socket(TrapFrame&, SyscallArguments const& arguments)
{
    int domain = as_int(arguments.values[0]);
    int type = as_int(arguments.values[1]);
    int protocol = as_int(arguments.values[2]);
    bool close_on_exec = type & SOCK_CLOEXEC;
    bool nonblocking = type & SOCK_NONBLOCK;
    // Either will do for now: what it is used for is checked then.
    if (m_sandbox && !m_sandbox->allows(SandboxOperation::NetworkOutbound, {}) && !m_sandbox->allows(SandboxOperation::NetworkInbound, {}))
        TRY(sandbox_check(SandboxOperation::NetworkOutbound, {}));
    auto socket = TRY(Net::Socket::create(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol));
    auto description = TRY(FileDescription::create_for_socket(move(socket)));
    if (nonblocking)
        description->set_status_flags(O_NONBLOCK);
    return TRY(allocate_fd(move(description), close_on_exec));
}

ErrorOr<FlatPtr> Process::sys_bind(TrapFrame&, SyscallArguments const& arguments)
{
    TRY(sandbox_check(SandboxOperation::NetworkInbound, {}));
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    auto address = TRY(read_address(arguments.values[1], arguments.values[2]));
    TRY(socket->bind(address));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_listen(TrapFrame&, SyscallArguments const& arguments)
{
    TRY(sandbox_check(SandboxOperation::NetworkInbound, {}));
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(socket->listen(as_int(arguments.values[1])));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_accept(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    int flags = as_int(arguments.values[3]);
    Net::SocketAddress peer;
    auto accepted = TRY(socket->accept(description->flags() & O_NONBLOCK, peer));
    TRY(write_address(peer, arguments.values[1], arguments.values[2]));
    auto new_description = TRY(FileDescription::create_for_socket(move(accepted)));
    if (flags & SOCK_NONBLOCK)
        new_description->set_status_flags(O_NONBLOCK);
    return TRY(allocate_fd(move(new_description), flags & SOCK_CLOEXEC));
}

ErrorOr<FlatPtr> Process::sys_connect(TrapFrame&, SyscallArguments const& arguments)
{
    TRY(sandbox_check(SandboxOperation::NetworkOutbound, {}));
    auto description = TRY(file(as_int(arguments.values[0])));
    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    auto address = TRY(read_address(arguments.values[1], arguments.values[2]));
    TRY(socket->connect(address, description->flags() & O_NONBLOCK));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sendto(TrapFrame&, SyscallArguments const& arguments)
{
    TRY(sandbox_check(SandboxOperation::NetworkOutbound, {}));
    auto description = TRY(file(as_int(arguments.values[0])));
    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    int flags = as_int(arguments.values[3]);
    Optional<Net::SocketAddress> to;
    if (arguments.values[4])
        to = TRY(read_address(arguments.values[4], arguments.values[5]));
    auto sent = socket->send(IOBuffer::user(arguments.values[1]), arguments.values[2], flags, to.has_value() ? &to.value() : nullptr, description->flags() & O_NONBLOCK);
    if (sent.is_error()) {
        if (sent.error().code() == EPIPE && !(flags & MSG_NOSIGNAL) && !socket->wants_no_sigpipe()) {
            vulture_siginfo info {};
            send_signal(SIGPIPE, info);
        }
        return sent.release_error();
    }
    return sent.value();
}

ErrorOr<FlatPtr> Process::sys_recvfrom(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    Net::SocketAddress from;
    size_t got = TRY(socket->receive(IOBuffer::user(arguments.values[1]), arguments.values[2], as_int(arguments.values[3]), &from, description->flags() & O_NONBLOCK));
    TRY(write_address(from, arguments.values[4], arguments.values[5]));
    return got;
}

ErrorOr<FlatPtr> Process::sys_getsockname(TrapFrame&, SyscallArguments const& arguments)
{
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(write_address(socket->local_address(), arguments.values[1], arguments.values[2]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getpeername(TrapFrame&, SyscallArguments const& arguments)
{
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    auto remote = TRY(socket->remote_address());
    TRY(write_address(remote, arguments.values[1], arguments.values[2]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_setsockopt(TrapFrame&, SyscallArguments const& arguments)
{
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(socket->set_option(as_int(arguments.values[1]), as_int(arguments.values[2]), arguments.values[3], arguments.values[4]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getsockopt(TrapFrame&, SyscallArguments const& arguments)
{
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(socket->get_option(as_int(arguments.values[1]), as_int(arguments.values[2]), arguments.values[3], arguments.values[4]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_shutdown(TrapFrame&, SyscallArguments const& arguments)
{
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(socket->shutdown(as_int(arguments.values[1])));
    return 0;
}

ErrorOr<FlatPtr> Process::socket_ioctl(unsigned request, FlatPtr argument)
{
    if (request != VULTURE_SIOCSIFCONFIG)
        return Error::from_errno(ENOTTY);
    TRY(sandbox_check(SandboxOperation::NetworkConfigure, {}));
    if (!m_credentials.is_superuser())
        return Error::from_errno(EPERM);
    auto config = TRY(copy_from_user<vulture_interface_config>(argument));
    config.name[sizeof(config.name) - 1] = 0;
    auto& stack = Net::NetworkStack::the();
    MutexLocker locker(stack.lock());
    size_t name_length = 0;
    while (config.name[name_length])
        ++name_length;
    auto* interface = stack.interface_named({ config.name, name_length });
    if (!interface)
        return Error::from_errno(ENXIO);
    interface->address = { __builtin_bswap32(config.address) };
    interface->netmask = { __builtin_bswap32(config.netmask) };
    interface->gateway = { __builtin_bswap32(config.gateway) };
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sandbox_apply(TrapFrame&, SyscallArguments const& arguments)
{
    size_t const size = arguments.values[1];
    if (size == 0 || size > 64 * KiB)
        return Error::from_errno(EINVAL);
    Vector<u8> text;
    TRY(text.try_resize(size));
    TRY(copy_from_user(text.data(), arguments.values[0], size));
    auto profile = TRY(SandboxProfile::parse({ reinterpret_cast<char const*>(text.data()), size }, m_sandbox));
    dmesgln("Sandbox: {}[{}] enters {}", name(), m_pid, profile->name());
    m_sandbox = move(profile);
    return 0;
}

// For vlaunchd, which hands out services: would this process's sandbox let it have this one? Only
// says no in its log if it doesn't, which is what it's asking for.
ErrorOr<FlatPtr> Process::sys_sandbox_check(TrapFrame&, SyscallArguments const& arguments)
{
    auto const pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[0]));
    size_t const size = arguments.values[2];
    if (size == 0 || size > 256)
        return Error::from_errno(EINVAL);
    char name[256];
    TRY(copy_from_user(name, arguments.values[1], size));
    StringView const service { name, size };
    for (auto& process : all()) {
        if (process->pid() != pid)
            continue;
        auto const* sandbox = process->sandbox();
        if (sandbox && !sandbox->allows(SandboxOperation::Service, service)) {
            dmesgln("Sandbox: {}[{}], in {}, denied service-lookup {}", process->name(), pid, sandbox->name(), service);
            return Error::from_errno(EPERM);
        }
        return 0;
    }
    return Error::from_errno(ESRCH);
}

}
