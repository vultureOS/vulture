/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/files.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/API/POSIX/ptrace.h>
#include <Kernel/API/POSIX/resource.h>
#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/FileSystem/FileDescription.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Scheduler.h>
#include <LibBase/Format.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibELF/ELF.h>

namespace Kernel {

SpinLock& process_tree_lock();

// A tracer's what waits for a process, so a process that's attached to becomes its child for as long as that lasts.
ErrorOr<FlatPtr> Process::sys_ptrace(TrapFrame&, SyscallArguments const& arguments)
{
    int const request = static_cast<i32>(arguments.values[0]);
    auto const pid = static_cast<ProcessID>(static_cast<i32>(arguments.values[1]));
    FlatPtr const address = arguments.values[2];
    int const data = static_cast<i32>(arguments.values[3]);

    // Nothing in a sandbox gets to look inside anything else.
    if (m_sandbox && request != PT_DENY_ATTACH)
        return Error::from_errno(EPERM);

    if (request == PT_DENY_ATTACH) {
        if (m_tracer_pid.load())
            return Error::from_errno(EBUSY);
        m_deny_attach.store(true);
        return 0;
    }

    if (request == PT_TRACE_ME) {
        SpinLockGuard guard(process_tree_lock());
        if (m_tracer_pid.load() || !m_parent)
            return Error::from_errno(EBUSY);
        m_parent_before_tracing = m_parent->pid();
        m_tracer_pid.store(m_parent->pid());
        return 0;
    }

    auto target = Process::from_pid(pid);
    if (!target || target->is_zombie())
        return Error::from_errno(ESRCH);

    if (request == PT_ATTACH) {
        if (target.ptr() == this || pid == 1)
            return Error::from_errno(EINVAL);
        if (target->m_deny_attach.load())
            return Error::from_errno(EPERM);
        auto const& theirs = target->credentials();
        // Its own, unless it's root; and nothing that has more rights than whoever started it.
        if (!m_credentials.is_superuser() && (m_credentials.euid != theirs.euid || theirs.uid != theirs.euid || theirs.gid != theirs.egid))
            return Error::from_errno(EPERM);
        {
            SpinLockGuard guard(process_tree_lock());
            if (target->m_tracer_pid.load())
                return Error::from_errno(EBUSY);
            // Are you tracing what's tracing you? That can only end one way.
            if (m_tracer_pid.load() == pid)
                return Error::from_errno(EBUSY);
            target->m_parent_before_tracing = target->m_parent ? target->m_parent->pid() : 0;
            target->m_tracer_pid.store(m_pid);
            if (target->m_parent != this) {
                if (target->m_parent)
                    target->m_parent->m_children.remove(*target);
                target->m_parent = this;
                m_children.append(*target);
            }
        }
        vulture_siginfo info {};
        info.si_code = SI_USER;
        info.si_pid = m_pid;
        target->send_signal(SIGSTOP, info);
        return 0;
    }

    // The rest are the tracer's, of something that's stopped for it.
    if (target->m_tracer_pid.load() != m_pid)
        return Error::from_errno(EPERM);
    if (request == PT_KILL) {
        vulture_siginfo info {};
        info.si_code = SI_USER;
        info.si_pid = m_pid;
        target->send_signal(SIGKILL, info);
        target->m_continue_wait.wake_all();
        return 0;
    }
    if (!target->m_trace_stopped.load())
        return Error::from_errno(EBUSY);

    auto const check_signal = [&]() -> ErrorOr<void> {
        if (data < 0 || data >= NSIG)
            return Error::from_errno(EINVAL);
        return {};
    };

    switch (request) {
    case PT_CONTINUE:
    case PT_STEP: {
        TRY(check_signal());
        // Anywhere but 1 means to go on from there.
        if (address != 1) {
            vulture_thread_state state {};
            Arch::read_thread_state(*target->m_trace_frame, state);
#if defined(__aarch64__)
            state.pc = address;
#else
            state.rip = address;
#endif
            Arch::write_thread_state(*target->m_trace_frame, state);
        }
        target->m_trace_step.store(request == PT_STEP);
        target->resume_from_tracer(data);
        return 0;
    }
    case PT_DETACH:
        TRY(check_signal());
        target->detach_from_tracer(data);
        return 0;
    case VULTURE_PT_GETREGS: {
        vulture_thread_state state {};
        Arch::read_thread_state(*target->m_trace_frame, state);
        TRY(copy_to_user(address, state));
        return 0;
    }
    case VULTURE_PT_SETREGS: {
        auto const state = TRY(copy_from_user<vulture_thread_state>(address));
#if defined(__aarch64__)
        if (!is_user_range(state.pc, 4) || !is_user_range(state.sp, 1))
            return Error::from_errno(EINVAL);
#else
        if (!is_user_range(state.rip, 1) || !is_user_range(state.rsp, 1))
            return Error::from_errno(EINVAL);
#endif
        Arch::write_thread_state(*target->m_trace_frame, state);
        return 0;
    }
    case VULTURE_PT_IO: {
        auto request_io = TRY(copy_from_user<vulture_ptrace_io>(address));
        constexpr size_t most_at_once = 16 * MiB;
        if (request_io.length > most_at_once)
            return Error::from_errno(EINVAL);
        Vector<u8> bytes;
        TRY(bytes.try_resize(request_io.length));
        auto const in_target = reinterpret_cast<FlatPtr>(request_io.target_address);
        auto const here = reinterpret_cast<FlatPtr>(request_io.local_address);
        auto space = target->address_space_reference();
        if (!space)
            return Error::from_errno(ESRCH);
        if (request_io.operation == VULTURE_PTRACE_READ) {
            TRY(space->debugger_read(in_target, { bytes.data(), bytes.size() }));
            if (bytes.size())
                TRY(copy_to_user(here, bytes.data(), bytes.size()));
        } else if (request_io.operation == VULTURE_PTRACE_WRITE) {
            if (bytes.size())
                TRY(copy_from_user(bytes.data(), here, bytes.size()));
            TRY(space->debugger_write(in_target, { bytes.data(), bytes.size() }));
        } else {
            return Error::from_errno(EINVAL);
        }
        return 0;
    }
    default:
        return Error::from_errno(EINVAL);
    }
}

int Process::stop_for_tracer(TrapFrame& frame, int signal)
{
    m_trace_frame = &frame;
    m_trace_resume_signal.store(0);
    m_trace_step.store(false);
    m_unreported_continue.store(false);
    m_unreported_stop.store(signal);
    m_stopped.store(true);
    m_trace_stopped.store(true);
    notify_parent(CLD_STOPPED, signal);
    {
        SpinLockGuard guard(m_threads_lock);
        for (auto& thread : m_threads)
            Scheduler::kick(thread);
    }
    wait_while_stopped(Thread::current());
    m_trace_stopped.store(false);
    m_stopped.store(false);
    m_trace_frame = nullptr;
    m_unreported_stop.store(0);
    if (m_trace_step.exchange(false)) {
        Thread::current().set_single_stepping(true);
        Arch::set_single_step(frame, true);
        InterruptDisabler disabler;
        Arch::note_single_stepping(true);
    }
    return m_trace_resume_signal.exchange(0);
}

void Process::resume_from_tracer(int signal)
{
    m_trace_resume_signal.store(signal);
    m_stopped.store(false);
    m_continue_wait.wake_all();
}

// ---- Core dumps.

namespace {

size_t align_note(size_t size)
{
    return (size + 3) & ~size_t(3);
}

}

bool Process::write_core_dump(TrapFrame const& frame, int signal)
{
    u64 const limit = m_limits[VULTURE_RLIMIT_CORE].rlim_cur;
    if (limit == 0)
        return false;
    // A program with someone else's rights has things in it that the person running it shouldn't see.
    if (m_credentials.euid != m_credentials.uid || m_credentials.egid != m_credentials.gid)
        return false;

    auto regions = m_address_space->describe_for_core();
    if (regions.is_error())
        return false;

    FixedStringBuilder<64> path;
    format_to(path, "/cores/core.{}", m_pid);
    auto opened = VirtualFileSystem::open(path.view(), "/"_sv, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0400);
    if (opened.is_error()) {
        dmesgln("Process {} ({}): no core dump: {}", m_pid, name(), opened.error());
        return false;
    }
    auto file = opened.release_value();

    using namespace ELF;
    constexpr size_t name_size = 8; // "VULTURE" and its nul.
    size_t const note_size = sizeof(Elf64_Nhdr) + align_note(name_size) + align_note(sizeof(vulture_core_thread));
    size_t const headers_size = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * (regions.value().size() + 1);

    Vector<u8> head;
    if (head.try_resize(headers_size + note_size).is_error())
        return false;
    __builtin_memset(head.data(), 0, head.size());

    auto* header = reinterpret_cast<Elf64_Ehdr*>(head.data());
    header->e_ident[0] = 0x7f;
    header->e_ident[1] = 'E';
    header->e_ident[2] = 'L';
    header->e_ident[3] = 'F';
    header->e_ident[EI_CLASS] = ELFCLASS64;
    header->e_ident[5] = 1; // Little-endian.
    header->e_ident[6] = 1; // The current version.
    header->e_type = ET_CORE;
    header->e_machine = EM_NATIVE;
    header->e_version = 1;
    header->e_phoff = sizeof(Elf64_Ehdr);
    header->e_ehsize = sizeof(Elf64_Ehdr);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = static_cast<u16>(regions.value().size() + 1);

    auto* segments = reinterpret_cast<Elf64_Phdr*>(head.data() + sizeof(Elf64_Ehdr));
    segments[0].p_type = PT_NOTE;
    segments[0].p_offset = headers_size;
    segments[0].p_filesz = note_size;
    segments[0].p_align = 4;

    u64 offset = align_up<u64>(headers_size + note_size, page_size);
    u64 const first_contents = offset;
    for (size_t i = 0; i < regions.value().size(); ++i) {
        auto const& region = regions.value()[i];
        auto& segment = segments[i + 1];
        segment.p_type = PT_LOAD;
        segment.p_flags = ((region.protection & PROT_READ) ? PF_R : 0) | ((region.protection & PROT_WRITE) ? PF_W : 0) | ((region.protection & PROT_EXEC) ? PF_X : 0);
        segment.p_vaddr = region.base;
        segment.p_memsz = region.size;
        segment.p_align = page_size;
        segment.p_offset = offset;
        // What's the same as a file's only said to be there. And up to the limit, as much as fits.
        if (region.has_contents && offset + region.size <= limit) {
            segment.p_filesz = region.size;
            offset += region.size;
        }
    }

    auto* note = reinterpret_cast<Elf64_Nhdr*>(head.data() + headers_size);
    note->n_namesz = name_size;
    note->n_descsz = sizeof(vulture_core_thread);
    note->n_type = VULTURE_NOTE_THREAD;
    __builtin_memcpy(reinterpret_cast<u8*>(note + 1), "VULTURE", name_size);
    // (Notes are only aligned to four bytes, which is less than this wants: so it's made here, and copied in.)
    vulture_core_thread thread {};
    thread.signal = signal;
    thread.pid = m_pid;
    __builtin_memcpy(thread.name, m_name, min(m_name_length, sizeof(thread.name) - 1));
    Arch::read_thread_state(frame, thread.state);
    __builtin_memcpy(reinterpret_cast<u8*>(note + 1) + align_note(name_size), &thread, sizeof(thread));

    auto const put = [&](u8 const* bytes, size_t size) -> bool {
        auto written = file->write(IOBuffer::kernel(const_cast<u8*>(bytes)), size);
        return !written.is_error() && written.value() == size;
    };
    if (!put(head.data(), head.size()))
        return false;
    static u8 const zeroes[page_size] {};
    if (first_contents > head.size() && !put(zeroes, first_contents - head.size()))
        return false;

    auto* page = static_cast<u8*>(kmalloc(page_size));
    if (!page)
        return false;
    bool ok = true;
    for (size_t i = 0; ok && i < regions.value().size(); ++i) {
        auto const& segment = segments[i + 1];
        for (u64 at = 0; ok && at < segment.p_filesz; at += page_size) {
            // What can't be read reads as zeroes: the rest of the dump is still worth having.
            if (m_address_space->debugger_read(segment.p_vaddr + at, { page, page_size }).is_error())
                __builtin_memset(page, 0, page_size);
            ok = put(page, page_size);
        }
    }
    kfree(page);
    if (ok)
        dmesgln("Process {} ({}): core dumped to {}", m_pid, name(), path.view());
    return ok;
}

}
