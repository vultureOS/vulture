/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Security/CodeSigning.h>
#include <Kernel/Security/Random.h>
#include <Kernel/API/POSIX/auxv.h>
#include <Kernel/API/POSIX/errno.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/API/POSIX/sysctl.h>
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
#include <Kernel/FileSystem/Poll.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Scheduler.h>
#include <Kernel/Time/Time.h>
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
    VERIFY(m_threads.is_empty());
    VERIFY(m_thread_objects.load() == 0);
    VERIFY(m_children.is_empty());
}

ErrorOr<NonnullRefPtr<Process>> Process::create(Process* parent)
{
    auto* process = new (Base::nothrow) Process(s_next_pid.fetch_add(1), parent);
    if (!process)
        return Error::from_errno(ENOMEM);
    process->m_start_time = Time::wall_clock_seconds();
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

void Process::describe(vulture_process_info& info)
{
    info = {};
    info.pid = m_pid;
    info.ppid = parent_pid();
    info.pgid = pgid();
    info.sid = sid();
    info.uid = m_credentials.uid;
    info.euid = m_credentials.euid;
    info.gid = m_credentials.gid;
    u64 ticks = m_exited_cpu_ticks.load();
    bool running = false;
    {
        SpinLockGuard guard(m_threads_lock);
        for (auto& thread : m_threads) {
            ticks += thread.cpu_ticks();
            running = running || thread.state() == Thread::State::Running || thread.state() == Thread::State::Runnable;
        }
        info.threads = m_threads.size();
    }
    if (is_zombie())
        info.state = 'Z';
    else if (is_stopped())
        info.state = 'T';
    else if (running)
        info.state = 'R';
    else
        info.state = 'S';
    // Our own references: whoever this is about may exec or exit at any moment.
    RefPtr<TTY> terminal;
    RefPtr<AddressSpace> space;
    {
        SpinLockGuard guard(m_describe_lock);
        terminal = m_controlling_terminal;
        space = m_address_space;
    }
    if (terminal)
        info.terminal = terminal->metadata().device;
    if (space && !is_zombie())
        info.resident_bytes = space->resident_pages() * page_size;
    info.cpu_nanoseconds = Time::nanoseconds_from_ticks(ticks);
    info.start_time = m_start_time;
    size_t length = min(m_name_length, sizeof(info.name) - 1);
    __builtin_memcpy(info.name, m_name, length);
}

static bool s_tracing { false };

void Process::set_tracing(bool tracing)
{
    s_tracing = tracing;
}

bool Process::is_tracing()
{
    return s_tracing;
}

RefPtr<AddressSpace> Process::address_space_reference()
{
    SpinLockGuard guard(m_describe_lock);
    return m_address_space;
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

void Process::add_thread(Thread& thread)
{
    SpinLockGuard guard(m_threads_lock);
    m_threads.append(thread);
    m_thread_objects.fetch_add(1);
}

RefPtr<Thread> Process::thread_with_id(u64 id)
{
    SpinLockGuard guard(m_threads_lock);
    for (auto& thread : m_threads) {
        if (thread.id() == id)
            return thread;
    }
    return {};
}

void Process::thread_finished(Thread& thread)
{
    m_exited_cpu_ticks.fetch_add(thread.cpu_ticks());
    if (m_thread_objects.fetch_sub(1) == 1) {
        // No thread runs on this address space anywhere now. (Freed outside the lock,
        // which is for moments.)
        RefPtr<AddressSpace> gone;
        {
            SpinLockGuard guard(m_describe_lock);
            gone = move(m_address_space);
        }
        return;
    }
    m_thread_gone_wait.wake_all();
}

String Process::current_directory()
{
    MutexLocker locker(m_cwd_lock);
    return m_cwd;
}

ErrorOr<void> Process::ensure_file_capacity(size_t wanted)
{
    for (;;) {
        size_t capacity;
        {
            SpinLockGuard guard(m_files_lock);
            capacity = m_file_capacity;
        }
        if (capacity >= wanted)
            return {};
        size_t new_capacity = max<size_t>(capacity * 2, 64);
        while (new_capacity < wanted)
            new_capacity *= 2;
        new_capacity = min(new_capacity, max_open_files);
        if (new_capacity < wanted)
            return Error::from_errno(EMFILE);
        // All zeroes is an empty FileDescriptor, and one can be moved by copying its bytes.
        auto* grown = static_cast<FileDescriptor*>(kmalloc(new_capacity * sizeof(FileDescriptor)));
        if (!grown)
            return Error::from_errno(ENOMEM);
        __builtin_memset(static_cast<void*>(grown), 0, new_capacity * sizeof(FileDescriptor));
        FileDescriptor* old = nullptr;
        {
            SpinLockGuard guard(m_files_lock);
            // Another thread may have got there first: then its table stays, and this goes.
            if (m_file_capacity < new_capacity) {
                if (m_files)
                    __builtin_memcpy(static_cast<void*>(grown), static_cast<void const*>(m_files), m_file_capacity * sizeof(FileDescriptor));
                old = m_files;
                m_files = grown;
                m_file_capacity = new_capacity;
                grown = nullptr;
            }
        }
        kfree(old);
        kfree(grown);
    }
}

Process::FileDescriptor* Process::file_entry(int fd)
{
    if (fd < 0 || static_cast<size_t>(fd) >= m_file_capacity)
        return nullptr;
    return &m_files[fd];
}

Process::FileDescriptor Process::take_file_descriptor(int fd)
{
    SpinLockGuard guard(m_files_lock);
    auto* entry = file_entry(fd);
    if (!entry || !entry->description)
        return {};
    FileDescriptor taken = move(*entry);
    *entry = {};
    if (static_cast<size_t>(fd) < m_first_free_fd)
        m_first_free_fd = static_cast<size_t>(fd);
    return taken;
}

void Process::close_all_files()
{
    FileDescriptor* files;
    size_t capacity;
    {
        SpinLockGuard guard(m_files_lock);
        files = m_files;
        capacity = m_file_capacity;
        m_files = nullptr;
        m_file_capacity = 0;
        m_first_free_fd = 0;
    }
    if (!files)
        return;
    // Closed outside the lock: closing a pipe end wakes the other side.
    for (size_t fd = 0; fd < capacity; ++fd)
        files[fd] = {};
    kfree(files);
}

void Process::close_files_on_exec()
{
    for (size_t fd = 0;; ++fd) {
        FileDescriptor closing;
        {
            SpinLockGuard guard(m_files_lock);
            while (fd < m_file_capacity && !(m_files[fd].description && m_files[fd].close_on_exec))
                ++fd;
            if (fd >= m_file_capacity)
                return;
            closing = move(m_files[fd]);
            m_files[fd] = {};
            if (fd < m_first_free_fd)
                m_first_free_fd = fd;
        }
    }
}

ErrorOr<int> Process::allocate_fd(NonnullRefPtr<FileDescription> description, bool close_on_exec, int lowest)
{
    size_t const limit = min(static_cast<size_t>(min<u64>(m_limits[VULTURE_RLIMIT_NOFILE].rlim_cur, max_open_files)), max_open_files);
    for (;;) {
        size_t wanted;
        {
            SpinLockGuard guard(m_files_lock);
            size_t const start = max(static_cast<size_t>(lowest), m_first_free_fd);
            for (size_t fd = start; fd < m_file_capacity && fd < limit; ++fd) {
                if (!m_files[fd].description) {
                    m_files[fd] = { move(description), close_on_exec };
                    if (start == m_first_free_fd)
                        m_first_free_fd = fd + 1;
                    return static_cast<int>(fd);
                }
            }
            if (max(start, m_file_capacity) >= limit)
                return Error::from_errno(EMFILE);
            wanted = max(start, m_file_capacity) + 1;
        }
        TRY(ensure_file_capacity(wanted));
    }
}

ErrorOr<NonnullRefPtr<FileDescription>> Process::file(int fd)
{
    SpinLockGuard guard(m_files_lock);
    auto* entry = file_entry(fd);
    if (!entry || !entry->description)
        return Error::from_errno(EBADF);
    return NonnullRefPtr<FileDescription>(*entry->description);
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
    process->set_name("vlaunchd"_sv);

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
    TRY(process->ensure_file_capacity(3));
    for (size_t fd = 0; fd < 3; ++fd)
        process->m_files[fd] = { console, false };
    process->m_first_free_fd = 3;

    auto thread = TRY(Thread::create("init"_sv, [](void* argument) {
        auto* init = static_cast<InitArguments*>(argument);
        auto& self = Thread::current();
        auto& frame = self.user_trap_frame();
        StringView const environment[] = { "PATH=/usr/bin:/bin:/usr/sbin:/sbin"_sv, "HOME=/var/root"_sv };
        auto result = self.process()->exec(StringView(init->path), { init->arguments, init->argument_count }, environment, frame);
        if (result.is_error()) {
            FixedStringBuilder<128> reason;
            format_to(reason, "cannot start {}: {}", StringView(init->path), result.error());
            panic(reason.view());
        }
        delete init;
        Arch::return_to_user(frame); }, init_arguments));
    thread->set_process(*process);
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
// The new program's stack: argc, then argv, environ and the auxiliary
// vector (pairs ending with AT_NULL), pointing at strings at the top.
struct AuxiliaryValue {
    u64 type;
    u64 value;
};

static ErrorOr<FlatPtr> build_initial_stack(Region& stack, FlatPtr top, Span<StringView const> arguments, Span<StringView const> environment, StringView path,
    Span<AuxiliaryValue const> auxiliary)
{
    constexpr size_t limit = 128 * KiB;
    size_t strings_size = path.length() + 1;
    for (auto string : arguments)
        strings_size += string.length() + 1;
    for (auto string : environment)
        strings_size += string.length() + 1;
    if (strings_size > limit || arguments.size() + environment.size() > 4096)
        return Error::from_errno(E2BIG);

    // AT_EXECFN, AT_RANDOM and AT_NULL follow the given pairs. Nobody can count on where in
    // the top page it all starts, either.
    size_t pointer_words = 1 + arguments.size() + 1 + environment.size() + 1 + 2 * (auxiliary.size() + 3);
    u8 random_bytes[16];
    u16 sub_page;
    get_random_bytes({ random_bytes, sizeof(random_bytes) });
    get_random_bytes({ reinterpret_cast<u8*>(&sub_page), sizeof(sub_page) });
    FlatPtr random_address = top - sizeof(random_bytes) - (sub_page % 256) * 16;
    FlatPtr strings_start = random_address - strings_size;
    FlatPtr sp = align_down<FlatPtr>(strings_start - pointer_words * sizeof(u64), 16);
    TRY(write_to_region(stack, random_address, { random_bytes, sizeof(random_bytes) }));

    auto* words = static_cast<u64*>(kzalloc(pointer_words * sizeof(u64)));
    if (!words)
        return Error::from_errno(ENOMEM);
    size_t word = 0;
    FlatPtr string_address = strings_start;
    auto place = [&](StringView string) -> ErrorOr<FlatPtr> {
        FlatPtr address = string_address;
        TRY(write_to_region(stack, address, { reinterpret_cast<u8 const*>(string.characters_without_null_termination()), string.length() }));
        string_address += string.length() + 1;
        return address;
    };
    auto fill = [&]() -> ErrorOr<void> {
        words[word++] = arguments.size();
        for (auto string : arguments)
            words[word++] = TRY(place(string));
        words[word++] = 0;
        for (auto string : environment)
            words[word++] = TRY(place(string));
        words[word++] = 0;
        for (auto const& entry : auxiliary) {
            words[word++] = entry.type;
            words[word++] = entry.value;
        }
        words[word++] = AT_EXECFN;
        words[word++] = TRY(place(path));
        words[word++] = AT_RANDOM;
        words[word++] = random_address;
        words[word++] = AT_NULL;
        words[word++] = 0;
        return write_to_region(stack, sp, { reinterpret_cast<u8 const*>(words), pointer_words * sizeof(u64) });
    };
    auto result = fill();
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

// One ELF image in a new address space, `bias` added to its addresses:
// the program, or its dynamic linker.
struct LoadedImage {
    FlatPtr bias { 0 };
    FlatPtr entry { 0 };
    FlatPtr program_headers { 0 }; // Where they are in memory (AT_PHDR).
    FlatPtr lowest { 0 };
};

static ErrorOr<LoadedImage> load_image(AddressSpace& space, ELF::Image const& image, FlatPtr base, StringView name)
{
    auto const& header = image.header();
    // An image linked for any address goes at `base`; one linked for its
    // own address (ET_EXEC) stays there.
    FlatPtr lowest_vaddr = ~FlatPtr(0);
    for (auto const& segment : image.program_headers()) {
        if (segment.p_type == ELF::PT_LOAD && segment.p_memsz > 0)
            lowest_vaddr = min(lowest_vaddr, align_down<FlatPtr>(segment.p_vaddr, page_size));
    }
    if (lowest_vaddr == ~FlatPtr(0))
        return Error::from_errno(ENOEXEC);
    LoadedImage loaded;
    loaded.bias = header.e_type == ELF::ET_DYN ? base - lowest_vaddr : 0;
    loaded.lowest = lowest_vaddr + loaded.bias;
    loaded.entry = header.e_entry + loaded.bias;
    if (!is_user_range(loaded.entry, 4))
        return Error::from_errno(ENOEXEC);

    for (auto const& segment : image.program_headers()) {
        if (segment.p_type == ELF::PT_PHDR)
            loaded.program_headers = segment.p_vaddr + loaded.bias;
        if (segment.p_type != ELF::PT_LOAD || segment.p_memsz == 0)
            continue;
        FlatPtr vaddr = segment.p_vaddr + loaded.bias;
        if (segment.p_filesz > segment.p_memsz || !is_user_range(vaddr, segment.p_memsz))
            return Error::from_errno(ENOEXEC);
        int protection = protection_from_segment_flags(segment.p_flags);
        if ((protection & PROT_WRITE) && (protection & PROT_EXEC))
            return Error::from_errno(ENOEXEC);
        FlatPtr start = align_down<FlatPtr>(vaddr, page_size);
        FlatPtr end = align_up<FlatPtr>(vaddr + segment.p_memsz, page_size);
        // Nothing of another segment, or of the other image, may be there.
        for (FlatPtr page = start; page < end; page += page_size) {
            if (space.find_region(page))
                return Error::from_errno(ENOEXEC);
        }
        auto* region = TRY(space.map_anonymous(start, end - start, protection, name, true));
        TRY(write_to_region(*region, vaddr, image.segment_data(segment)));
        // (exec checked the file, before it got here.)
        if (protection & PROT_EXEC)
            region->set_checked_code();
        if (protection & PROT_EXEC) {
            for (FlatPtr page = start; page < end; page += page_size) {
                auto physical = region->object().page(region->object_index(page));
                if (physical.get())
                    Arch::make_code_coherent(physical);
            }
        }
        // Without PT_PHDR: the headers inside the segment that maps the
        // start of the file.
        if (!loaded.program_headers && segment.p_offset <= header.e_phoff && header.e_phoff < segment.p_offset + segment.p_filesz)
            loaded.program_headers = vaddr + (header.e_phoff - segment.p_offset);
    }
    return loaded;
}

ErrorOr<void> Process::exec(StringView path, Span<StringView const> arguments, Span<StringView const> environment, TrapFrame& frame)
{
    auto directory = current_directory();
    auto resolved = TRY(VirtualFileSystem::resolve(path, directory.view()));
    TRY(sandbox_check(SandboxOperation::Execute, resolved.path.view()));
    auto executable = TRY(read_executable(*resolved.inode));

    // A script: "#!interpreter [argument]" runs the interpreter with the
    // script's path (and the original arguments after argv[0]).
    auto bytes = executable->bytes();
    if (bytes.size() >= 2 && bytes[0] == '#' && bytes[1] == '!') {
        if (m_interpreter_depth >= 2)
            return Error::from_errno(ELOOP);
        size_t end = 2;
        while (end < bytes.size() && end < 256 && bytes[end] != '\n')
            ++end;
        StringView line { reinterpret_cast<char const*>(bytes.data()) + 2, end - 2 };
        line = line.trim_whitespace();
        auto space = line.find(' ');
        StringView interpreter = space.has_value() ? line.substring_view(0, space.value()) : line;
        StringView argument = space.has_value() ? line.substring_view(space.value() + 1).trim_whitespace() : StringView {};
        if (interpreter.is_empty())
            return Error::from_errno(ENOEXEC);
        Vector<StringView> new_arguments;
        TRY(new_arguments.try_append(interpreter));
        if (!argument.is_empty())
            TRY(new_arguments.try_append(argument));
        TRY(new_arguments.try_append(path));
        for (size_t i = 1; i < arguments.size(); ++i)
            TRY(new_arguments.try_append(arguments[i]));
        ++m_interpreter_depth;
        auto result = exec(interpreter, { new_arguments.begin(), new_arguments.size() }, environment, frame);
        --m_interpreter_depth;
        return result;
    }

    auto image_or_error = ELF::Image::parse(executable->bytes());
    if (image_or_error.is_error())
        return Error::from_errno(ENOEXEC);
    auto const& image = image_or_error.value();
    auto const& header = image.header();
    if ((header.e_type != ELF::ET_EXEC && header.e_type != ELF::ET_DYN) || header.e_machine != ELF::EM_NATIVE)
        return Error::from_errno(ENOEXEC);

    // Is this something that may run at all?
    auto const origin = TRY(CodeSigning::check_program(*resolved.inode, executable->bytes(), resolved.path.view()));

    // A dynamically linked program names its dynamic linker (/usr/lib/dyld),
    // which the kernel loads too and starts instead; it loads the libraries.
    RefPtr<KernelBuffer> interpreter;
    Optional<ELF::Image> interpreter_image;
    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_INTERP)
            continue;
        auto name = image.segment_data(segment);
        size_t length = 0;
        while (length < name.size() && name[length] != 0)
            ++length;
        StringView interpreter_path { reinterpret_cast<char const*>(name.data()), length };
        if (length == 0 || length >= 256 || interpreter_path[0] != '/')
            return Error::from_errno(ENOEXEC);
        auto resolved_interpreter = TRY(VirtualFileSystem::resolve(interpreter_path, "/"_sv));
        interpreter = TRY(read_executable(*resolved_interpreter.inode));
        (void)TRY(CodeSigning::check_program(*resolved_interpreter.inode, interpreter->bytes(), interpreter_path));
        auto parsed = ELF::Image::parse(interpreter->bytes());
        if (parsed.is_error())
            return Error::from_errno(ENOEXEC);
        interpreter_image = parsed.release_value();
        auto const& interpreter_header = interpreter_image->header();
        if ((interpreter_header.e_type != ELF::ET_EXEC && interpreter_header.e_type != ELF::ET_DYN) || interpreter_header.e_machine != ELF::EM_NATIVE)
            return Error::from_errno(ENOEXEC);
        for (auto const& inner : interpreter_image->program_headers()) {
            if (inner.p_type == ELF::PT_INTERP)
                return Error::from_errno(ENOEXEC);
        }
        break;
    }
    // A position-independent program cannot relocate itself.
    if (header.e_type == ELF::ET_DYN && !interpreter)
        return Error::from_errno(ENOEXEC);

    // Installed code, and whatever it goes on to start: "/Applications/Hello.app", or the program
    // if it isn't in a bundle. (Set once, so that this can't be washed off by running something else.)
    if (origin == CodeSigning::Origin::ThirdParty && m_responsible_application.is_empty()) {
        auto const program_path = resolved.path.view();
        auto const bundle = program_path.find(".app/"_sv);
        m_responsible_application = TRY(String::from_view(bundle.has_value() ? program_path.substring_view(0, bundle.value() + 4) : program_path));
    }
    auto random_pages = [](u64 count) {
        u64 random;
        get_random_bytes({ reinterpret_cast<u8*>(&random), sizeof(random) });
        return (random % count) * page_size;
    };
    auto space = TRY(AddressSpace::create());
    auto program = TRY(load_image(*space, image, header.e_type == ELF::ET_DYN ? MemoryLayout::user_program_base + random_pages(MemoryLayout::user_program_random_pages) : 0, "program"_sv));
    Optional<LoadedImage> linker;
    if (interpreter_image.has_value())
        linker = TRY(load_image(*space, *interpreter_image, interpreter_image->header().e_type == ELF::ET_DYN ? MemoryLayout::user_interpreter_base + random_pages(MemoryLayout::user_interpreter_random_pages) : 0, "dyld"_sv));

    FlatPtr stack_top = MemoryLayout::user_stack_top - random_pages(MemoryLayout::user_stack_random_pages);
    FlatPtr stack_bottom = stack_top - MemoryLayout::user_stack_size;
    auto* stack = TRY(space->map_anonymous(stack_bottom, MemoryLayout::user_stack_size, PROT_READ | PROT_WRITE, "stack"_sv, true));
    // Set-user-ID and set-group-ID programs run as the file's owner and
    // group (from below); the dynamic linker must then trust no environment.
    auto program_file = resolved.inode->metadata();
    // Nothing gets out of a sandbox by way of another user.
    if (m_sandbox && (program_file.permissions & 06000))
        return Error::from_errno(EPERM);
    // A program that's being watched, and could be changed, doesn't get anyone else's rights.
    bool const honours_set_id = !m_tracer_pid.load();
    if (!honours_set_id)
        program_file.permissions &= ~06000u;
    u32 new_euid = (program_file.permissions & 04000) ? program_file.uid : m_credentials.euid;
    u32 new_egid = (program_file.permissions & 02000) ? program_file.gid : m_credentials.egid;
    bool secure = new_euid != m_credentials.uid || new_egid != m_credentials.gid;
    AuxiliaryValue const auxiliary[] = {
        { AT_PHDR, program.program_headers },
        { AT_PHENT, sizeof(ELF::Elf64_Phdr) },
        { AT_PHNUM, header.e_phnum },
        { AT_PAGESZ, page_size },
        { AT_BASE, linker.has_value() ? linker->lowest : 0 },
        { AT_FLAGS, 0 },
        { AT_ENTRY, program.entry },
        { AT_UID, m_credentials.uid },
        { AT_EUID, new_euid },
        { AT_GID, m_credentials.gid },
        { AT_EGID, new_egid },
        { AT_SECURE, secure ? 1u : 0u },
    };
    FlatPtr stack_pointer = TRY(build_initial_stack(*stack, stack_top, arguments, environment, path, { auxiliary, array_size(auxiliary) }));

    // The point of no return: the new program replaces the old one, as the
    // process's only thread.
    end_other_threads();
    m_ending_thread.store(nullptr);
    RefPtr<AddressSpace> old_space;
    {
        // No switch may see the process between the two address spaces (the lock masks
        // interrupts), nor anyone looking in from outside.
        SpinLockGuard guard(m_describe_lock);
        old_space = move(m_address_space);
        m_address_space = move(space);
        m_address_space->activate();
    }
    // No thread uses the old one now (this process has one). It goes here, or with
    // whoever is looking at it.
    old_space.clear();
    {
        auto& thread = Thread::current();
        thread.m_alternate_stack_size = 0;
        thread.m_clear_tid_address = 0;
        Arch::set_current_user_thread_pointer(0);
    }

    if (s_tracing)
        dmesgln("process: {} execs {} (user {})", m_pid, path, m_credentials.euid);

    m_executable = executable;
    m_exec_count.fetch_add(1);
    m_cancel_handler.store(0);
    notify_pollers();
    // The tracer gets to look before the first instruction.
    if (m_tracer_pid.load()) {
        vulture_siginfo info {};
        info.si_code = SI_KERNEL;
        force_signal(SIGTRAP, info);
    }
    m_executable_bias = program.bias;
    m_interpreter = interpreter;
    m_interpreter_bias = linker.has_value() ? linker->bias : 0;
    if (program_file.permissions & 04000) {
        m_credentials.euid = program_file.uid;
        m_credentials.saved_uid = program_file.uid;
    }
    if (program_file.permissions & 02000) {
        m_credentials.egid = program_file.gid;
        m_credentials.saved_gid = program_file.gid;
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
    close_files_on_exec();
    Arch::prepare_user_entry(frame, linker.has_value() ? linker->entry : program.entry, stack_pointer);
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
    if (s_tracing)
        dmesgln("process: {} ({}) ends with status {:#x}", m_pid, StringView(m_name, m_name_length), status);
    VERIFY(this == current());
    end_other_threads();
    if (this == s_init) {
        FixedStringBuilder<64> reason;
        format_to(reason, "init exited (status {:#x})", status);
        panic(reason.view());
    }
    // Whoever it's been tracing carries on without it. And whoever's been tracing this has seen the last of it: what it ended with is
    // for its real parent.
    for (auto& process : Process::all()) {
        if (process->m_tracer_pid.load() == m_pid && process.ptr() != this)
            process->detach_from_tracer(0);
    }
    if (m_tracer_pid.load())
        detach_from_tracer(0);
    close_all_files();
    m_address_space->remove_all_regions();
    lose_controlling_terminal();
    m_alarm_timer.cancel();
    m_cpu_timer_look.cancel();

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
    // A kqueue somewhere may be waiting for this.
    notify_pollers();
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
    exit_current_thread();
}

// --- Threads ---

bool Process::must_end(Thread const& thread) const
{
    auto* ending = m_ending_thread.load();
    return ending && ending != &thread;
}

void Process::end_other_threads()
{
    auto& self = Thread::current();
    Thread* expected = nullptr;
    if (!m_ending_thread.compare_exchange_strong(expected, &self))
        exit_current_thread();
    {
        SpinLockGuard guard(m_threads_lock);
        for (auto& thread : m_threads) {
            if (&thread != &self)
                thread.interrupt_for_signal();
        }
    }
    // Those stopped with the process (it may be stopped by now).
    m_continue_wait.wake_all();
    // Not just out of the list: off every processor, too.
    m_thread_gone_wait.wait_until([&] { return m_thread_objects.load() == 1; });
}

void Process::exit_current_thread()
{
    auto& thread = Thread::current();
    {
        SpinLockGuard guard(m_threads_lock);
        if (thread.m_process_node.is_in_list())
            m_threads.remove(thread);
    }
    // It may have been the thread a signal was meant to interrupt.
    if (!m_ending_thread.load())
        reroute_pending_signals(m_pending_signals.load());
    Thread::exit();
}

// --- Signals ---

bool Process::has_deliverable_signal(Thread const& thread) const
{
    if (must_end(thread))
        return true;
    u64 pending = m_pending_signals.load(MemoryOrder::Acquire) | thread.m_pending_signals.load(MemoryOrder::Acquire);
    return (pending & ~thread.m_signal_mask.load(MemoryOrder::Relaxed)) || (pending & unblockable_signals);
}

bool Process::has_deliverable_signal() const
{
    return has_deliverable_signal(Thread::current());
}

bool Process::has_work_before_user() const
{
    auto const& thread = Thread::current();
    return has_deliverable_signal() || m_stopped.load() || (thread.cancellation_is_due() && m_cancel_handler.load());
}

void Process::enter_cancel_handler(TrapFrame& frame)
{
    auto& thread = Thread::current();
    auto const handler = m_cancel_handler.load();
    if (!handler)
        return;
    // Only the once: it's on its way out, and has things to do first that mustn't be interrupted by this.
    thread.set_cancel_enabled(false);
    thread.m_syscall_interrupted = false;
    Arch::enter_user_function(frame, handler);
}

void Process::finish_interrupted_syscall(TrapFrame& frame)
{
    auto& thread = Thread::current();
    if (!thread.m_syscall_interrupted)
        return;
    thread.m_syscall_interrupted = false;
    Arch::rewind_syscall(frame, thread.m_interrupted_syscall, thread.m_interrupted_syscall_argument);
}

void Process::do_work_before_user(TrapFrame& frame)
{
    auto& thread = Thread::current();
    if (must_end(thread))
        exit_current_thread();
    u64 pending = m_pending_signals.load() | thread.m_pending_signals.load();
    if (m_stopped.load() && !(pending & bit(SIGKILL))) {
        wait_while_stopped(thread);
        return;
    }
    // Before anything else this thread might do on its way back: it isn't.
    if (thread.cancellation_is_due() && m_cancel_handler.load()) {
        enter_cancel_handler(frame);
        return;
    }
    dispatch_signal(frame);
}

void Process::interrupt_a_thread_for(int signal)
{
    SpinLockGuard guard(m_threads_lock);
    for (auto& thread : m_threads) {
        if ((bit(signal) & unblockable_signals) || !(thread.m_signal_mask.load() & bit(signal))) {
            thread.interrupt_for_signal();
            return;
        }
    }
}

void Process::reroute_pending_signals(u64 signals)
{
    while (signals) {
        interrupt_a_thread_for(__builtin_ctzll(signals));
        signals &= signals - 1;
    }
}

bool Process::blocked_by_every_thread(int signal)
{
    SpinLockGuard guard(m_threads_lock);
    for (auto& thread : m_threads) {
        if (!(thread.m_signal_mask.load() & bit(signal)))
            return false;
    }
    return true;
}

static bool is_ignored(SignalAction const& action, int signal);

static constexpr u64 stop_signals = bit(SIGSTOP) | bit(SIGTSTP) | bit(SIGTTIN) | bit(SIGTTOU);

void Process::detach_from_tracer(int signal)
{
    {
        SpinLockGuard guard(s_tree_lock);
        m_tracer_pid.store(0);
        // Back where it was, if that's still around. Otherwise to whoever looks after orphans.
        Process* parent_before = nullptr;
        for (auto& candidate : s_processes) {
            if (candidate.pid() == m_parent_before_tracing && !candidate.is_zombie()) {
                parent_before = &candidate;
                break;
            }
        }
        if (!parent_before)
            parent_before = s_init;
        if (m_parent != parent_before && parent_before != this) {
            if (m_parent)
                m_parent->m_children.remove(*this);
            m_parent = parent_before;
            parent_before->m_children.append(*this);
        }
    }
    if (m_trace_stopped.load())
        resume_from_tracer(signal);
}

u64 Process::cpu_ticks_used()
{
    u64 ticks = m_exited_cpu_ticks.load();
    SpinLockGuard guard(m_threads_lock);
    for (auto& thread : m_threads)
        ticks += thread.cpu_ticks();
    return ticks;
}

void Process::look_at_cpu_timers()
{
    u64 const used = cpu_ticks_used();
    bool fired[2] {};
    bool any_left = false;
    {
        SpinLockGuard guard(m_cpu_timers_lock);
        for (int which = 0; which < 2; ++which) {
            auto& timer = m_cpu_timers[which];
            if (timer.next && used >= timer.next) {
                fired[which] = true;
                timer.next = timer.interval ? used + timer.interval : 0;
            }
            any_left = any_left || timer.next;
        }
    }
    vulture_siginfo info {};
    info.si_code = SI_KERNEL;
    if (fired[0])
        send_signal(SIGVTALRM, info);
    if (fired[1])
        send_signal(SIGPROF, info);
    if (any_left) {
        Processor::current().timer_queue().add(m_cpu_timer_look, Time::now() + Time::ticks_from_microseconds(cpu_timer_look_microseconds), [](Timer& timer) {
            static_cast<Process*>(timer.context)->look_at_cpu_timers();
        });
    }
}

bool Process::is_using(FileSystem const& file_system, StringView path)
{
    {
        SpinLockGuard guard(m_files_lock);
        for (size_t fd = 0; fd < m_file_capacity; ++fd) {
            auto const& entry = m_files[fd];
            if (entry.description && entry.description->inode() && &entry.description->inode()->file_system() == &file_system)
                return true;
        }
    }
    {
        MutexLocker locker(m_cwd_lock);
        auto const cwd = m_cwd.view();
        if (cwd == path || (cwd.starts_with(path) && cwd.length() > path.length() && cwd[path.length()] == '/'))
            return true;
    }
    auto space = address_space_reference();
    return space && space->maps_file_system(file_system);
}

void Process::send_signal(int signal, vulture_siginfo const& info)
{
    VERIFY(signal > 0 && signal < NSIG);
    // Whether anything comes of it or not: kqueue says it was sent.
    m_signal_counts[signal].fetch_add(1);
    notify_pollers();
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
        // sleeps or waits (POSIX). One ignored only by default stays while
        // every thread blocks it: sigwait() may want it.
        auto const& action = m_signal_actions[signal];
        if (is_ignored(action, signal) && !(action.handler == VULTURE_SIG_DFL && blocked_by_every_thread(signal)))
            return;
        m_signal_info[signal] = info;
        m_signal_info[signal].si_signo = signal;
        m_pending_signals.fetch_or(bit(signal));
    }
    if (signal == SIGKILL)
        m_continue_wait.wake_all();
    m_suspend_wait.wake_all();
    interrupt_a_thread_for(signal);
}

void Process::send_signal_to_thread(Thread& thread, int signal, vulture_siginfo const& info)
{
    VERIFY(signal > 0 && signal < NSIG);
    if (signal == SIGCONT)
        continue_running();
    {
        SpinLockGuard guard(m_signal_lock);
        auto const& action = m_signal_actions[signal];
        if (is_ignored(action, signal) && !(action.handler == VULTURE_SIG_DFL && (thread.m_signal_mask.load() & bit(signal))))
            return;
        thread.m_signal_info[signal] = info;
        thread.m_signal_info[signal].si_signo = signal;
        thread.m_pending_signals.fetch_or(bit(signal));
    }
    if (signal == SIGKILL)
        m_continue_wait.wake_all();
    m_suspend_wait.wake_all();
    thread.interrupt_for_signal();
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
    // One that has finished is still a member until it's been waited for: a shell putting a pipeline
    // together can't be told that the group has gone because its first command was quick.
    for (auto& process : s_processes) {
        if (process.pgid() == pgid && process.sid() == sid)
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
    auto& thread = Thread::current();
    bool blocked = thread.process() == this && (thread.m_signal_mask.load() & bit(signal));
    return blocked || m_signal_actions[signal].handler == VULTURE_SIG_IGN;
}

ErrorOr<void> Process::set_controlling_terminal(TTY& tty)
{
    if (m_sid.load() != m_pid || m_controlling_terminal)
        return Error::from_errno(EPERM);
    if (tty.session() != 0 && tty.session() != m_pid)
        return Error::from_errno(EPERM);
    tty.set_session(m_pid, pgid());
    SpinLockGuard guard(m_describe_lock);
    m_controlling_terminal = tty;
    return {};
}

void Process::lose_controlling_terminal()
{
    RefPtr<TTY> tty;
    {
        SpinLockGuard guard(m_describe_lock);
        tty = move(m_controlling_terminal);
    }
    if (tty && m_sid.load() == m_pid)
        tty->release_session(m_pid);
}

void Process::continue_running()
{
    // Its tracer says when that is. SIGCONT stays pending, for it to hear about.
    if (m_trace_stopped.load())
        return;
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
    // The others stop on their way to user mode: those running get there now.
    {
        SpinLockGuard guard(m_threads_lock);
        for (auto& thread : m_threads)
            Scheduler::kick(thread);
    }
    wait_while_stopped(Thread::current());
    m_stopped.store(false);
}

// Until SIGCONT (continue_running), SIGKILL, or the thread must end.
void Process::wait_while_stopped(Thread const& thread)
{
    m_continue_wait.wait_until([&] {
        return !m_stopped.load() || ((m_pending_signals.load() | thread.m_pending_signals.load()) & bit(SIGKILL)) || must_end(thread);
    });
}

void Process::force_signal(int signal, vulture_siginfo const& info)
{
    // A fault cannot be ignored or blocked: that would retry it forever. It
    // is the faulting thread's.
    auto& thread = Thread::current();
    {
        SpinLockGuard guard(m_signal_lock);
        thread.m_signal_mask.fetch_and(~bit(signal));
        if (m_signal_actions[signal].handler == VULTURE_SIG_IGN)
            m_signal_actions[signal] = {};
    }
    send_signal_to_thread(thread, signal, info);
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
    case SIGIO:
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
    auto& thread = Thread::current();
    int signal = 0;
    SignalAction action;
    vulture_siginfo info;
    {
        SpinLockGuard guard(m_signal_lock);
        u64 mask = thread.m_signal_mask.load();
        // Its own first: a fault must be handled before it happens again.
        u64 own = thread.m_pending_signals.load();
        own = (own & ~mask) | (own & unblockable_signals);
        u64 shared = m_pending_signals.load();
        shared = (shared & ~mask) | (shared & unblockable_signals);
        if (own) {
            signal = __builtin_ctzll(own);
            thread.m_pending_signals.fetch_and(~bit(signal));
            info = thread.m_signal_info[signal];
        } else if (shared) {
            signal = __builtin_ctzll(shared);
            m_pending_signals.fetch_and(~bit(signal));
            info = m_signal_info[signal];
        } else {
            return;
        }
        action = m_signal_actions[signal];
    }

    // Its tracer hears about everything but the end of it, and may change its mind about what this is.
    if (signal != SIGKILL && m_tracer_pid.load()) {
        int const decided = stop_for_tracer(frame, signal);
        if (decided == 0)
            return;
        if (decided != signal) {
            signal = decided;
            info = {};
            info.si_signo = signal;
            info.si_code = SI_USER;
            info.si_pid = m_tracer_pid.load();
        }
        SpinLockGuard guard(m_signal_lock);
        action = m_signal_actions[signal];
    }

    if (signal == SIGKILL || signal == SIGSTOP || action.handler == VULTURE_SIG_DFL) {
        auto what = default_action(signal);
        if (what == DefaultAction::Ignore)
            return;
        if (what == DefaultAction::Stop) {
            stop(signal);
            return;
        }
        // A crash report for faults, and for signals a process sends
        // itself (abort()); not for `kill -SEGV` from elsewhere.
        if (what == DefaultAction::CrashReport && (info.si_code != SI_USER || info.si_pid == m_pid))
            report_crash(frame, signal, info);
        bool const dumped = what == DefaultAction::CrashReport && write_core_dump(frame, signal);
        terminate(VULTURE_W_SIGNALED(signal, dumped));
    }
    if (action.handler == VULTURE_SIG_IGN)
        return;

    // A handler's going to run. What it interrupted says EINTR, unless the handler said otherwise: then it's made again, when that returns.
    if (thread.m_syscall_interrupted) {
        thread.m_syscall_interrupted = false;
        if (action.flags & SA_RESTART)
            Arch::rewind_syscall(frame, thread.m_interrupted_syscall, thread.m_interrupted_syscall_argument);
    }

    // After sigsuspend(), the handler's return restores the mask from before it.
    u64 saved_mask = thread.m_mask_after_suspend.has_value() ? thread.m_mask_after_suspend.release_value() : thread.m_signal_mask.load();
    // The alternate stack, if the handler wants it and is not on it already.
    FlatPtr stack_top = 0;
    if ((action.flags & SA_ONSTACK) && thread.m_alternate_stack_size) {
        FlatPtr sp = Arch::user_stack_pointer(frame);
        if (sp < thread.m_alternate_stack || sp - thread.m_alternate_stack >= thread.m_alternate_stack_size)
            stack_top = thread.m_alternate_stack + thread.m_alternate_stack_size;
    }
    if (Arch::push_signal_frame(frame, signal, info, saved_mask, action.handler, action.restorer, stack_top).is_error()) {
        // No room for the frame (the stack is gone or bad): fatal.
        report_crash(frame, SIGSEGV, info);
        terminate(VULTURE_W_SIGNALED(SIGSEGV, false));
    }
    SpinLockGuard guard(m_signal_lock);
    u64 mask = thread.m_signal_mask.load() | action.mask;
    if (!(action.flags & SA_NODEFER))
        mask |= bit(signal);
    thread.m_signal_mask.store(mask & ~unblockable_signals);
    if (action.flags & SA_RESETHAND)
        m_signal_actions[signal] = {};
}

bool Process::is_being_killed() const
{
    return m_pending_signals.load() & bit(SIGKILL);
}

void Process::handle_user_fault(TrapFrame& frame, FlatPtr address, AccessKind, bool permission_fault, PageFaultResult result)
{
    if (result == PageFaultResult::OutOfMemory) {
        // On its way out already: the signal takes it from here, on the way back to user mode.
        if (is_being_killed())
            return;
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
    info.si_code = SI_KERNEL;
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

namespace {

// Names the addresses of a crash report: in the program, in the dynamic
// linker, or in a library mapped from a file (read once for the report),
// the last two with the library's name, as in "abort+0x14 (libSystem.B.dylib)".
class CrashSymbolizer {
public:
    CrashSymbolizer(AddressSpace& space, RefPtr<KernelBuffer> program, FlatPtr program_bias, RefPtr<KernelBuffer> linker, FlatPtr linker_bias)
        : m_space(space)
        , m_program_bias(program_bias)
        , m_linker_bias(linker_bias)
    {
        if (program) {
            if (auto parsed = ELF::Image::parse(program->bytes()); !parsed.is_error())
                m_program = parsed.release_value();
        }
        if (linker) {
            if (auto parsed = ELF::Image::parse(linker->bytes()); !parsed.is_error())
                m_linker = parsed.release_value();
        }
    }

    void write(FormatSink& sink, FlatPtr address)
    {
        if (m_program.has_value() && m_program->contains_address(address - m_program_bias)) {
            write_user_symbol(sink, m_program, address - m_program_bias);
            return;
        }
        if (m_linker.has_value() && m_linker->contains_address(address - m_linker_bias)) {
            write_user_symbol(sink, m_linker, address - m_linker_bias);
            sink.write(" (dyld)"_sv);
            return;
        }
        auto location = m_space.file_location(address);
        if (!location.has_value() || !location->file) {
            sink.write("??"_sv);
            return;
        }
        if (location->file.ptr() != m_library_file.ptr()) {
            m_library_file = location->file;
            m_library_bytes = nullptr;
            m_library = {};
            if (auto bytes = read_executable(*location->file); !bytes.is_error()) {
                m_library_bytes = bytes.release_value();
                if (auto parsed = ELF::Image::parse(m_library_bytes->bytes()); !parsed.is_error())
                    m_library = parsed.release_value();
            }
        }
        Optional<u64> vaddr = m_library.has_value() ? m_library->address_of_offset(location->offset) : Optional<u64> {};
        if (!vaddr.has_value()) {
            sink.write("??"_sv);
            return;
        }
        write_user_symbol(sink, m_library, vaddr.value());
        if (auto name = m_library->soname(); name.has_value())
            format_to(sink, " ({})", name.value());
    }

private:
    AddressSpace& m_space;
    Optional<ELF::Image> m_program;
    FlatPtr m_program_bias;
    Optional<ELF::Image> m_linker;
    FlatPtr m_linker_bias;
    RefPtr<Inode> m_library_file;
    RefPtr<KernelBuffer> m_library_bytes;
    Optional<ELF::Image> m_library;
};

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
#if defined(__aarch64__)
    user_view.sp = frame.user_sp;
#endif
    FixedStringBuilder<2048> registers;
    Arch::dump_registers(registers, user_view);
    kernel_log().write(registers.view());

    CrashSymbolizer symbolizer(*m_address_space, m_executable, m_executable_bias, m_interpreter, m_interpreter_bias);
    dmesgln("Backtrace:");
    FixedStringBuilder<256> line;
    format_to(line, "  #0  {:#018x}  ", frame.program_counter());
    symbolizer.write(line, frame.program_counter());
    dmesgln("{}", line.view());
    FlatPtr frame_pointer = frame.frame_pointer();
    for (size_t index = 1; index < 32 && frame_pointer; ++index) {
        auto record = copy_from_user<Arch::StackFrame>(frame_pointer);
        if (record.is_error() || record.value().return_address == 0)
            break;
        // Without the signature, if it is signed.
        FlatPtr const return_address = record.value().return_address & 0x0000'ffff'ffff'ffff;
        line.clear();
        format_to(line, "  #{:<2} {:#018x}  ", index, return_address);
        symbolizer.write(line, return_address - 4);
        dmesgln("{}", line.view());
        if (record.value().previous_frame_pointer <= frame_pointer)
            break;
        frame_pointer = record.value().previous_frame_pointer;
    }
}

}
