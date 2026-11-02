/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibBase/Types.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// The mitigations, from the side of a program trying to do what they are there to stop. Each
// attempt is in a child, as it should end in a signal; what the kernel itself must refuse, see
// Meta/hardening-test.sh.

namespace {

template<typename Function>
int run_in_child(Function function)
{
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        function();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (WIFSIGNALED(status))
        return WTERMSIG(status);
    return 0;
}

// Ends in a signal, whichever of these it is.
bool ends_in_memory_fault(int signal)
{
    return signal == SIGSEGV || signal == SIGBUS || signal == SIGILL;
}

// Calling what the compiler can't see through, or it decides that can't be what was meant.
[[gnu::noinline]] void call_at(void* address)
{
    void* volatile opaque = address;
    asm volatile("" ::: "memory");
    reinterpret_cast<void (*)()>(opaque)();
}

[[gnu::noinline]] void smash_stack(size_t count)
{
    char array[16];
    char volatile* volatile pointer = array;
    for (size_t i = 0; i < count; ++i)
        pointer[i] = 'A';
}

void test_write_xor_execute()
{
    // Not both at once, however it is asked for.
    void* both = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(both == MAP_FAILED && errno == EACCES);
    void* writable = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(writable != MAP_FAILED);
    CHECK(mprotect(writable, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == -1 && errno == EACCES);
    munmap(writable, 4096);
    int fd = open("/usr/lib/libSystem.B.dylib", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EACCES);
    close(fd);

    // Code isn't writable.
    CHECK(ends_in_memory_fault(run_in_child([] {
        auto* code = reinterpret_cast<u32 volatile*>(&write);
        *code = 0;
    })));
    // Constants aren't either.
    CHECK(ends_in_memory_fault(run_in_child([] {
        static char const constant[] = "stays as it is";
        *const_cast<char volatile*>(constant) = 'X';
    })));
    // And nothing that is written may run: not the heap, nor the stack, nor data.
    static constexpr u32 just_return[2] = { 0xd503201f, 0xd65f03c0 };
    CHECK(ends_in_memory_fault(run_in_child([] {
        void* heap = malloc(64);
        memcpy(heap, just_return, sizeof(just_return));
        call_at(heap);
    })));
    CHECK(ends_in_memory_fault(run_in_child([] {
        u32 on_stack[2];
        memcpy(on_stack, just_return, sizeof(just_return));
        call_at(on_stack);
    })));
    CHECK(ends_in_memory_fault(run_in_child([] {
        static u32 data[2];
        memcpy(data, just_return, sizeof(just_return));
        call_at(data);
    })));
    // Nor is there anything at address zero.
    CHECK(ends_in_memory_fault(run_in_child([] { *reinterpret_cast<u32 volatile*>(16) = 0; })));
    CHECK(ends_in_memory_fault(run_in_child([] { reinterpret_cast<void (*)()>(16)(); })));
    // And the kernel's half isn't there at all, from here.
    CHECK(ends_in_memory_fault(run_in_child([] { (void)*reinterpret_cast<u32 volatile*>(0xffffffff80000000); })));
}

void test_stack_protector()
{
    int const signal = run_in_child([] { smash_stack(256); });
    CHECK(signal == SIGABRT);
    // Short of the canary, nothing is wrong.
    CHECK(run_in_child([] { smash_stack(8); }) == 0);
}

// In the hardware's own bits.
constexpr unsigned long capability_pointer_authentication = 1ul << 30;
constexpr unsigned long capability_branch_targets = 1ul << 40;

[[gnu::noinline]] void change_my_return_address()
{
    (void)getpid(); // So that this isn't a leaf, which needn't sign what it never spills.
    auto* frame = static_cast<u64*>(__builtin_frame_address(0));
    frame[1] ^= 0x10;
}

[[gnu::noinline]] void somewhere_to_jump()
{
    printf("hardening: somewhere_to_jump ran\n");
}

void test_control_flow()
{
    unsigned long const capabilities = getauxval(AT_HWCAP);
    if (capabilities & capability_pointer_authentication) {
        // Changing a return address isn't enough: it has to be signed, with a key nobody has.
        CHECK(ends_in_memory_fault(run_in_child([] { change_my_return_address(); })));
        printf("hardening: a changed return address was caught (PAC)\n");
    } else {
        printf("hardening: this processor has no pointer authentication\n");
    }
    if (capabilities & capability_branch_targets) {
        // To where a function starts is fine, and into the middle of one isn't.
        CHECK(run_in_child([] { call_at(reinterpret_cast<void*>(&somewhere_to_jump)); }) == 0);
        CHECK(run_in_child([] { call_at(reinterpret_cast<u8*>(&somewhere_to_jump) + 12); }) == SIGILL);
        printf("hardening: a jump into a function was caught (BTI)\n");
    } else {
        printf("hardening: this processor has no branch target checks\n");
    }
}

// One number per thing whose place should move: a run of this program, printing them.
struct Addresses {
    u64 values[6];
};

Addresses addresses_of_this_run()
{
    Addresses addresses;
    int on_stack;
    void* in_heap = malloc(32);
    void* mapped = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    addresses.values[0] = reinterpret_cast<u64>(&addresses_of_this_run);
    addresses.values[1] = reinterpret_cast<u64>(&on_stack);
    addresses.values[2] = reinterpret_cast<u64>(in_heap);
    addresses.values[3] = reinterpret_cast<u64>(mapped);
    addresses.values[4] = reinterpret_cast<u64>(&malloc);
    addresses.values[5] = reinterpret_cast<u64>(&environ);
    return addresses;
}

void test_layout_randomization(char const* self)
{
    static char const* const names[] = { "program", "stack", "heap", "mmap", "library", "library's data" };
    constexpr int runs = 6;
    u64 seen[runs][6];
    for (int run = 0; run < runs; ++run) {
        int fds[2];
        CHECK(pipe(fds) == 0);
        pid_t child = fork();
        if (child == 0) {
            dup2(fds[1], 1);
            close(fds[0]);
            close(fds[1]);
            execl(self, self, "--addresses", nullptr);
            _exit(127);
        }
        close(fds[1]);
        Addresses got {};
        size_t done = 0;
        while (done < sizeof(got)) {
            ssize_t piece = read(fds[0], reinterpret_cast<u8*>(&got) + done, sizeof(got) - done);
            if (piece <= 0)
                break;
            done += static_cast<size_t>(piece);
        }
        close(fds[0]);
        CHECK(done == sizeof(got));
        int status;
        waitpid(child, &status, 0);
        for (int i = 0; i < 6; ++i)
            seen[run][i] = got.values[i];
    }
    for (int i = 0; i < 6; ++i) {
        int distinct = 0;
        for (int run = 0; run < runs; ++run) {
            bool again = false;
            for (int earlier = 0; earlier < run; ++earlier) {
                if (seen[earlier][i] == seen[run][i])
                    again = true;
            }
            if (!again)
                ++distinct;
        }
        printf("hardening: %-16s %d of %d places differ, e.g. %#llx\n", names[i], distinct, runs, static_cast<unsigned long long>(seen[0][i]));
        // By chance two might meet; all of them would mean nothing moves at all.
        CHECK(distinct >= runs - 1);
    }

    // A fork is the same program, in the same place, by definition.
    int fds[2];
    CHECK(pipe(fds) == 0);
    u64 const here = reinterpret_cast<u64>(&test_layout_randomization);
    pid_t child = fork();
    if (child == 0) {
        u64 there = reinterpret_cast<u64>(&test_layout_randomization);
        (void)!write(fds[1], &there, sizeof(there));
        _exit(0);
    }
    u64 there = 0;
    (void)!read(fds[0], &there, sizeof(there));
    int status;
    waitpid(child, &status, 0);
    CHECK(here == there);
    close(fds[0]);
    close(fds[1]);
}

}

int main(int, char** argv)
{
    if (argv[1] && strcmp(argv[1], "--addresses") == 0) {
        auto addresses = addresses_of_this_run();
        (void)!write(1, &addresses, sizeof(addresses));
        return 0;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_write_xor_execute();
    test_stack_protector();
    test_control_flow();
    test_layout_randomization(argv[0]);
    return finish();
}
