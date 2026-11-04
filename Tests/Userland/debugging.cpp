/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <LibELF/ELF.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// What a debugger needs: to stop a program, look at it, change it and let it go on; and to find out afterwards what it was doing, from
// what it left behind.

namespace {

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "debugging: line %d: %s: %s\n", __LINE__, #condition, strerror(errno)); \
            exit(1);                                                                           \
        }                                                                                      \
    } while (0)

// Somewhere in memory for the tracer to find, in the copy that fork() gives the child.
volatile unsigned long long s_shared_value = 0x1234'5678'9abc'def0ull;
volatile int s_breakpoint_reached = 0;

__attribute__((noinline)) int a_function_to_break_in(int value)
{
    s_breakpoint_reached = s_breakpoint_reached + 1;
    return value + 1;
}

int wait_stopped(pid_t pid)
{
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "debugging: expected it to stop; status %#x\n", status);
        exit(1);
    }
    return WSTOPSIG(status);
}

unsigned long long read_word(pid_t pid, void const* address)
{
    unsigned long long value = 0;
    vulture_ptrace_io io { VULTURE_PTRACE_READ, const_cast<void*>(address), &value, sizeof(value) };
    CHECK(ptrace(VULTURE_PT_IO, pid, &io, 0) == 0);
    return value;
}

void write_bytes(pid_t pid, void const* address, void const* bytes, size_t size)
{
    vulture_ptrace_io io { VULTURE_PTRACE_WRITE, const_cast<void*>(address), const_cast<void*>(bytes), size };
    CHECK(ptrace(VULTURE_PT_IO, pid, &io, 0) == 0);
}

unsigned long long program_counter(vulture_thread_state const& state)
{
#if defined(__aarch64__)
    return state.pc;
#else
    return state.rip;
#endif
}

void set_program_counter(vulture_thread_state& state, unsigned long long value)
{
#if defined(__aarch64__)
    state.pc = value;
#else
    state.rip = value;
#endif
}

void stopping_and_looking()
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        ptrace(PT_TRACE_ME, 0, nullptr, 0);
        raise(SIGSTOP);
        _exit(s_shared_value == 0xfeedfacecafebeefull ? 11 : 12);
    }
    CHECK(wait_stopped(child) == SIGSTOP);
    printf("debugging: a signal stops it for the tracer\n");

    errno = 0;
    CHECK(read_word(child, const_cast<unsigned long long*>(&s_shared_value)) == 0x1234'5678'9abc'def0ull);
    unsigned long long const replacement = 0xfeedfacecafebeefull;
    write_bytes(child, const_cast<unsigned long long*>(&s_shared_value), &replacement, sizeof(replacement));
    CHECK(read_word(child, const_cast<unsigned long long*>(&s_shared_value)) == replacement);
    printf("debugging: its memory can be read and changed\n");
    CHECK(s_shared_value == 0x1234'5678'9abc'def0ull);

    vulture_thread_state state {};
    CHECK(ptrace(VULTURE_PT_GETREGS, child, &state, 0) == 0);
    CHECK(program_counter(state) != 0);
    vulture_thread_state again = state;
    CHECK(ptrace(VULTURE_PT_SETREGS, child, &again, 0) == 0);
    printf("debugging: registers read and written\n");

    errno = 0;
    vulture_ptrace_io bad { VULTURE_PTRACE_READ, reinterpret_cast<void*>(8), &again, 8 };
    CHECK(ptrace(VULTURE_PT_IO, child, &bad, 0) < 0 && errno == EFAULT);

    CHECK(ptrace(PT_CONTINUE, child, reinterpret_cast<void*>(1), 0) == 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 11);
    printf("debugging: and it carries on, with what was changed\n");
}

volatile sig_atomic_t s_handled = 0;

void signals_are_decided_by_the_tracer()
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        ptrace(PT_TRACE_ME, 0, nullptr, 0);
        signal(SIGUSR1, [](int) { s_handled = s_handled + 1; });
        raise(SIGUSR1); // The tracer lets this through.
        raise(SIGUSR2); // This would kill it, if the tracer let it through.
        raise(SIGUSR2);
        _exit(s_handled == 2 ? 21 : 22);
    }
    CHECK(wait_stopped(child) == SIGUSR1);
    CHECK(ptrace(PT_CONTINUE, child, reinterpret_cast<void*>(1), SIGUSR1) == 0);
    CHECK(wait_stopped(child) == SIGUSR2);
    CHECK(ptrace(PT_CONTINUE, child, reinterpret_cast<void*>(1), 0) == 0);
    CHECK(wait_stopped(child) == SIGUSR2);
    // Something else, instead.
    CHECK(ptrace(PT_CONTINUE, child, reinterpret_cast<void*>(1), SIGUSR1) == 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 21);
    printf("debugging: signals go through, go away, or become others\n");

    child = fork();
    if (child == 0) {
        ptrace(PT_TRACE_ME, 0, nullptr, 0);
        raise(SIGSTOP);
        _exit(0);
    }
    CHECK(wait_stopped(child) == SIGSTOP);
    CHECK(ptrace(PT_KILL, child, nullptr, 0) == 0);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    printf("debugging: PT_KILL\n");
}

void starting_a_program()
{
    pid_t child = fork();
    if (child == 0) {
        ptrace(PT_TRACE_ME, 0, nullptr, 0);
        execl("/bin/echo", "echo", "debugging: the traced program ran", nullptr);
        _exit(99);
    }
    CHECK(wait_stopped(child) == SIGTRAP);
    printf("debugging: exec stops it before its first instruction\n");
    vulture_thread_state state {};
    CHECK(ptrace(VULTURE_PT_GETREGS, child, &state, 0) == 0);
    CHECK(ptrace(PT_CONTINUE, child, reinterpret_cast<void*>(1), 0) == 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void breakpoints_and_steps()
{
#if defined(__aarch64__)
    unsigned int const breakpoint = 0xd4200000; // brk #0
    size_t const instruction_size = sizeof(breakpoint);
#else
    unsigned char const breakpoint = 0xcc; // int3
    size_t const instruction_size = sizeof(breakpoint);
#endif
    auto* target = reinterpret_cast<void*>(&a_function_to_break_in);

    pid_t child = fork();
    if (child == 0) {
        ptrace(PT_TRACE_ME, 0, nullptr, 0);
        raise(SIGSTOP);
        int const first = a_function_to_break_in(40);
        _exit(first == 41 && s_breakpoint_reached == 1 ? 31 : 32);
    }
    CHECK(wait_stopped(child) == SIGSTOP);
    unsigned long long const original = read_word(child, target);
    write_bytes(child, target, &breakpoint, instruction_size);
    CHECK(ptrace(PT_CONTINUE, child, reinterpret_cast<void*>(1), 0) == 0);
    CHECK(wait_stopped(child) == SIGTRAP);
    vulture_thread_state state {};
    CHECK(ptrace(VULTURE_PT_GETREGS, child, &state, 0) == 0);
#if defined(__aarch64__)
    CHECK(program_counter(state) == reinterpret_cast<unsigned long long>(target));
#else
    // Having done it, this one's past it.
    CHECK(program_counter(state) == reinterpret_cast<unsigned long long>(target) + 1);
#endif
    printf("debugging: a breakpoint in its code stops it, there\n");

    // As it was. It's a private copy of that page: anyone else with this code didn't see that.
    write_bytes(child, target, &original, instruction_size);
    set_program_counter(state, reinterpret_cast<unsigned long long>(target));
    CHECK(ptrace(VULTURE_PT_SETREGS, child, &state, 0) == 0);

    // One instruction at a time.
    auto last = program_counter(state);
    int moved = 0;
    for (int i = 0; i < 4; ++i) {
        CHECK(ptrace(PT_STEP, child, reinterpret_cast<void*>(1), 0) == 0);
        CHECK(wait_stopped(child) == SIGTRAP);
        CHECK(ptrace(VULTURE_PT_GETREGS, child, &state, 0) == 0);
        if (program_counter(state) != last)
            ++moved;
        last = program_counter(state);
    }
    CHECK(moved >= 3);
    printf("debugging: stepping goes one instruction at a time\n");

    CHECK(ptrace(PT_CONTINUE, child, reinterpret_cast<void*>(1), 0) == 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 31);
    printf("debugging: and it finishes as if nothing had happened\n");
}

void attaching()
{
    int ready[2];
    int done[2];
    CHECK(pipe(ready) == 0 && pipe(done) == 0);
    pid_t middle = fork();
    if (middle == 0) {
        // A process that's not a child of the one attaching... from the one that made it.
        close(ready[0]);
        close(done[1]);
        char byte = 'r';
        CHECK(write(ready[1], &byte, 1) == 1);
        CHECK(read(done[0], &byte, 1) >= 0);
        _exit(41);
    }
    close(ready[1]);
    close(done[0]);
    char byte;
    CHECK(read(ready[0], &byte, 1) == 1);
    CHECK(ptrace(PT_ATTACH, middle, nullptr, 0) == 0);
    CHECK(wait_stopped(middle) == SIGSTOP);
    printf("debugging: PT_ATTACH stops what's running\n");

    errno = 0;
    CHECK(ptrace(PT_ATTACH, middle, nullptr, 0) < 0 && errno == EBUSY);
    errno = 0;
    CHECK(ptrace(PT_ATTACH, getpid(), nullptr, 0) < 0 && errno == EINVAL);
    errno = 0;
    CHECK(ptrace(PT_ATTACH, 1, nullptr, 0) < 0 && errno == EINVAL);
    errno = 0;
    CHECK(ptrace(PT_ATTACH, 999999, nullptr, 0) < 0 && errno == ESRCH);
    errno = 0;
    CHECK(ptrace(PT_CONTINUE, 999999, reinterpret_cast<void*>(1), 0) < 0 && errno == ESRCH);

    CHECK(ptrace(PT_DETACH, middle, reinterpret_cast<void*>(1), 0) == 0);
    errno = 0;
    CHECK(ptrace(PT_CONTINUE, middle, reinterpret_cast<void*>(1), 0) < 0 && errno == EPERM);
    CHECK(write(done[1], "g", 1) == 1);
    int status = 0;
    CHECK(waitpid(middle, &status, 0) == middle);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 41);
    printf("debugging: PT_DETACH lets it go\n");
    close(ready[0]);
    close(done[1]);

    // Not everything's for looking at.
    int sync[2];
    CHECK(pipe(sync) == 0);
    pid_t denied = fork();
    if (denied == 0) {
        ptrace(PT_DENY_ATTACH, 0, nullptr, 0);
        char go = 'x';
        (void)write(sync[1], &go, 1);
        pause();
        _exit(0);
    }
    CHECK(read(sync[0], &byte, 1) == 1);
    errno = 0;
    CHECK(ptrace(PT_ATTACH, denied, nullptr, 0) < 0 && errno == EPERM);
    printf("debugging: PT_DENY_ATTACH\n");
    kill(denied, SIGKILL);
    waitpid(denied, nullptr, 0);

    // A tracer that goes away doesn't take what it was tracing with it.
    int grandchild_pipe[2];
    CHECK(pipe(grandchild_pipe) == 0);
    pid_t traced = fork();
    if (traced == 0) {
        close(grandchild_pipe[0]);
        pid_t tracee = getpid();
        pid_t tracer = fork();
        if (tracer == 0) {
            if (ptrace(PT_ATTACH, tracee, nullptr, 0) != 0)
                _exit(1);
            int stopped = 0;
            (void)waitpid(tracee, &stopped, 0);
            _exit(0);
        }
        // Stops, for as long as the tracer lasts.
        int waited = 0;
        waitpid(tracer, &waited, 0);
        (void)write(grandchild_pipe[1], "y", 1);
        _exit(51);
    }
    close(grandchild_pipe[1]);
    char landed = 0;
    CHECK(read(grandchild_pipe[0], &landed, 1) == 1 && landed == 'y');
    int status_of_traced = 0;
    CHECK(waitpid(traced, &status_of_traced, 0) == traced);
    CHECK(WIFEXITED(status_of_traced) && WEXITSTATUS(status_of_traced) == 51);
    printf("debugging: a tracer's end lets it go, and back to its parent\n");
    close(grandchild_pipe[0]);
}

void core_dumps()
{
    constexpr unsigned long long marker = 0xc0dedbadc0dedbadull;
    static volatile unsigned long long s_in_memory = 0;
    pid_t child = fork();
    if (child == 0) {
        rlimit none { RLIM_INFINITY, RLIM_INFINITY };
        setrlimit(RLIMIT_CORE, &none);
        s_in_memory = marker;
        // Somewhere it isn't going to get away with.
        *reinterpret_cast<volatile int*>(8) = 1;
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(WCOREDUMP(status));
    printf("debugging: the status says a core was dumped\n");

    char path[64];
    snprintf(path, sizeof(path), "/cores/core.%d", child);
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    struct stat status_of_core;
    CHECK(fstat(fd, &status_of_core) == 0 && status_of_core.st_size > 4096);
    CHECK((status_of_core.st_mode & 0777) == 0400);
    auto* contents = static_cast<unsigned char*>(malloc(static_cast<size_t>(status_of_core.st_size)));
    CHECK(contents);
    ssize_t got = 0;
    while (got < status_of_core.st_size) {
        ssize_t const more = read(fd, contents + got, static_cast<size_t>(status_of_core.st_size - got));
        CHECK(more > 0);
        got += more;
    }
    close(fd);

    auto const* header = reinterpret_cast<ELF::Elf64_Ehdr const*>(contents);
    CHECK(memcmp(header->e_ident, "\x7f" "ELF", 4) == 0 && header->e_type == ELF::ET_CORE);
    auto const* segments = reinterpret_cast<ELF::Elf64_Phdr const*>(contents + header->e_phoff);
    CHECK(segments[0].p_type == ELF::PT_NOTE);

    auto const* note = reinterpret_cast<ELF::Elf64_Nhdr const*>(contents + segments[0].p_offset);
    CHECK(note->n_type == VULTURE_NOTE_THREAD && note->n_descsz == sizeof(vulture_core_thread));
    auto const* thread = reinterpret_cast<vulture_core_thread const*>(reinterpret_cast<unsigned char const*>(note + 1) + ((note->n_namesz + 3) & ~3u));
    CHECK(thread->signal == SIGSEGV && thread->pid == child);
    printf("debugging: the core says what ended it: signal %d, at %#llx\n", thread->signal, program_counter(thread->state));

    bool found = false;
    for (size_t i = 1; i < header->e_phnum; ++i) {
        auto const& segment = segments[i];
        auto const wanted = reinterpret_cast<unsigned long long>(&s_in_memory);
        if (segment.p_type == ELF::PT_LOAD && wanted >= segment.p_vaddr && wanted + 8 <= segment.p_vaddr + segment.p_filesz) {
            CHECK(memcmp(contents + segment.p_offset + (wanted - segment.p_vaddr), &marker, 8) == 0);
            found = true;
        }
    }
    CHECK(found);
    printf("debugging: and what was in its memory\n");
    free(contents);
    CHECK(unlink(path) == 0);

    // Nothing, by default, or asked for nothing.
    child = fork();
    if (child == 0) {
        rlimit none { 0, RLIM_INFINITY };
        setrlimit(RLIMIT_CORE, &none);
        *reinterpret_cast<volatile int*>(8) = 1;
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && !WCOREDUMP(status));
    snprintf(path, sizeof(path), "/cores/core.%d", child);
    struct stat absent;
    CHECK(stat(path, &absent) < 0 && errno == ENOENT);
    printf("debugging: no limit to speak of, no core\n");
}

}

int main()
{
    stopping_and_looking();
    signals_are_decided_by_the_tracer();
    starting_a_program();
    breakpoints_and_steps();
    attaching();
    core_dumps();
    printf("debugging: PASS\n");
    return 0;
}
