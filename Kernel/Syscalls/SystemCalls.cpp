/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Security/CodeSigning.h>
#include <Kernel/Security/Seal.h>
#include <Kernel/Security/DebugFault.h>
#include <Kernel/API/POSIX/socket.h>
#include <Kernel/Net/NetworkStack.h>
#include <Kernel/Net/Socket.h>
#include <Kernel/Net/TCP.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/poll.h>
#include <Kernel/API/POSIX/sysctl.h>
#include <Kernel/API/POSIX/utsname.h>
#include <Kernel/Arch/Power.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/IPC/Port.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Security/Random.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Time/Time.h>

namespace Kernel {

namespace {

SpinLock s_hostname_lock;
char s_hostname[VULTURE_UTSNAME_LENGTH] = "vulture";

void copy_string(char (&destination)[VULTURE_UTSNAME_LENGTH], StringView source)
{
    size_t length = min(source.length(), sizeof(destination) - 1);
    __builtin_memcpy(destination, source.characters_without_null_termination(), length);
    destination[length] = '\0';
}

}

ErrorOr<FlatPtr> Process::sys_uname(TrapFrame&, SyscallArguments const& arguments)
{
    vulture_utsname name {};
    copy_string(name.sysname, "vultureOS"_sv);
    {
        SpinLockGuard guard(s_hostname_lock);
        copy_string(name.nodename, StringView(s_hostname));
    }
    copy_string(name.release, VULTURE_VERSION_STRING ""_sv);
    copy_string(name.version, "vultureOS " VULTURE_VERSION_STRING " (vultureKernel)"_sv);
    copy_string(name.machine, "arm64"_sv);
    TRY(copy_to_user(arguments.values[0], name));
    return 0;
}

ErrorOr<FlatPtr> Process::sys_getrandom(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr buffer = arguments.values[0];
    size_t length = min<size_t>(arguments.values[1], 256 * KiB);
    size_t done = 0;
    while (done < length) {
        u8 chunk[256];
        size_t piece = min(length - done, sizeof(chunk));
        get_random_bytes({ chunk, piece });
        TRY(copy_to_user(buffer + done, chunk, piece));
        done += piece;
    }
    return done;
}

ErrorOr<FlatPtr> Process::sys_poll(TrapFrame&, SyscallArguments const& arguments)
{
    FlatPtr address = arguments.values[0];
    size_t count = arguments.values[1];
    i64 timeout = static_cast<i32>(arguments.values[2]);
    constexpr size_t max_descriptors = 256;
    if (count > max_descriptors)
        return Error::from_errno(EINVAL);
    vulture_pollfd descriptors[max_descriptors];
    if (count > 0)
        TRY(copy_from_user(descriptors, address, count * sizeof(vulture_pollfd)));
    u64 deadline = timeout < 0 ? ~0ull : Time::now() + Time::ticks_from_microseconds(static_cast<u64>(timeout) * 1000);

    // Ports we poll know we do, and wake us themselves.
    struct WatchedPort {
        NonnullRefPtr<Port> port;
        bool receive_right;
    };
    Vector<WatchedPort> watched_ports;
    for (size_t i = 0; i < count && timeout != 0; ++i) {
        if (descriptors[i].fd < 0)
            continue;
        auto description = file(descriptors[i].fd);
        if (description.is_error() || !description.value()->port())
            continue;
        bool receive_right = description.value()->is_port_receive_right();
        if (watched_ports.try_append({ *description.value()->port(), receive_right }).is_error())
            break;
        description.value()->port()->add_poller(Thread::current(), receive_right);
    }
    struct Unwatch {
        Vector<WatchedPort>& ports;
        ~Unwatch()
        {
            for (auto& watched : ports)
                watched.port->remove_poller(Thread::current(), watched.receive_right);
        }
    } unwatch { watched_ports };

    for (;;) {
        // Read before looking, so a change after the look wakes us.
        u64 generation = poll_generation();
        size_t ready = 0;
        for (size_t i = 0; i < count; ++i) {
            auto& descriptor = descriptors[i];
            descriptor.revents = 0;
            if (descriptor.fd < 0)
                continue;
            auto description = file(descriptor.fd);
            if (description.is_error()) {
                descriptor.revents = POLLNVAL;
            } else {
                auto& open_file = *description.value();
                if ((descriptor.events & (POLLIN | POLLRDNORM)) && open_file.is_readable() && open_file.can_read())
                    descriptor.revents |= descriptor.events & (POLLIN | POLLRDNORM);
                if ((descriptor.events & POLLOUT) && open_file.is_writable() && open_file.can_write())
                    descriptor.revents |= POLLOUT;
                if (open_file.is_hung_up())
                    descriptor.revents |= POLLHUP;
            }
            if (descriptor.revents)
                ++ready;
        }
        if (ready > 0 || timeout == 0) {
            if (count > 0)
                TRY(copy_to_user(address, descriptors, count * sizeof(vulture_pollfd)));
            return ready;
        }
        // No timeout: no timer to arm (and, under a hypervisor, no timer
        // register to write and trap on) for every wait.
        WaitQueue::WaitResult result;
        if (timeout < 0)
            result = poll_queue().wait_until_interruptible([&] { return poll_generation() != generation; }) ? WaitQueue::WaitResult::Satisfied : WaitQueue::WaitResult::Interrupted;
        else
            result = poll_queue().wait_until_interruptible([&] { return poll_generation() != generation; }, deadline);
        if (result == WaitQueue::WaitResult::Interrupted)
            return Error::from_errno(EINTR);
        if (result == WaitQueue::WaitResult::TimedOut) {
            if (count > 0)
                TRY(copy_to_user(address, descriptors, count * sizeof(vulture_pollfd)));
            return 0;
        }
    }
}

static void add_connection_row(Net::Socket& socket, Vector<u8>& rows)
{
    vulture_connection_info info {};
    if (!socket.describe(info))
        return;
    size_t offset = rows.size();
    if (rows.try_resize(offset + sizeof(info)).is_error())
        return;
    __builtin_memcpy(rows.begin() + offset, &info, sizeof(info));
}

// sysctl by name: "kern.ostype", "hw.ncpu"... A value is read into
// `old_buffer` (its size in and out through `old_length_address`), and
// for writable ones (kern.hostname, by root) set from `new_buffer`.
ErrorOr<FlatPtr> Process::sys_sysctl(TrapFrame&, SyscallArguments const& arguments)
{
    char name_buffer[64];
    size_t name_length = arguments.values[1];
    if (name_length == 0 || name_length >= sizeof(name_buffer))
        return Error::from_errno(ENOENT);
    TRY(copy_from_user(name_buffer, arguments.values[0], name_length));
    StringView name { name_buffer, name_length };
    FlatPtr old_buffer = arguments.values[2];
    FlatPtr old_length_address = arguments.values[3];
    FlatPtr new_buffer = arguments.values[4];
    size_t new_length = arguments.values[5];

    // Nothing is changed from inside a sandbox.
    if (new_buffer && m_sandbox)
        return Error::from_errno(EPERM);

    u8 value[128];
    size_t value_length = 0;
    Vector<u8> table; // For the names that give a list.
    ReadonlyBytes result;
    auto set_string = [&](StringView text) {
        value_length = min(text.length(), sizeof(value) - 1);
        __builtin_memcpy(value, text.characters_without_null_termination(), value_length);
        value[value_length++] = '\0';
    };
    auto set_number = [&](auto number) {
        __builtin_memcpy(value, &number, sizeof(number));
        value_length = sizeof(number);
    };
    char hostname[VULTURE_UTSNAME_LENGTH];
    if (name == "kern.ostype"_sv) {
        set_string("vultureOS"_sv);
    } else if (name == "kern.osrelease"_sv) {
        set_string(VULTURE_VERSION_STRING ""_sv);
    } else if (name == "kern.version"_sv) {
        set_string("vultureOS " VULTURE_VERSION_STRING " (vultureKernel, aarch64)"_sv);
    } else if (name == "kern.hostname"_sv) {
        if (new_buffer) {
            if (!m_credentials.is_superuser())
                return Error::from_errno(EPERM);
            if (new_length >= sizeof(hostname))
                return Error::from_errno(EINVAL);
            TRY(copy_from_user(hostname, new_buffer, new_length));
            hostname[new_length] = '\0';
            SpinLockGuard guard(s_hostname_lock);
            __builtin_memcpy(s_hostname, hostname, new_length + 1);
        }
        {
            SpinLockGuard guard(s_hostname_lock);
            __builtin_memcpy(hostname, s_hostname, sizeof(hostname));
        }
        set_string(StringView(hostname));
    } else if (name == "kern.boottime"_sv) {
        u64 uptime_ns = Time::nanoseconds_from_ticks(Time::now());
        set_number(static_cast<i64>((Time::wall_clock_nanoseconds() - uptime_ns) / 1'000'000'000));
    } else if (name == "kern.uptime_ns"_sv) {
        set_number(static_cast<u64>(Time::nanoseconds_from_ticks(Time::now())));
    } else if (name == "kern.maxproc"_sv) {
        set_number(static_cast<i32>(32768));
    } else if (name == "kern.nprocs"_sv) {
        set_number(static_cast<i32>(count()));
    } else if (name == "hw.ncpu"_sv || name == "hw.activecpu"_sv) {
        set_number(static_cast<i32>(Processor::online_count()));
    } else if (name == "hw.memsize"_sv) {
        set_number(static_cast<u64>(physical_memory().statistics().managed_pages * page_size));
    } else if (name == "hw.freemem"_sv) {
        set_number(static_cast<u64>(physical_memory().statistics().free_pages * page_size));
    } else if (name == "hw.pagesize"_sv) {
        set_number(static_cast<i32>(page_size));
    } else if (name == "hw.machine"_sv) {
        set_string("arm64"_sv);
    } else if (name == "kern.proc.all"_sv) {
        for (auto& process : all()) {
            // From inside a sandbox, it is the only thing there is.
            if (m_sandbox && process.ptr() != this)
                continue;
            vulture_process_info info;
            process->describe(info);
            size_t offset = table.size();
            TRY(table.try_resize(offset + sizeof(info)));
            __builtin_memcpy(table.begin() + offset, &info, sizeof(info));
        }
    } else if (name == "debug.fault"_sv && !debug_fault().is_empty()) {
        // A deliberate mistake with this process's memory: see Security/DebugFault.h.
        dmesgln("Deliberately touching user memory at {:#x}: {}", new_buffer, debug_fault());
        if (debug_fault() == "user-read"_sv)
            dmesgln("Read: {}", static_cast<u32>(*reinterpret_cast<u8 volatile*>(new_buffer)));
        else if (debug_fault() == "user-write"_sv)
            *reinterpret_cast<u8 volatile*>(new_buffer) = 0;
        else if (debug_fault() == "user-execute"_sv)
            reinterpret_cast<void (*)()>(new_buffer)();
        set_number(0u);
    } else if (name == "security.process.third-party"_sv) {
        // Is that process, or what started it, something installed? For privacyd, which only takes
        // answers from what came with the system. The process in question is given as the "new"
        // value, as there's no other way to say which.
        if (!m_credentials.is_superuser() || new_length != sizeof(i32))
            return Error::from_errno(EPERM);
        auto const pid = TRY(copy_from_user<i32>(new_buffer));
        bool found = false;
        int third_party = 1;
        for (auto& process : all()) {
            if (process->pid() == pid) {
                found = true;
                third_party = process->responsible_application().is_empty() ? 0 : 1;
                break;
            }
        }
        if (!found)
            return Error::from_errno(ESRCH);
        set_number(third_party);
    } else if (name == "security.seal.active"_sv) {
        set_number(Seal::is_active() ? 1 : 0);
    } else if (name == "security.code.enforced"_sv) {
        set_number(CodeSigning::is_enforced() ? 1 : 0);
    } else if (name == "security.code.trust"_sv || name == "security.code.distrust"_sv) {
        // What root, having checked it, vouches for: see Security/CodeSigning.h.
        if (!m_credentials.is_superuser())
            return Error::from_errno(EPERM);
        if (new_length != CodeSigning::hash_size)
            return Error::from_errno(EINVAL);
        u8 hash[CodeSigning::hash_size];
        TRY(copy_from_user(hash, new_buffer, sizeof(hash)));
        if (name == "security.code.trust"_sv)
            TRY(CodeSigning::trust(hash));
        else
            CodeSigning::distrust(hash);
        set_number(0);
    } else if (name == "hw.optional.hwcap"_sv) {
        // What the processor can do, for programs to choose code by: bits as in AT_HWCAP.
        set_number(Arch::hardware_capabilities());
    } else if (name == "net.interfaces"_sv) {
        auto& stack = Net::NetworkStack::the();
        MutexLocker locker(stack.lock());
        for (auto& interface : stack.interfaces()) {
            vulture_interface_info info {};
            auto interface_name = interface->name();
            __builtin_memcpy(info.name, interface_name.characters_without_null_termination(), min<size_t>(interface_name.length(), sizeof(info.name) - 1));
            __builtin_memcpy(info.mac, interface->mac().bytes, 6);
            info.mtu = interface->mtu();
            info.flags = VULTURE_IFF_UP | VULTURE_IFF_RUNNING | (interface->is_loopback() ? VULTURE_IFF_LOOPBACK : VULTURE_IFF_BROADCAST);
            info.address = __builtin_bswap32(interface->address.value);
            info.netmask = __builtin_bswap32(interface->netmask.value);
            info.gateway = __builtin_bswap32(interface->gateway.value);
            info.received_packets = interface->received_packets;
            info.received_bytes = interface->received_bytes;
            info.sent_packets = interface->sent_packets;
            info.sent_bytes = interface->sent_bytes;
            info.dropped_packets = interface->dropped_packets;
            size_t offset = table.size();
            TRY(table.try_resize(offset + sizeof(info)));
            __builtin_memcpy(table.begin() + offset, &info, sizeof(info));
        }
    } else if (name == "net.connections"_sv) {
        auto& stack = Net::NetworkStack::the();
        MutexLocker locker(stack.lock());
        Net::TCPSocket::for_each([](Net::TCPSocket& socket, void* rows) { add_connection_row(socket, *static_cast<Vector<u8>*>(rows)); }, &table);
        Net::UDPSocket::for_each([](Net::UDPSocket& socket, void* rows) { add_connection_row(socket, *static_cast<Vector<u8>*>(rows)); }, &table);
    } else if (name == "net.inet.debug.drop_one_in"_sv) {
        // Lose one packet of every N on loopback, to test what is done about loss. For root.
        if (new_buffer) {
            if (!m_credentials.is_superuser())
                return Error::from_errno(EPERM);
            if (new_length != sizeof(u32))
                return Error::from_errno(EINVAL);
            auto rate = TRY(copy_from_user<u32>(new_buffer));
            MutexLocker locker(Net::NetworkStack::the().lock());
            Net::NetworkStack::the().set_drop_rate(rate);
        }
        set_number(0u);
    } else if (name == "vfs.mounts"_sv) {
        TRY(VirtualFileSystem::for_each_mount_point([](void* context, FileSystem& file_system, StringView path) -> ErrorOr<void> {
            auto& rows = *static_cast<Vector<u8>*>(context);
            vulture_mount_info info {};
            auto copy = [](char* destination, size_t size, StringView text) {
                size_t length = min(text.length(), size - 1);
                __builtin_memcpy(destination, text.characters_without_null_termination(), length);
            };
            copy(info.type, sizeof(info.type), file_system.name());
            copy(info.point, sizeof(info.point), path);
            copy(info.device, sizeof(info.device), file_system.device_name());
            auto statistics = file_system.statistics();
            info.block_size = statistics.block_size;
            info.blocks = statistics.blocks;
            info.free_blocks = statistics.free_blocks;
            info.read_only = file_system.is_read_only();
            size_t offset = rows.size();
            TRY(rows.try_resize(offset + sizeof(info)));
            __builtin_memcpy(rows.begin() + offset, &info, sizeof(info));
            return {};
        }, &table));
    } else {
        return Error::from_errno(ENOENT);
    }
    result = name == "kern.proc.all"_sv || name == "vfs.mounts"_sv || name == "net.interfaces"_sv || name == "net.connections"_sv ? ReadonlyBytes { table.begin(), table.size() } : ReadonlyBytes { value, value_length };
    if (old_length_address) {
        auto room = TRY(copy_from_user<u64>(old_length_address));
        if (old_buffer) {
            if (room < result.size())
                return Error::from_errno(ENOMEM);
            TRY(copy_to_user(old_buffer, result.data(), result.size()));
        }
        TRY(copy_to_user(old_length_address, static_cast<u64>(result.size())));
    }
    return 0;
}

ErrorOr<FlatPtr> Process::sys_reboot(TrapFrame&, SyscallArguments const& arguments)
{
    if (!m_credentials.is_superuser() || m_sandbox)
        return Error::from_errno(EPERM);
    int how = static_cast<int>(arguments.values[0]);
    if (auto result = VirtualFileSystem::sync_all(); result.is_error())
        dmesgln("Reboot: could not write everything to disk: {}", result.error());
    if (how == VULTURE_REBOOT_POWER_OFF) {
        dmesgln("Powering off at the request of process {} ({}).", m_pid, name());
        Arch::power_off();
    }
    dmesgln("Restarting at the request of process {} ({}).", m_pid, name());
    Arch::reboot();
}

}
