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
#include <Kernel/Net/LocalSocket.h>
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

// A Unix-domain socket's address is its path, where it isn't quite a string: sun_len and the size given can each be anything,
// and it stops at a nul if there is one.
ErrorOr<String> read_local_path(FlatPtr address, size_t size)
{
    constexpr size_t name_offset = 2;
    // (As a Mac allows: larger than the structure says, if it's been made larger.)
    constexpr size_t most = 255;
    if (size <= name_offset || size > most)
        return Error::from_errno(EINVAL);
    char raw[most];
    TRY(copy_from_user(raw, address, size));
    if (static_cast<u8>(raw[1]) != AF_UNIX)
        return Error::from_errno(EAFNOSUPPORT);
    size_t length = 0;
    while (name_offset + length < size && raw[name_offset + length])
        ++length;
    if (length == 0)
        return Error::from_errno(EINVAL);
    return String::from_view({ raw + name_offset, length });
}

ErrorOr<void> write_local_address(StringView path, FlatPtr buffer, FlatPtr size_address)
{
    if (!buffer || !size_address)
        return {};
    vulture_sockaddr_un raw {};
    raw.sun_family = AF_UNIX;
    size_t const copied = min(path.length(), sizeof(raw.sun_path) - 1);
    if (copied)
        __builtin_memcpy(raw.sun_path, path.characters_without_null_termination(), copied);
    size_t const whole = __builtin_offsetof(vulture_sockaddr_un, sun_path) + (copied ? copied + 1 : 0);
    raw.sun_len = static_cast<u8>(whole);
    auto room = TRY(copy_from_user<u32>(size_address));
    TRY(copy_to_user(buffer, &raw, min<size_t>(room, whole)));
    TRY(copy_to_user(size_address, static_cast<u32>(whole)));
    return {};
}

// However much the buffers of a message come to, read in one go.
constexpr size_t most_in_a_message = 4 * MiB;

ErrorOr<Vector<vulture_iovec>> read_message_vectors(vulture_msghdr const& header, size_t& total)
{
    if (header.msg_iovlen < 0 || header.msg_iovlen > VULTURE_IOV_MAX)
        return Error::from_errno(EMSGSIZE);
    Vector<vulture_iovec> vectors;
    TRY(vectors.try_resize(static_cast<size_t>(header.msg_iovlen)));
    if (header.msg_iovlen)
        TRY(copy_from_user(vectors.data(), reinterpret_cast<FlatPtr>(header.msg_iov), vectors.size() * sizeof(vulture_iovec)));
    u64 sum = 0;
    for (auto const& vector : vectors) {
        if (__builtin_add_overflow(sum, vector.iov_len, &sum) || sum > 0x7fffffffull)
            return Error::from_errno(EINVAL);
        if (!is_user_range(reinterpret_cast<FlatPtr>(vector.iov_base), vector.iov_len))
            return Error::from_errno(EFAULT);
    }
    total = sum;
    return vectors;
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
    if (domain == AF_UNIX) {
        // What's checked is where it goes, which is a file.
        if (protocol != 0)
            return Error::from_errno(EPROTONOSUPPORT);
        auto local = TRY(LocalSocket::create(type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)));
        auto description = TRY(FileDescription::create_for_local_socket(move(local)));
        if (nonblocking)
            description->set_status_flags(O_NONBLOCK);
        return TRY(allocate_fd(move(description), close_on_exec));
    }
    // Either will do for now: what it is used for is checked then.
    if (m_sandbox && !m_sandbox->allows(SandboxOperation::NetworkOutbound, {}) && !m_sandbox->allows(SandboxOperation::NetworkInbound, {}))
        TRY(sandbox_check(SandboxOperation::NetworkOutbound, {}));
    auto socket = TRY(Net::Socket::create(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol));
    auto description = TRY(FileDescription::create_for_socket(move(socket)));
    if (nonblocking)
        description->set_status_flags(O_NONBLOCK);
    return TRY(allocate_fd(move(description), close_on_exec));
}

ErrorOr<FlatPtr> Process::sys_socketpair(TrapFrame&, SyscallArguments const& arguments)
{
    int const domain = as_int(arguments.values[0]);
    int const type = as_int(arguments.values[1]);
    if (domain != AF_UNIX)
        return Error::from_errno(EOPNOTSUPP);
    if (as_int(arguments.values[2]) != 0)
        return Error::from_errno(EPROTONOSUPPORT);
    RefPtr<LocalSocket> first, second;
    TRY(LocalSocket::create_pair(type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), first, second));
    auto first_description = TRY(FileDescription::create_for_local_socket(*first));
    auto second_description = TRY(FileDescription::create_for_local_socket(*second));
    if (type & SOCK_NONBLOCK) {
        first_description->set_status_flags(O_NONBLOCK);
        second_description->set_status_flags(O_NONBLOCK);
    }
    int fds[2];
    fds[0] = TRY(allocate_fd(move(first_description), type & SOCK_CLOEXEC));
    auto other = allocate_fd(move(second_description), type & SOCK_CLOEXEC);
    if (other.is_error()) {
        release_fd(fds[0]);
        return other.release_error();
    }
    fds[1] = other.value();
    if (auto copied = copy_to_user(arguments.values[3], fds); copied.is_error()) {
        release_fd(fds[0]);
        release_fd(fds[1]);
        return copied.release_error();
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_bind(TrapFrame&, SyscallArguments const& arguments)
{
    if (auto description = TRY(file(as_int(arguments.values[0]))); description->local_socket()) {
        auto path = TRY(read_local_path(arguments.values[1], arguments.values[2]));
        TRY(description->local_socket()->bind(path.view(), current_directory().view(), 0777 & ~m_umask));
        return 0;
    }
    TRY(sandbox_check(SandboxOperation::NetworkInbound, {}));
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    auto address = TRY(read_address(arguments.values[1], arguments.values[2]));
    TRY(socket->bind(address));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_listen(TrapFrame&, SyscallArguments const& arguments)
{
    if (auto description = TRY(file(as_int(arguments.values[0]))); description->local_socket()) {
        TRY(description->local_socket()->listen(as_int(arguments.values[1])));
        return 0;
    }
    TRY(sandbox_check(SandboxOperation::NetworkInbound, {}));
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(socket->listen(as_int(arguments.values[1])));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_accept(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (auto* local = description->local_socket()) {
        int const flags = as_int(arguments.values[3]);
        auto accepted = TRY(local->accept(description->flags() & O_NONBLOCK));
        auto peer = accepted->peer_path();
        TRY(write_local_address(peer.is_error() ? StringView {} : peer.value().view(), arguments.values[1], arguments.values[2]));
        auto new_description = TRY(FileDescription::create_for_local_socket(move(accepted)));
        if (flags & SOCK_NONBLOCK)
            new_description->set_status_flags(O_NONBLOCK);
        return TRY(allocate_fd(move(new_description), flags & SOCK_CLOEXEC));
    }
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
    auto description = TRY(file(as_int(arguments.values[0])));
    if (auto* local = description->local_socket()) {
        auto path = TRY(read_local_path(arguments.values[1], arguments.values[2]));
        TRY(local->connect(path.view(), current_directory().view()));
        return 0;
    }
    TRY(sandbox_check(SandboxOperation::NetworkOutbound, {}));
    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    auto address = TRY(read_address(arguments.values[1], arguments.values[2]));
    TRY(socket->connect(address, description->flags() & O_NONBLOCK));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_sendto(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    if (auto* local = description->local_socket()) {
        int const flags = as_int(arguments.values[3]);
        String to;
        if (arguments.values[4])
            to = TRY(read_local_path(arguments.values[4], arguments.values[5]));
        auto sent = local->send(IOBuffer::user(arguments.values[1]), arguments.values[2], (description->flags() & O_NONBLOCK) || (flags & MSG_DONTWAIT), {}, to.view(), current_directory().view());
        if (sent.is_error()) {
            if (sent.error().code() == EPIPE && !(flags & MSG_NOSIGNAL) && !local->wants_no_sigpipe()) {
                vulture_siginfo info {};
                send_signal(SIGPIPE, info);
            }
            return sent.release_error();
        }
        return sent.value();
    }
    TRY(sandbox_check(SandboxOperation::NetworkOutbound, {}));
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
    if (auto* local = description->local_socket()) {
        int const flags = as_int(arguments.values[3]);
        size_t const count = arguments.values[2];
        bool const nonblocking = (description->flags() & O_NONBLOCK) || (flags & MSG_DONTWAIT);
        String from;
        size_t done = 0;
        do {
            // (Files that come with it have nowhere to go, here, and close.)
            LocalSocket::Rights rights;
            bool truncated = false;
            auto got = local->receive(IOBuffer::user(arguments.values[1] + done), count - done, nonblocking, flags & MSG_PEEK, rights, truncated, &from);
            if (got.is_error()) {
                if (done)
                    break;
                return got.release_error();
            }
            done += got.value();
            if (got.value() == 0)
                break;
        } while ((flags & MSG_WAITALL) && local->type() == SOCK_STREAM && done < count);
        TRY(write_local_address(from.view(), arguments.values[4], arguments.values[5]));
        return done;
    }
    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    Net::SocketAddress from;
    size_t got = TRY(socket->receive(IOBuffer::user(arguments.values[1]), arguments.values[2], as_int(arguments.values[3]), &from, description->flags() & O_NONBLOCK));
    TRY(write_address(from, arguments.values[4], arguments.values[5]));
    return got;
}

ErrorOr<FlatPtr> Process::sys_sendmsg(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    auto const header = TRY(copy_from_user<vulture_msghdr>(arguments.values[1]));
    int const flags = as_int(arguments.values[2]);
    size_t total = 0;
    auto const vectors = TRY(read_message_vectors(header, total));
    if (total > most_in_a_message)
        return Error::from_errno(EMSGSIZE);
    Vector<u8> gathered;
    TRY(gathered.try_resize(total));
    size_t at = 0;
    for (auto const& vector : vectors) {
        if (vector.iov_len)
            TRY(copy_from_user(gathered.data() + at, reinterpret_cast<FlatPtr>(vector.iov_base), vector.iov_len));
        at += vector.iov_len;
    }

    LocalSocket::Rights rights;
    auto const control = reinterpret_cast<FlatPtr>(header.msg_control);
    if (control && header.msg_controllen) {
        size_t offset = 0;
        while (offset + sizeof(vulture_cmsghdr) <= header.msg_controllen) {
            auto const message = TRY(copy_from_user<vulture_cmsghdr>(control + offset));
            if (message.cmsg_len < sizeof(message) || offset + message.cmsg_len > header.msg_controllen)
                return Error::from_errno(EINVAL);
            if (message.cmsg_level != SOL_SOCKET || message.cmsg_type != SCM_RIGHTS)
                return Error::from_errno(EINVAL);
            size_t const count = (message.cmsg_len - sizeof(message)) / sizeof(int);
            if (rights.size() + count > 253)
                return Error::from_errno(EMSGSIZE);
            for (size_t i = 0; i < count; ++i) {
                auto const fd = TRY(copy_from_user<int>(control + offset + sizeof(message) + i * sizeof(int)));
                auto sent_file = TRY(file(fd));
                // A kqueue's about this process, and means nothing to another.
                if (sent_file->kqueue())
                    return Error::from_errno(EBADF);
                TRY(rights.try_append(move(sent_file)));
            }
            offset += (message.cmsg_len + 3) & ~size_t(3);
        }
    }

    bool const nonblocking = (description->flags() & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    auto const buffer = IOBuffer::kernel(gathered.data());
    if (auto* local = description->local_socket()) {
        String to;
        if (header.msg_name && header.msg_namelen)
            to = TRY(read_local_path(reinterpret_cast<FlatPtr>(header.msg_name), header.msg_namelen));
        auto sent = local->send(buffer, total, nonblocking, move(rights), to.view(), current_directory().view());
        if (sent.is_error()) {
            if (sent.error().code() == EPIPE && !(flags & MSG_NOSIGNAL) && !local->wants_no_sigpipe()) {
                vulture_siginfo info {};
                send_signal(SIGPIPE, info);
            }
            return sent.release_error();
        }
        return sent.value();
    }

    auto* socket = description->socket();
    if (!socket)
        return Error::from_errno(ENOTSOCK);
    if (!rights.is_empty())
        return Error::from_errno(EOPNOTSUPP);
    TRY(sandbox_check(SandboxOperation::NetworkOutbound, {}));
    Optional<Net::SocketAddress> to;
    if (header.msg_name && header.msg_namelen)
        to = TRY(read_address(reinterpret_cast<FlatPtr>(header.msg_name), header.msg_namelen));
    auto sent = socket->send(buffer, total, flags, to.has_value() ? &to.value() : nullptr, nonblocking);
    if (sent.is_error()) {
        if (sent.error().code() == EPIPE && !(flags & MSG_NOSIGNAL) && !socket->wants_no_sigpipe()) {
            vulture_siginfo info {};
            send_signal(SIGPIPE, info);
        }
        return sent.release_error();
    }
    return sent.value();
}

ErrorOr<FlatPtr> Process::sys_recvmsg(TrapFrame&, SyscallArguments const& arguments)
{
    auto description = TRY(file(as_int(arguments.values[0])));
    FlatPtr const header_address = arguments.values[1];
    auto const header = TRY(copy_from_user<vulture_msghdr>(header_address));
    int const flags = as_int(arguments.values[2]);
    size_t total = 0;
    auto const vectors = TRY(read_message_vectors(header, total));
    size_t const room = min(total, most_in_a_message);
    Vector<u8> received;
    TRY(received.try_resize(room));
    bool const nonblocking = (description->flags() & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    auto const buffer = IOBuffer::kernel(received.data());
    FlatPtr const name_size_address = header_address + __builtin_offsetof(vulture_msghdr, msg_namelen);
    FlatPtr const name = reinterpret_cast<FlatPtr>(header.msg_name);

    size_t got = 0;
    int result_flags = 0;
    u32 control_used = 0;
    if (auto* local = description->local_socket()) {
        LocalSocket::Rights rights;
        bool truncated = false;
        String from;
        got = TRY(local->receive(buffer, room, nonblocking, flags & MSG_PEEK, rights, truncated, &from));
        if (truncated)
            result_flags |= MSG_TRUNC;
        if (name)
            TRY(write_local_address(from.view(), name, name_size_address));
        if (!rights.is_empty()) {
            auto const control = reinterpret_cast<FlatPtr>(header.msg_control);
            size_t const fit = control && header.msg_controllen > sizeof(vulture_cmsghdr) ? (header.msg_controllen - sizeof(vulture_cmsghdr)) / sizeof(int) : 0;
            size_t const given = min(fit, rights.size());
            if (given < rights.size())
                result_flags |= MSG_CTRUNC;
            if (given) {
                Vector<int> fds;
                TRY(fds.try_ensure_capacity(given));
                for (size_t i = 0; i < given; ++i) {
                    auto fd = allocate_fd(rights[i], false);
                    if (fd.is_error()) {
                        for (int opened : fds)
                            release_fd(opened);
                        return fd.release_error();
                    }
                    MUST(fds.try_append(fd.value()));
                }
                vulture_cmsghdr message {};
                message.cmsg_len = static_cast<u32>(sizeof(message) + given * sizeof(int));
                message.cmsg_level = SOL_SOCKET;
                message.cmsg_type = SCM_RIGHTS;
                TRY(copy_to_user(control, message));
                TRY(copy_to_user(control + sizeof(message), fds.data(), given * sizeof(int)));
                control_used = message.cmsg_len;
            }
        }
    } else {
        auto* socket = description->socket();
        if (!socket)
            return Error::from_errno(ENOTSOCK);
        Net::SocketAddress from;
        got = TRY(socket->receive(buffer, room, flags, &from, nonblocking));
        if (name)
            TRY(write_address(from, name, name_size_address));
    }

    // Out to where each piece of it goes.
    size_t at = 0;
    for (auto const& vector : vectors) {
        if (at == got)
            break;
        size_t const chunk = min<size_t>(vector.iov_len, got - at);
        if (chunk)
            TRY(copy_to_user(reinterpret_cast<FlatPtr>(vector.iov_base), received.data() + at, chunk));
        at += chunk;
    }
    TRY(copy_to_user(header_address + __builtin_offsetof(vulture_msghdr, msg_controllen), control_used));
    TRY(copy_to_user(header_address + __builtin_offsetof(vulture_msghdr, msg_flags), result_flags));
    return got;
}

ErrorOr<FlatPtr> Process::sys_getsockname(TrapFrame&, SyscallArguments const& arguments)
{
    if (auto description = TRY(file(as_int(arguments.values[0]))); description->local_socket()) {
        auto const path = description->local_socket()->local_path();
        TRY(write_local_address(path.view(), arguments.values[1], arguments.values[2]));
        return 0;
    }
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(write_address(socket->local_address(), arguments.values[1], arguments.values[2]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getpeername(TrapFrame&, SyscallArguments const& arguments)
{
    if (auto description = TRY(file(as_int(arguments.values[0]))); description->local_socket()) {
        auto const path = TRY(description->local_socket()->peer_path());
        TRY(write_local_address(path.view(), arguments.values[1], arguments.values[2]));
        return 0;
    }
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    auto remote = TRY(socket->remote_address());
    TRY(write_address(remote, arguments.values[1], arguments.values[2]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_setsockopt(TrapFrame&, SyscallArguments const& arguments)
{
    if (auto description = TRY(file(as_int(arguments.values[0]))); description->local_socket()) {
        int const level = as_int(arguments.values[1]);
        int const name = as_int(arguments.values[2]);
        if (level != SOL_SOCKET)
            return Error::from_errno(ENOPROTOOPT);
        // The rest there's nothing to do with, but nothing wrong with asking.
        if (name == SO_NOSIGPIPE)
            description->local_socket()->set_no_sigpipe(TRY(copy_from_user<int>(arguments.values[3])) != 0);
        return 0;
    }
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(socket->set_option(as_int(arguments.values[1]), as_int(arguments.values[2]), arguments.values[3], arguments.values[4]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getsockopt(TrapFrame&, SyscallArguments const& arguments)
{
    if (auto description = TRY(file(as_int(arguments.values[0]))); description->local_socket()) {
        auto* local = description->local_socket();
        int const level = as_int(arguments.values[1]);
        int const name = as_int(arguments.values[2]);
        FlatPtr const value = arguments.values[3];
        FlatPtr const size_address = arguments.values[4];
        auto const room = TRY(copy_from_user<u32>(size_address));
        if (level == VULTURE_SOL_LOCAL && name == VULTURE_LOCAL_PEERCRED) {
            u32 uid = 0, gid = 0;
            TRY(local->peer_credentials(uid, gid));
            vulture_xucred credentials {};
            credentials.cr_uid = uid;
            credentials.cr_ngroups = 1;
            credentials.cr_groups[0] = gid;
            TRY(copy_to_user(value, &credentials, min<size_t>(room, sizeof(credentials))));
            TRY(copy_to_user(size_address, static_cast<u32>(sizeof(credentials))));
            return 0;
        }
        if (level != SOL_SOCKET)
            return Error::from_errno(ENOPROTOOPT);
        int answer = 0;
        switch (name) {
        case SO_TYPE:
            answer = local->type();
            break;
        case SO_ERROR:
            break;
        case SO_NOSIGPIPE:
            answer = local->wants_no_sigpipe();
            break;
        case SO_SNDBUF:
        case SO_RCVBUF:
            answer = static_cast<int>(local->buffer_size());
            break;
        default:
            return Error::from_errno(ENOPROTOOPT);
        }
        TRY(copy_to_user(value, &answer, min<size_t>(room, sizeof(answer))));
        TRY(copy_to_user(size_address, static_cast<u32>(sizeof(answer))));
        return 0;
    }
    auto socket = TRY(socket_for(as_int(arguments.values[0])));
    TRY(socket->get_option(as_int(arguments.values[1]), as_int(arguments.values[2]), arguments.values[3], arguments.values[4]));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_shutdown(TrapFrame&, SyscallArguments const& arguments)
{
    if (auto description = TRY(file(as_int(arguments.values[0]))); description->local_socket()) {
        TRY(description->local_socket()->shutdown(as_int(arguments.values[1])));
        return 0;
    }
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
