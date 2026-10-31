/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// Runs `body` in a child and reports the signal that killed it (0 if none).
template<typename Body>
static int signal_of_child(Body body)
{
    pid_t pid = fork();
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

[[gnu::noinline]] static unsigned long recurse(unsigned depth)
{
    // About 1 KiB of stack per level, grown into page by page.
    char volatile frame[1000];
    frame[0] = static_cast<char>(depth);
    frame[999] = static_cast<char>(depth);
    if (depth == 0)
        return frame[0];
    return recurse(depth - 1) + frame[999];
}

int main()
{
    // A large mapping costs nothing until touched (demand paging).
    constexpr size_t size = 64 * 1024 * 1024;
    auto* region = static_cast<unsigned char*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(region != MAP_FAILED);
    if (region != MAP_FAILED) {
        CHECK(region[0] == 0 && region[size - 1] == 0);
        for (size_t offset = 0; offset < size; offset += 64 * 1024)
            region[offset] = static_cast<unsigned char>(offset >> 16);
        bool intact = true;
        for (size_t offset = 0; offset < size; offset += 64 * 1024)
            intact = intact && region[offset] == static_cast<unsigned char>(offset >> 16);
        CHECK(intact);
        CHECK(munmap(region, size) == 0);
        // Gone: touching it now is a crash.
        CHECK(signal_of_child([&] { region[0] = 1; }) == SIGSEGV);
    }

    // Read-only memory cannot be written; mprotect changes that.
    auto* page = static_cast<int*>(mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(page != MAP_FAILED);
    CHECK(page[0] == 0);
    CHECK(signal_of_child([&] { page[0] = 1; }) == SIGSEGV);
    CHECK(mprotect(page, 4096, PROT_READ | PROT_WRITE) == 0);
    page[0] = 5;
    CHECK(page[0] == 5);
    CHECK(munmap(page, 4096) == 0);

    // Code is not writable, and data is not executable.
    CHECK(signal_of_child([] { *reinterpret_cast<unsigned volatile*>(&main) = 0; }) == SIGSEGV);
    static unsigned s_data_word = 0xd65f03c0; // "ret"
    CHECK(signal_of_child([] { reinterpret_cast<void (*)()>(&s_data_word)(); }) == SIGSEGV);

    // The stack grows on demand, well past one page.
    CHECK(recurse(1000) != 1);

    // Bad arguments are refused.
    CHECK(mmap(nullptr, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED);
    CHECK(munmap(reinterpret_cast<void*>(0x1001), 4096) == -1);
    return finish();
}
