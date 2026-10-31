/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/API/POSIX/wait.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/MemoryLayout.h>
#include <Kernel/Arch/UserContext.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Memory/UserAccess.h>
#include <Kernel/TTY/TTY.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <LibBase/New.h>
#include <LibELF/Demangle.h>
#include <LibELF/ELF.h>
#include <LibELF/Image.h>

namespace Kernel {

// Guards every process's parent and children links and the process table.
static SpinLock s_tree_lock;
static Process::TableList s_processes;
static Atomic<ProcessID> s_next_pid { 1 };
static Process* s_init;

SpinLock& process_tree_lock()
{
    return s_tree_lock;
}

void Process::register_with_parent()
{
    SpinLockGuard guard(s_tree_lock);
    s_processes.append(*this);
    ref();
    if (m_parent) {
        m_parent->m_children.append(*this);
        ref();
    }
}

void Process::unregister_from_table()
{
    VERIFY(s_tree_lock.is_locked_by_current_processor());
    s_processes.remove(*this);
    unref();
}

static constexpr u64 bit(int signal)
{
    return 1ull << signal;
}

static constexpr u64 unblockable_signals = bit(SIGKILL) | bit(SIGSTOP);

Process::Process(ProcessID pid, Process* parent)
    : m_pid(pid)
    , m_parent(parent)
    , m_cwd(MUST(String::from_view("/"_sv)))
{
}

Process::~Process()
{
    VERIFY(!m_thread);
    VERIFY(m_children.is_empty());
}

ErrorOr<NonnullRefPtr<Process>> Process::create(Process* parent)
{
    auto* process = new (Base::nothrow) Process(s_next_pid.fetch_add(1), parent);
    if (!process)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*process);
}

Process* Process::current()
{
    if (!Scheduler::is_running())
        return nullptr;
    return Thread::current().process();
}

RefPtr<Process> Process::from_pid(ProcessID pid)
{
    SpinLockGuard guard(s_tree_lock);
    for (auto& process : s_processes) {
        if (process.pid() == pid)
            return process;
    }
    return nullptr;
}

Vector<NonnullRefPtr<Process>> Process::all()
{
    Vector<NonnullRefPtr<Process>> processes;
    SpinLockGuard guard(s_tree_lock);
    for (auto& process : s_processes) {
        if (processes.try_append(process).is_error())
            break;
    }
    return processes;
}

size_t Process::count()
{
    SpinLockGuard guard(s_tree_lock);
    return s_processes.size();
}

ProcessID Process::parent_pid() const
{
    SpinLockGuard guard(s_tree_lock);
    return m_parent ? m_parent->pid() : 0;
}

void Process::set_name(StringView name)
{
    m_name_length = min(name.length(), sizeof(m_name));
    __builtin_memcpy(m_name, name.characters_without_null_termination(), m_name_length);
}

void Process::thread_finished()
{
    // The thread no longer runs on this address space anywhere.
    m_thread = nullptr;
    m_address_space.clear();
}

ErrorOr<int> Process::allocate_fd(NonnullRefPtr<FileDescription> description, bool close_on_exec, int lowest)
{
    SpinLockGuard guard(m_files_lock);
    for (size_t fd = static_cast<size_t>(lowest); fd < max_open_files; ++fd) {
        if (!m_files[fd].description) {
            m_files[fd] = { move(description), close_on_exec };
            return static_cast<int>(fd);
        }
    }
    return Error::from_errno(EMFILE);
}

ErrorOr<NonnullRefPtr<FileDescription>> Process::file(int fd)
{
    if (fd < 0 || static_cast<size_t>(fd) >= max_open_files)
        return Error::from_errno(EBADF);
    SpinLockGuard guard(m_files_lock);
    if (!m_files[fd].description)
        return Error::from_errno(EBADF);
    return NonnullRefPtr<FileDescription>(*m_files[fd].description);
}

ErrorOr<String> Process::copy_path_from_user(FlatPtr address)
{
    char path[VirtualFileSystem::max_path];
    auto length = TRY(copy_string_from_user(address, path, sizeof(path)));
    if (length == 0)
        return Error::from_errno(ENOENT);
    return String::from_view({ path, length });
}

// --- Starting programs ---

namespace {

// The arguments of init, owned by its thread until exec copies them.
struct InitArguments {
    char path[64];
    char storage[512];
    StringView arguments[8];
    size_t argument_count;
};

}

ErrorOr<NonnullRefPtr<Process>> Process::create_init(StringView path, Span<StringView const> arguments)
{
    VERIFY(!s_init);
    auto process = TRY(create(nullptr));
    process->m_pgid.store(process->pid());
    process->m_sid.store(process->pid());
    VERIFY(process->pid() == 1);
    process->set_name("init"_sv);

    auto* init_arguments = new (Base::nothrow) InitArguments {};
    if (!init_arguments || path.length() >= sizeof(init_arguments->path) || arguments.size() > array_size(init_arguments->arguments))
        return Error::from_errno(E2BIG);
    __builtin_memcpy(init_arguments->path, path.characters_without_null_termination(), path.length());
    size_t used = 0;
    for (auto argument : arguments) {
        if (used + argument.length() > sizeof(init_arguments->storage))
            return Error::from_errno(E2BIG);
        __builtin_memcpy(init_arguments->storage + used, argument.characters_without_null_termination(), argument.length());
        init_arguments->arguments[init_arguments->argument_count++] = { init_arguments->storage + used, argument.length() };
        used += argument.length();
    }

    auto console = TRY(VirtualFileSystem::open("/dev/console"_sv, "/"_sv, O_RDWR, 0));
    for (size_t fd = 0; fd < 3; ++fd)
        process->m_files[fd] = { console, false };

    auto thread = TRY(Thread::create("init"_sv, [](void* argument) {
        auto* init = static_cast<InitArguments*>(argument);
        auto& self = Thread::current();
        auto& frame = self.user_trap_frame();
        StringView const environment[] = { "PATH=/bin:/usr/bin:/sbin"_sv, "HOME=/"_sv };
        auto result = self.process()->exec(StringView(init->path), { init->arguments, init->argument_count }, environment, frame);
        if (result.is_error()) {
            FixedStringBuilder<128> reason;
            format_to(reason, "cannot start {}: {}", StringView(init->path), result.error());
            panic(reason.view());
        }
        delete init;
        Arch::return_to_user(frame); }, init_arguments));
    thread->set_process(*process);
    process->m_thread = thread.ptr();
    {
        SpinLockGuard guard(s_tree_lock);
        s_processes.append(*process);
        process->ref();
        s_init = process.ptr();
    }
    thread->start();
    return process;
}

// Writes into an object's pages directly (before the program runs).
static ErrorOr<void> write_to_region(Region& region, FlatPtr address, ReadonlyBytes data)
{
    size_t done = 0;
    while (done < data.size()) {
        FlatPtr page_address = align_down<FlatPtr>(address + done, page_size);
        size_t index = region.object_index(page_address);
        auto page = region.object().page(index);
        if (!page.get()) {
            page = TRY(physical_memory().allocate_user_page());
            region.object().set_page(index, page);
        }
        size_t offset = (address + done) - page_address;
        size_t count = min(page_size - offset, data.size() - done);
        __builtin_memcpy(physical_to_virtual(page) + offset, data.data() + done, count);
        done += count;
    }
    return {};
}

static int protection_from_segment_flags(u32 flags)
{
    int protection = 0;
    if (flags & ELF::PF_R)
        protection |= PROT_READ;
    if (flags & ELF::PF_W)
        protection |= PROT_WRITE;
    if (flags & ELF::PF_X)
        protection |= PROT_EXEC;
    return protection;
}

// The initial stack, laid out the way crt0 expects: argc, argv[], NULL,
// envp[], NULL, auxv pairs, then the strings themselves at the top.
static ErrorOr<FlatPtr> build_initial_stack(Region& stack, Span<StringView const> arguments, Span<StringView const> environment, FlatPtr entry)
{
    constexpr size_t limit = 128 * KiB;
    size_t strings_size = 0;
    for (auto string : arguments)
        strings_size += string.length() + 1;
    for (auto string : environment)
        strings_size += string.length() + 1;
    if (strings_size > limit || arguments.size() + environment.size() > 4096)
        return Error::from_errno(E2BIG);

    constexpr u64 at_null = 0;
    constexpr u64 at_pagesz = 6;
    constexpr u64 at_entry = 9;
    size_t pointer_words = 1 + arguments.size() + 1 + environment.size() + 1 + 6;
    FlatPtr top = MemoryLayout::user_stack_top;
    FlatPtr strings_start = top - strings_size;
    FlatPtr sp = align_down<FlatPtr>(strings_start - pointer_words * sizeof(u64), 16);

    auto* words = static_cast<u64*>(kzalloc(pointer_words * sizeof(u64)));
    if (!words)
        return Error::from_errno(ENOMEM);
    size_t word = 0;
    words[word++] = arguments.size();
    FlatPtr string_address = strings_start;
    for (auto string : arguments) {
        words[word++] = string_address;
        TRY(write_to_region(stack, string_address, { reinterpret_cast<u8 const*>(string.characters_without_null_termination()), string.length() }));
        string_address += string.length() + 1;
    }
    words[word++] = 0;
    for (auto string : environment) {
        words[word++] = string_address;
        TRY(write_to_region(stack, string_address, { reinterpret_cast<u8 const*>(string.characters_without_null_termination()), string.length() }));
        string_address += string.length() + 1;
    }
    words[word++] = 0;
    words[word++] = at_pagesz;
    words[word++] = page_size;
    words[word++] = at_entry;
    words[word++] = entry;
    words[word++] = at_null;
    words[word++] = 0;
    auto result = write_to_region(stack, sp, { reinterpret_cast<u8 const*>(words), pointer_words * sizeof(u64) });
    kfree(words);
    TRY(result);
    // The NULs after each string: pages start zeroed, and only the
    // characters were written.
    return sp;
}

// The whole program file, read into memory: the ELF parser and the crash
// reporter want it in one piece.
static ErrorOr<NonnullRefPtr<KernelBuffer>> read_executable(Inode& inode)
{
    auto metadata = inode.metadata();
    if (metadata.type != InodeType::File)
        return Error::from_errno(EACCES);
    TRY(check_access(metadata, 1));
    constexpr u64 largest = 4 * MiB;
    if (metadata.size > largest)
        return Error::from_errno(E2BIG);
    auto buffer = TRY(KernelBuffer::create(metadata.size));
    size_t read = TRY(inode.read(0, metadata.size, IOBuffer::kernel(buffer->bytes().data())));
    if (read != metadata.size)
        return Error::from_errno(EIO);
    return buffer;
}

ErrorOr<void> Process::exec(StringView path, Span<StringView const> arguments, Span<StringView const> environment, TrapFrame& frame)
{
    auto resolved = TRY(VirtualFileSystem::resolve(path, m_cwd.view()));
    auto executable = TRY(read_executable(*resolved.inode));
    auto image_or_error = ELF::Image::parse(executable->bytes());
    if (image_or_error.is_error())
        return Error::from_errno(ENOEXEC);
    auto const& image = image_or_error.value();
    auto const& header = image.header();
    if (header.e_type != ELF::ET_EXEC || header.e_machine != ELF::EM_AARCH64 || !is_user_range(header.e_entry, 4))
        return Error::from_errno(ENOEXEC);

    auto space = TRY(AddressSpace::create());
    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_LOAD || segment.p_memsz == 0)
            continue;
        if (segment.p_filesz > segment.p_memsz || !is_user_range(segment.p_vaddr, segment.p_memsz))
            return Error::from_errno(ENOEXEC);
        int protection = protection_from_segment_flags(segment.p_flags);
        if ((protection & PROT_WRITE) && (protection & PROT_EXEC))
            return Error::from_errno(ENOEXEC);
        FlatPtr start = align_down<FlatPtr>(segment.p_vaddr, page_size);
        FlatPtr end = align_up<FlatPtr>(segment.p_vaddr + segment.p_memsz, page_size);
        auto* region = TRY(space->map_anonymous(start, end - start, protection, "program"_sv, true));
        // File contents are copied now; the rest (.bss) is zero on demand.
        TRY(write_to_region(*region, segment.p_vaddr, image.segment_data(segment)));
        if (protection & PROT_EXEC) {
            for (FlatPtr page = start; page < end; page += page_size) {
                auto physical = region->object().page(region->object_index(page));
                if (physical.get())
                    Arch::make_code_coherent(physical);
            }
        }
    }

    FlatPtr stack_bottom = MemoryLayout::user_stack_top - MemoryLayout::user_stack_size;
    auto* stack = TRY(space->map_anonymous(stack_bottom, MemoryLayout::user_stack_size, PROT_READ | PROT_WRITE, "stack"_sv, true));
    FlatPtr stack_pointer = TRY(build_initial_stack(*stack, arguments, environment, header.e_entry));

    // The point of no return: the new program replaces the old one.
    OwnPtr<AddressSpace> old_space;
    {
        // No switch may see the process between the two address spaces.
        InterruptDisabler disabler;
        old_space = move(m_address_space);
        m_address_space = OwnPtr<AddressSpace>(move(space));
        m_address_space->activate();
    }
    // Nothing uses the old one now (this process has one thread).
    old_space.clear();

    m_executable = executable;
    // Set-user-ID and set-group-ID programs run as the file's owner and group.
    auto program = resolved.inode->metadata();
    if (program.permissions & 04000) {
        m_credentials.euid = program.uid;
        m_credentials.saved_uid = program.uid;
    }
    if (program.permissions & 02000) {
        m_credentials.egid = program.gid;
        m_credentials.saved_gid = program.gid;
    }
    auto slash = path.find_last('/');
    set_name(slash.has_value() ? path.substring_view(slash.value() + 1) : path);
    {
        SpinLockGuard guard(m_signal_lock);
        for (auto& action : m_signal_actions) {
            if (action.handler != VULTURE_SIG_IGN)
                action = {};
        }
    }
    FileDescriptor closing[max_open_files];
    {
        SpinLockGuard guard(m_files_lock);
        for (size_t fd = 0; fd < max_open_files; ++fd) {
            if (m_files[fd].description && m_files[fd].close_on_exec)
                closing[fd] = move(m_files[fd]);
        }
    }
    for (auto& file : closing)
        file = {};
    Arch::prepare_user_entry(frame, header.e_entry, stack_pointer);
    return {};
}

// --- Ending ---

void Process::child_state_changed()
{
    m_child_events.fetch_add(1);
    m_child_wait.wake_all();
}

void Process::terminate(int status)
{
    VERIFY(this == current());
    if (m_exiting.exchange(true)) {
        Thread::exit();
    }
    if (this == s_init) {
        FixedStringBuilder<64> reason;
        format_to(reason, "init exited (status {:#x})", status);
        panic(reason.view());
    }
    // Closed outside the lock: closing a pipe end wakes the other side.
    FileDescriptor closing[max_open_files];
    {
        SpinLockGuard guard(m_files_lock);
        for (size_t fd = 0; fd < max_open_files; ++fd)
            closing[fd] = move(m_files[fd]);
    }
    for (auto& file : closing)
        file = {};
    m_address_space->remove_all_regions();
    lose_controlling_terminal();

    Process* parent;
    {
        SpinLockGuard guard(s_tree_lock);
        m_exit_status = status;
        // Orphans go to init, which reaps them.
        bool handed_over_zombie = false;
        while (auto* child = m_children.take_first()) {
            child->m_parent = s_init;
            s_init->m_children.append(*child);
            handed_over_zombie = handed_over_zombie || child->is_zombie();
        }
        if (handed_over_zombie)
            s_init->m_child_events.fetch_add(1);
        m_zombie.store(true, MemoryOrder::Release);
        parent = m_parent;
        if (parent)
            parent->ref();
    }
    if (parent) {
        vulture_siginfo info {};
        info.si_signo = SIGCHLD;
        info.si_code = WIFSIGNALED(status) ? CLD_KILLED : CLD_EXITED;
        info.si_pid = m_pid;
        info.si_status = status;
        parent->send_signal(SIGCHLD, info);
        parent->child_state_changed();
        if (parent != s_init)
            s_init->child_state_changed();
        parent->unref();
    } else {
        s_init->child_state_changed();
    }
    Thread::exit();
}

// --- Signals ---

bool Process::has_deliverable_signal() const
{
    u64 pending = m_pending_signals.load(MemoryOrder::Acquire);
    return (pending & ~m_signal_mask) || (pending & unblockable_signals);
}

static bool is_ignored(SignalAction const& action, int signal);

static constexpr u64 stop_signals = bit(SIGSTOP) | bit(SIGTSTP) | bit(SIGTTIN) | bit(SIGTTOU);

void Process::send_signal(int signal, vulture_siginfo const& info)
{
    VERIFY(signal > 0 && signal < NSIG);
    // SIGCONT continues a stopped process even if it is ignored or caught.
    if (signal == SIGCONT)
        continue_running();
    {
        SpinLockGuard guard(m_signal_lock);
        // A stop cancels a pending continue, and a continue pending stops.
        if (bit(signal) & stop_signals)
            m_pending_signals.fetch_and(~bit(SIGCONT));
        if (signal == SIGCONT)
            m_pending_signals.fetch_and(~stop_signals);
        // An ignored signal is discarded right away: it must not interrupt
        // sleeps or waits (POSIX).
        if (is_ignored(m_signal_actions[signal], signal))
            return;
        m_signal_info[signal] = info;
        m_signal_info[signal].si_signo = signal;
        m_pending_signals.fetch_or(bit(signal));
    }
    if (signal == SIGKILL)
        m_continue_wait.wake_all();
    if (m_thread) {
        m_thread->interrupt_sleep();
        // A waitpid(), or a wait on a pipe, returns to let the signal through.
        m_thread->interrupt_waits();
    }
}

ErrorOr<void> Process::send_signal_to_group(ProcessID pgid, int signal, vulture_siginfo const& info)
{
    Vector<NonnullRefPtr<Process>> members;
    {
        SpinLockGuard guard(s_tree_lock);
        for (auto& process : s_processes) {
            if (process.pgid() == pgid && !process.is_zombie())
                members.append(process);
        }
    }
    if (members.is_empty())
        return Error::from_errno(ESRCH);
    if (signal != 0) {
        for (auto& process : members)
            process->send_signal(signal, info);
    }
    return {};
}

bool Process::group_exists_in_session(ProcessID pgid, ProcessID sid)
{
    SpinLockGuard guard(s_tree_lock);
    for (auto& process : s_processes) {
        if (process.pgid() == pgid && process.sid() == sid && !process.is_zombie())
            return true;
    }
    return false;
}

// Tells the parent (SIGCHLD, and wakes its waitpid) that this process
// stopped or continued.
void Process::notify_parent(int code, int status)
{
    Process* parent;
    {
        SpinLockGuard guard(s_tree_lock);
        parent = m_parent;
        if (parent)
            parent->ref();
    }
    if (!parent)
        return;
    bool quiet = false;
    {
        SpinLockGuard guard(parent->m_signal_lock);
        quiet = code == CLD_STOPPED && (parent->m_signal_actions[SIGCHLD].flags & SA_NOCLDSTOP);
    }
    if (!quiet) {
        vulture_siginfo info {};
        info.si_signo = SIGCHLD;
        info.si_code = code;
        info.si_pid = m_pid;
        info.si_status = status;
        parent->send_signal(SIGCHLD, info);
    }
    parent->child_state_changed();
    parent->unref();
}

bool Process::is_signal_ignored_or_blocked(int signal) const
{
    SpinLockGuard guard(m_signal_lock);
    return (m_signal_mask & bit(signal)) || m_signal_actions[signal].handler == VULTURE_SIG_IGN;
}

ErrorOr<void> Process::set_controlling_terminal(TTY& tty)
{
    if (m_sid.load() != m_pid || m_controlling_terminal)
        return Error::from_errno(EPERM);
    if (tty.session() != 0 && tty.session() != m_pid)
        return Error::from_errno(EPERM);
    tty.set_session(m_pid, pgid());
    m_controlling_terminal = tty;
    return {};
}

void Process::lose_controlling_terminal()
{
    if (auto tty = move(m_controlling_terminal); tty && m_sid.load() == m_pid)
        tty->release_session(m_pid);
}

void Process::continue_running()
{
    if (!m_stopped.exchange(false))
        return;
    m_unreported_stop.store(0);
    m_unreported_continue.store(true);
    m_continue_wait.wake_all();
    notify_parent(CLD_CONTINUED, SIGCONT);
}

void Process::stop(int signal)
{
    m_unreported_continue.store(false);
    m_unreported_stop.store(signal);
    m_stopped.store(true);
    notify_parent(CLD_STOPPED, signal);
    // Until SIGCONT (continue_running) or SIGKILL.
    m_continue_wait.wait_until([&] {
        return !m_stopped.load() || (m_pending_signals.load() & bit(SIGKILL));
    });
    m_stopped.store(false);
}

void Process::force_signal(int signal, vulture_siginfo const& info)
{
    // A fault cannot be ignored or blocked: that would retry it forever.
    {
        SpinLockGuard guard(m_signal_lock);
        m_signal_mask &= ~bit(signal);
        if (m_signal_actions[signal].handler == VULTURE_SIG_IGN)
            m_signal_actions[signal] = {};
    }
    send_signal(signal, info);
}

enum class DefaultAction : u8 {
    Terminate,
    CrashReport, // Terminate, with a report.
    Ignore,
    Stop,
};

static DefaultAction default_action(int signal)
{
    switch (signal) {
    case SIGQUIT:
    case SIGILL:
    case SIGTRAP:
    case SIGABRT:
    case SIGBUS:
    case SIGFPE:
    case SIGSEGV:
        return DefaultAction::CrashReport;
    case SIGCHLD:
    case SIGCONT:
    case SIGURG:
    case SIGWINCH:
        return DefaultAction::Ignore;
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return DefaultAction::Stop;
    default:
        return DefaultAction::Terminate;
    }
}

static bool is_ignored(SignalAction const& action, int signal)
{
    if (signal == SIGKILL || signal == SIGSTOP)
        return false;
    return action.handler == VULTURE_SIG_IGN || (action.handler == VULTURE_SIG_DFL && default_action(signal) == DefaultAction::Ignore);
}

void Process::dispatch_signal(TrapFrame& frame)
{
    int signal = 0;
    SignalAction action;
    vulture_siginfo info;
    {
        SpinLockGuard guard(m_signal_lock);
        u64 pending = m_pending_signals.load();
        u64 deliverable = (pending & ~m_signal_mask) | (pending & unblockable_signals);
        if (!deliverable)
            return;
        signal = __builtin_ctzll(deliverable);
        m_pending_signals.fetch_and(~bit(signal));
        action = m_signal_actions[signal];
        info = m_signal_info[signal];
    }

    if (signal == SIGKILL || signal == SIGSTOP || action.handler == VULTURE_SIG_DFL) {
        auto what = default_action(signal);
        if (what == DefaultAction::Ignore)
            return;
        if (what == DefaultAction::Stop) {
            stop(signal);
            return;
        }
        if (what == DefaultAction::CrashReport)
            report_crash(frame, signal, info);
        terminate(VULTURE_W_SIGNALED(signal, what == DefaultAction::CrashReport));
    }
    if (action.handler == VULTURE_SIG_IGN)
        return;

    u64 saved_mask = m_signal_mask;
    if (Arch::push_signal_frame(frame, signal, info, saved_mask, action.handler, action.restorer).is_error()) {
        // No room for the frame (the stack is gone or bad): fatal.
        report_crash(frame, SIGSEGV, info);
        terminate(VULTURE_W_SIGNALED(SIGSEGV, true));
    }
    SpinLockGuard guard(m_signal_lock);
    m_signal_mask |= action.mask;
    if (!(action.flags & SA_NODEFER))
        m_signal_mask |= bit(signal);
    m_signal_mask &= ~unblockable_signals;
    if (action.flags & SA_RESETHAND)
        m_signal_actions[signal] = {};
}

void Process::handle_user_fault(TrapFrame& frame, FlatPtr address, AccessKind, bool permission_fault, PageFaultResult result)
{
    if (result == PageFaultResult::OutOfMemory) {
        dmesgln("Process {} ({}): out of memory handling a page fault at {:#x}; killing it", m_pid, name(), address);
        terminate(VULTURE_W_SIGNALED(SIGKILL, false));
    }
    vulture_siginfo info {};
    info.si_code = permission_fault ? SEGV_ACCERR : SEGV_MAPERR;
    info.si_addr = reinterpret_cast<void*>(address);
    (void)frame;
    force_signal(SIGSEGV, info);
}

void Process::handle_user_exception(TrapFrame& frame, int signal)
{
    vulture_siginfo info {};
    info.si_addr = reinterpret_cast<void*>(frame.program_counter());
    force_signal(signal, info);
}

static void write_user_symbol(FormatSink& sink, Optional<ELF::Image> const& image, FlatPtr address)
{
    Optional<ELF::Symbol> symbol;
    if (image.has_value())
        symbol = image->find_symbol(address);
    if (!symbol.has_value()) {
        sink.write("??"_sv);
        return;
    }
    if (!ELF::demangle(symbol->name, sink))
        sink.write(symbol->name);
    format_to(sink, "+{:#x}", address - symbol->address);
}

void Process::report_crash(TrapFrame const& frame, int signal, vulture_siginfo const& info)
{
    FixedStringBuilder<192> reason;
    if (signal == SIGSEGV || signal == SIGBUS || signal == SIGILL) {
        // The exception that led here, decoded like a kernel panic's.
        Arch::describe_exception(reason, frame, 0);
    } else {
        format_to(reason, "killed by signal {}", signal);
    }
    dmesgln("Process {} ({}) crashed: {}", m_pid, name(), reason.view());
    if (info.si_addr && (signal == SIGSEGV || signal == SIGBUS))
        dmesgln("  fault address {:p}", info.si_addr);

    TrapFrame user_view = frame;
    user_view.sp = frame.user_sp;
    FixedStringBuilder<2048> registers;
    Arch::dump_registers(registers, user_view);
    kernel_log().write(registers.view());

    Optional<ELF::Image> image;
    if (m_executable) {
        if (auto parsed = ELF::Image::parse(m_executable->bytes()); !parsed.is_error())
            image = parsed.release_value();
    }
    dmesgln("Backtrace:");
    FixedStringBuilder<256> line;
    format_to(line, "  #0  {:#018x}  ", frame.program_counter());
    write_user_symbol(line, image, frame.program_counter());
    dmesgln("{}", line.view());
    FlatPtr frame_pointer = frame.frame_pointer();
    for (size_t index = 1; index < 32 && frame_pointer; ++index) {
        auto record = copy_from_user<Arch::StackFrame>(frame_pointer);
        if (record.is_error() || record.value().return_address == 0)
            break;
        line.clear();
        format_to(line, "  #{:<2} {:#018x}  ", index, record.value().return_address);
        write_user_symbol(line, image, record.value().return_address - 4);
        dmesgln("{}", line.view());
        if (record.value().previous_frame_pointer <= frame_pointer)
            break;
        frame_pointer = record.value().previous_frame_pointer;
    }
}

}
