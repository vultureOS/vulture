/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <LibBase/Types.h>
#include <LibCrypto/SHA2.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
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

// A copy of a program on the system, changed in `change_at` if that isn't negative.
bool make_copy(char const* from, char const* to, off_t change_at)
{
    int in = open(from, O_RDONLY);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0)
        return false;
    static u8 buffer[64 * 1024];
    for (;;) {
        ssize_t got = read(in, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        if (write(out, buffer, static_cast<size_t>(got)) != got)
            return false;
    }
    if (change_at >= 0) {
        u8 byte = 0;
        CHECK(pread(in, &byte, 1, change_at) == 1);
        byte ^= 0xff;
        CHECK(pwrite(out, &byte, 1, change_at) == 1);
    }
    close(in);
    close(out);
    return true;
}

// What it exits with, or -errno if it wouldn't run at all.
int run_program(char const* path)
{
    int fds[2];
    CHECK(pipe2(fds, O_CLOEXEC) == 0);
    pid_t child = fork();
    if (child == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1);
        dup2(null, 2);
        char arg0[] = "true";
        char* arguments[] = { arg0, nullptr };
        execv(path, arguments);
        int const error = errno;
        (void)!write(fds[1], &error, sizeof(error));
        _exit(127);
    }
    close(fds[1]);
    int error = 0;
    ssize_t const got = read(fds[0], &error, sizeof(error));
    close(fds[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (got == sizeof(error))
        return -error;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

void test_code_signing()
{
    int enforced = 0;
    size_t size = sizeof(enforced);
    CHECK(sysctlbyname("security.code.enforced", &enforced, &size, nullptr, 0) == 0 && enforced == 1);

    // A faithful copy is the same program: its hash is what is vouched for.
    constexpr char const* copy = "/tmp/hardening-copy";
    CHECK(make_copy("/usr/bin/true", copy, -1));
    CHECK(run_program(copy) == 0);
    // One byte different isn't. (One far from anything that would stop it being a program.)
    CHECK(make_copy("/usr/bin/true", copy, 4000));
    CHECK(run_program(copy) == -EPERM);

    // Root vouching for it is what lets it run, and taking that back stops it.
    {
        u8 bytes[64 * 1024];
        int fd = open(copy, O_RDONLY);
        CHECK(fd >= 0);
        Crypto::SHA256 hasher;
        for (;;) {
            ssize_t got = read(fd, bytes, sizeof(bytes));
            if (got <= 0)
                break;
            hasher.update({ bytes, static_cast<size_t>(got) });
        }
        close(fd);
        auto const hash = hasher.digest();
        CHECK(sysctlbyname("security.code.trust", nullptr, nullptr, hash.data(), hash.size()) == 0);
        CHECK(run_program(copy) == 0);
        CHECK(sysctlbyname("security.code.distrust", nullptr, nullptr, hash.data(), hash.size()) == 0);
        CHECK(run_program(copy) == -EPERM);
    }

    // Nothing that isn't a file vouched for can be code at all.
    void* anonymous = mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(anonymous == MAP_FAILED && errno == EPERM);
    void* written = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(written != MAP_FAILED);
    CHECK(mprotect(written, 4096, PROT_READ) == 0);
    CHECK(mprotect(written, 4096, PROT_READ | PROT_EXEC) == -1 && errno == EPERM);
    munmap(written, 4096);

    int fd = open(copy, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EPERM);
    void* readable = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(readable != MAP_FAILED);
    CHECK(mprotect(readable, 4096, PROT_READ | PROT_EXEC) == -1 && errno == EPERM);
    munmap(readable, 4096);
    close(fd);
    unlink(copy);

    // A library that came with the system can be mapped as code; but not if it was ever writable.
    fd = open("/usr/lib/libSystem.B.dylib", O_RDONLY);
    CHECK(fd >= 0);
    void* library = mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    CHECK(library != MAP_FAILED);
    munmap(library, 4096);
    void* changed = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(changed != MAP_FAILED);
    *static_cast<u8*>(changed) ^= 1;
    CHECK(mprotect(changed, 4096, PROT_READ | PROT_EXEC) == -1 && errno == EPERM);
    munmap(changed, 4096);
    close(fd);
}

void test_seal()
{
    int active = 0;
    size_t size = sizeof(active);
    CHECK(sysctlbyname("security.seal.active", &active, &size, nullptr, 0) == 0 && active == 1);
    CHECK(geteuid() == 0);

    // Not even root.
    auto is_read_only = [](int result) { return result == -1 && errno == EROFS; };
    CHECK(is_read_only(open("/bin/ls", O_WRONLY)));
    CHECK(is_read_only(open("/bin/ls", O_RDWR)));
    CHECK(is_read_only(open("/bin/ls", O_RDONLY | O_TRUNC)));
    CHECK(is_read_only(open("/bin/new-file", O_WRONLY | O_CREAT, 0644)));
    CHECK(is_read_only(open("/System/Library/new-file", O_WRONLY | O_CREAT, 0644)));
    CHECK(is_read_only(open("/usr/lib/libSystem.B.dylib", O_WRONLY)));
    CHECK(is_read_only(open("/System/Library/Seal/Manifest", O_WRONLY)));
    CHECK(is_read_only(unlink("/bin/ls")));
    CHECK(is_read_only(rename("/bin/ls", "/bin/ls2")));
    CHECK(is_read_only(rename("/bin/ls", "/tmp/ls")) || errno == EXDEV);
    CHECK(is_read_only(rename("/System", "/System2")));
    CHECK(is_read_only(rename("/usr/local", "/usr/local2")));
    CHECK(is_read_only(rmdir("/usr/share")));
    CHECK(is_read_only(mkdir("/System/new-folder", 0755)));
    CHECK(is_read_only(symlink("/tmp", "/usr/bin/new-link")));
    CHECK(is_read_only(link("/bin/ls", "/usr/bin/ls2")));
    CHECK(is_read_only(link("/bin/ls", "/Users/hard-link-to-ls")));
    CHECK(is_read_only(chmod("/usr/bin/id", 04755)));
    CHECK(is_read_only(chown("/bin/ls", 501, 20)));
    CHECK(is_read_only(truncate("/bin/ls", 0)));
    timespec times[2] = { { 0, 0 }, { 0, 0 } };
    CHECK(is_read_only(utimensat(AT_FDCWD, "/bin/ls", times, 0)));
    CHECK(is_read_only(open("/etc/ssl/cert.pem", O_WRONLY)));
    // Nor by going to the disk underneath, whichever name it goes by.
    vulture_mount_info mounts[8];
    size = sizeof(mounts);
    CHECK(sysctlbyname("vfs.mounts", mounts, &size, nullptr, 0) == 0);
    char device[64];
    snprintf(device, sizeof(device), "/dev/%s", mounts[0].device);
    if (access(device, F_OK) == 0) {
        CHECK(open(device, O_WRONLY) == -1 && errno == EPERM);
        char* partition = strrchr(device, 's');
        if (partition && partition > device + 5) {
            *partition = 0;
            CHECK(open(device, O_RDWR) == -1 && errno == EPERM);
        }
    }

    // Reading still works, and the places left open are open.
    int fd = open("/bin/ls", O_RDONLY);
    CHECK(fd >= 0);
    close(fd);
    fd = open("/usr/local/hardening-test", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    close(fd);
    CHECK(unlink("/usr/local/hardening-test") == 0);
    CHECK(mkdir("/Applications/Hardening-Test.app", 0755) == 0);
    CHECK(rmdir("/Applications/Hardening-Test.app") == 0);
    // What the system came with in there isn't open, though.
    CHECK(is_read_only(rename("/Applications/Calculator.app", "/Applications/Calculator2.app")));
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
    test_code_signing();
    test_seal();
    test_layout_randomization(argv[0]);
    return finish();
}
