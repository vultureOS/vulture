/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibBase/New.h>
#include <LibBase/Types.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

// The memory compressor, as a program sees it, which is not at all. A sweep (the sysctl) compresses this
// process's memory on demand; then everything a program or the kernel can do with a page has to go on
// working. On a small machine, this also uses half as much again as it has.

namespace {

constexpr size_t page_size = 4096;

u64 statistic(char const* name)
{
    u64 value = 0;
    size_t size = sizeof(value);
    CHECK(sysctlbyname(name, &value, &size, nullptr, 0) == 0);
    return value;
}

void sweep()
{
    int ignored = 0;
    size_t size = sizeof(ignored);
    int const one = 1;
    CHECK(sysctlbyname("vm.compressor.sweep", &ignored, &size, &one, sizeof(one)) == 0);
}

// Different for every page, and compressible: some of what's in there says which page it is.
void fill_page(u8* page, u64 seed)
{
    auto* words = reinterpret_cast<u64*>(page);
    for (size_t i = 0; i < page_size / sizeof(u64); ++i)
        words[i] = (i % 8 == 0) ? seed : (i % 8 == 1 ? ~seed : 0x0123'4567'89ab'cdefull + (i % 8));
}

[[nodiscard]] bool page_is_right(u8 const* page, u64 seed)
{
    auto const* words = reinterpret_cast<u64 const*>(page);
    for (size_t i = 0; i < page_size / sizeof(u64); ++i) {
        u64 const expected = (i % 8 == 0) ? seed : (i % 8 == 1 ? ~seed : 0x0123'4567'89ab'cdefull + (i % 8));
        if (words[i] != expected) {
            fprintf(stderr, "compressor: page %llu: word %zu is %llx, not %llx\n", static_cast<unsigned long long>(seed), i,
                static_cast<unsigned long long>(words[i]), static_cast<unsigned long long>(expected));
            return false;
        }
    }
    return true;
}

u8* map_pages(size_t count)
{
    void* memory = mmap(nullptr, count * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    return static_cast<u8*>(memory);
}

u8* fill_range(size_t count, u64 first_seed)
{
    auto* memory = map_pages(count);
    for (size_t i = 0; i < count; ++i)
        fill_page(memory + i * page_size, first_seed + i);
    return memory;
}

bool range_is_right(u8 const* memory, size_t count, u64 first_seed)
{
    for (size_t i = 0; i < count; ++i) {
        if (!page_is_right(memory + i * page_size, first_seed + i))
            return false;
    }
    return true;
}

void test_comes_back()
{
    constexpr size_t count = 256;
    auto* memory = fill_range(count, 1000);
    u64 const compressed_before = statistic("vm.compressor.compressions");
    u64 const decompressed_before = statistic("vm.compressor.decompressions");
    sweep();
    CHECK(statistic("vm.compressor.compressions") >= compressed_before + count);
    CHECK(statistic("vm.compressor.pages") >= count);
    // It really is smaller.
    CHECK(statistic("vm.compressor.pool_bytes") < statistic("vm.compressor.pages") * page_size);
    CHECK(statistic("vm.compressor.stored_bytes") <= statistic("vm.compressor.pool_bytes"));
    CHECK(range_is_right(memory, count, 1000));
    CHECK(statistic("vm.compressor.decompressions") >= decompressed_before + count);
    CHECK(munmap(memory, count * page_size) == 0);
}

void test_being_written_comes_back_too()
{
    constexpr size_t count = 32;
    auto* memory = fill_range(count, 2000);
    sweep();
    // A write as the first touch, of part of the page, keeps the rest.
    for (size_t i = 0; i < count; ++i)
        memory[i * page_size + page_size - 1] ^= 0xff;
    for (size_t i = 0; i < count; ++i)
        memory[i * page_size + page_size - 1] ^= 0xff;
    CHECK(range_is_right(memory, count, 2000));
    CHECK(munmap(memory, count * page_size) == 0);
}

void test_zeros_are_let_go()
{
    constexpr size_t count = 64;
    auto* memory = map_pages(count);
    for (size_t i = 0; i < count; ++i) {
        memory[i * page_size] = 1;
        memory[i * page_size] = 0;
    }
    u64 const zeros_before = statistic("vm.compressor.zero_pages");
    u64 const pages_before = statistic("vm.compressor.pages");
    sweep();
    CHECK(statistic("vm.compressor.zero_pages") >= zeros_before + count);
    // They take up nothing.
    CHECK(statistic("vm.compressor.pages") <= pages_before + count / 2);
    for (size_t i = 0; i < count * page_size; ++i) {
        if (memory[i] != 0) {
            CHECK(!"a zero page isn't");
            break;
        }
    }
    CHECK(munmap(memory, count * page_size) == 0);
}

void test_what_will_not_compress_stays()
{
    constexpr size_t count = 16;
    auto* memory = map_pages(count);
    u32 state = 0x7f4a7c15;
    for (size_t i = 0; i < count * page_size; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        memory[i] = static_cast<u8>(state >> 11);
    }
    u32 const first = *reinterpret_cast<u32 const*>(memory);
    u64 const rejected_before = statistic("vm.compressor.rejected");
    sweep();
    u64 const rejected = statistic("vm.compressor.rejected");
    CHECK(rejected >= rejected_before + count);
    // What hasn't been touched since hasn't changed, and isn't worth trying again. (It's the rest of
    // this program that may be new.)
    sweep();
    CHECK(statistic("vm.compressor.rejected") < rejected + count);
    CHECK(*reinterpret_cast<u32 const*>(memory) == first);
    // And now that it's something else, it's worth trying.
    for (size_t i = 0; i < count; ++i)
        fill_page(memory + i * page_size, 12000 + i);
    u64 const compressed_before = statistic("vm.compressor.compressions");
    sweep();
    CHECK(statistic("vm.compressor.compressions") >= compressed_before + count);
    CHECK(range_is_right(memory, count, 12000));
    CHECK(munmap(memory, count * page_size) == 0);
}

void test_fork_shares_it()
{
    constexpr size_t count = 128;
    auto* memory = fill_range(count, 3000);
    sweep();
    u64 const pages_held = statistic("vm.compressor.pages");
    // Forking copies no compressed pages. Each decompresses its own.
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (statistic("vm.compressor.pages") > pages_held + 8)
            _exit(2);
        if (!range_is_right(memory, count, 3000))
            _exit(3);
        // What this does to them is its own business.
        for (size_t i = 0; i < count; ++i)
            fill_page(memory + i * page_size, 9000 + i);
        sweep();
        if (!range_is_right(memory, count, 9000))
            _exit(4);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(range_is_right(memory, count, 3000));
    CHECK(munmap(memory, count * page_size) == 0);

    // And when the parent goes on to go first.
    memory = fill_range(count, 4000);
    int ready[2];
    int done[2];
    CHECK(pipe(ready) == 0 && pipe(done) == 0);
    child = fork();
    if (child == 0) {
        char c;
        (void)!read(ready[0], &c, 1);
        // Parent has compressed its own, shared with this: which it can't, until this one has gone
        // its own way. So this one's are all still there.
        _exit(range_is_right(memory, count, 4000) ? 0 : 1);
    }
    sweep();
    for (size_t i = 0; i < count; ++i)
        fill_page(memory + i * page_size, 5000 + i);
    sweep();
    CHECK(range_is_right(memory, count, 5000));
    CHECK(write(ready[1], "x", 1) == 1);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(ready[0]);
    close(ready[1]);
    close(done[0]);
    close(done[1]);
    CHECK(munmap(memory, count * page_size) == 0);
}

void test_the_kernel_reaches_it()
{
    constexpr size_t count = 8;
    auto* from = fill_range(count, 6000);
    auto* to = map_pages(count);
    int ends[2];
    CHECK(pipe(ends) == 0);
    sweep();
    // copy_from_user, faulting: a pipe holds a page or so at a time.
    for (size_t i = 0; i < count; ++i) {
        CHECK(write(ends[1], from + i * page_size, page_size) == static_cast<ssize_t>(page_size));
        // copy_to_user, into a page compressed since it was last touched.
        memset(to + i * page_size, 0x11, page_size);
        sweep();
        CHECK(read(ends[0], to + i * page_size, page_size) == static_cast<ssize_t>(page_size));
    }
    close(ends[0]);
    close(ends[1]);
    CHECK(range_is_right(to, count, 6000));
    CHECK(range_is_right(from, count, 6000));
    CHECK(munmap(from, count * page_size) == 0);
    CHECK(munmap(to, count * page_size) == 0);
}

void test_protection_still_counts()
{
    constexpr size_t count = 16;
    auto* memory = fill_range(count, 7000);
    sweep();
    // Read-only doesn't lose it, or make it writable.
    CHECK(mprotect(memory, count * page_size, PROT_READ) == 0);
    pid_t child = fork();
    if (child == 0) {
        signal(SIGSEGV, SIG_DFL);
        memory[0] = 1;
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(range_is_right(memory, count, 7000));
    // Nothing at all doesn't lose it either.
    CHECK(mprotect(memory, count * page_size, PROT_READ | PROT_WRITE) == 0);
    sweep();
    CHECK(mprotect(memory, count * page_size, PROT_NONE) == 0);
    child = fork();
    if (child == 0) {
        signal(SIGSEGV, SIG_DFL);
        volatile u8 c = memory[0];
        (void)c;
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    sweep();
    CHECK(mprotect(memory, count * page_size, PROT_READ | PROT_WRITE) == 0);
    CHECK(range_is_right(memory, count, 7000));
    CHECK(munmap(memory, count * page_size) == 0);
}

void test_unmapping_frees_it()
{
    constexpr size_t count = 512;
    auto* memory = fill_range(count, 8000);
    sweep();
    u64 const held = statistic("vm.compressor.pages");
    CHECK(held >= count);
    // The middle only, splitting the region: the rest is still there, either side.
    CHECK(munmap(memory + 100 * page_size, 100 * page_size) == 0);
    CHECK(statistic("vm.compressor.pages") <= held - 100);
    CHECK(range_is_right(memory, 100, 8000));
    CHECK(range_is_right(memory + 200 * page_size, count - 200, 8000 + 200));
    sweep();
    u64 const held_now = statistic("vm.compressor.pages");
    CHECK(munmap(memory, 100 * page_size) == 0);
    CHECK(munmap(memory + 200 * page_size, (count - 200) * page_size) == 0);
    CHECK(statistic("vm.compressor.pages") <= held_now - (count - 100));
}

// A futex belongs to the address, not to whichever page has it this minute.
void test_threads_keep_going()
{
    struct Shared {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
        int turn = 0;
    };
    auto* memory = map_pages(1);
    auto* shared = new (memory) Shared;
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, [](void* argument) -> void* {
        auto* shared = static_cast<Shared*>(argument);
        for (int i = 0; i < 20; ++i) {
            pthread_mutex_lock(&shared->mutex);
            while (shared->turn % 2 == 0)
                pthread_cond_wait(&shared->condition, &shared->mutex);
            ++shared->turn;
            pthread_cond_broadcast(&shared->condition);
            pthread_mutex_unlock(&shared->mutex);
        }
        return nullptr;
    }, shared) == 0);
    for (int i = 0; i < 20; ++i) {
        // (Compressing the very page they are waiting on, while they do.)
        sweep();
        pthread_mutex_lock(&shared->mutex);
        ++shared->turn;
        pthread_cond_broadcast(&shared->condition);
        while (shared->turn % 2 == 1)
            pthread_cond_wait(&shared->condition, &shared->mutex);
        pthread_mutex_unlock(&shared->mutex);
    }
    CHECK(pthread_join(thread, nullptr) == 0);
    CHECK(shared->turn == 40);
    CHECK(munmap(memory, page_size) == 0);
}

// What this is all for: using more memory than there is.
void test_more_than_there_is(u64 memory_size)
{
    size_t const total_pages = (memory_size / page_size) * 3 / 2;
    printf("compressor: %llu MiB of memory, using %llu MiB in three processes\n", static_cast<unsigned long long>(memory_size >> 20),
        static_cast<unsigned long long>((total_pages * page_size) >> 20));
    fflush(stdout);
    // Three at once: so each is made to wait by what the others do, and has its pages taken while
    // it's working.
    constexpr int workers = 3;
    pid_t children[workers];
    for (int w = 0; w < workers; ++w) {
        children[w] = fork();
        CHECK(children[w] >= 0);
        if (children[w] == 0) {
            size_t const count = total_pages / workers;
            u64 const first = static_cast<u64>(w + 1) << 40;
            auto* memory = map_pages(count);
            for (size_t i = 0; i < count; ++i)
                fill_page(memory + i * page_size, first + i);
            // In a different order, a few times, so that what is wanted isn't what was last compressed.
            for (int pass = 0; pass < 2; ++pass) {
                size_t const stride = pass == 0 ? 1 : 257;
                for (size_t i = 0, at = 0; i < count; ++i, at = (at + stride) % count) {
                    if (!page_is_right(memory + at * page_size, first + at))
                        _exit(1);
                }
            }
            // And it's writable: all of it again.
            for (size_t i = 0; i < count; ++i)
                fill_page(memory + i * page_size, first + count + i);
            if (!range_is_right(memory, count, first + count))
                _exit(2);
            _exit(0);
        }
    }
    for (int w = 0; w < workers; ++w) {
        int status = 0;
        CHECK(waitpid(children[w], &status, 0) == children[w]);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            fprintf(stderr, "compressor: worker %d: status %#x\n", w, status);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    printf("compressor: %llu pages compressed, %llu brought back\n", static_cast<unsigned long long>(statistic("vm.compressor.compressions")),
        static_cast<unsigned long long>(statistic("vm.compressor.decompressions")));
    fflush(stdout);
    CHECK(statistic("vm.compressor.compressions") > total_pages / 4);
    // It has all gone, with them.
    CHECK(statistic("vm.compressor.pages") < total_pages / 64);
}

// With nothing left that will compress: what's using the most is what has to go, not whoever asks for
// the next page, and not the system. This process is using a little and has pages to ask for too.
void test_running_out()
{
    constexpr size_t mine = 512;
    auto* memory = fill_range(mine, 11000);
    pid_t greedy = fork();
    CHECK(greedy >= 0);
    if (greedy == 0) {
        u32 state = 0x9e3779b9;
        // Far more than there is, none of it compressible, for as long as it's let.
        for (;;) {
            auto* more = map_pages(1024);
            auto* words = reinterpret_cast<u32*>(more);
            for (size_t i = 0; i < 1024 * page_size / sizeof(u32); ++i) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                words[i] = state;
            }
        }
    }
    int status = 0;
    // Going on using its own, while that happens, and being made to wait, but not more than that.
    for (;;) {
        pid_t const ended = waitpid(greedy, &status, WNOHANG);
        CHECK(ended >= 0);
        if (ended != 0)
            break;
        CHECK(range_is_right(memory, mine, 11000));
        for (size_t i = 0; i < mine; ++i)
            fill_page(memory + i * page_size, 11000 + i);
    }
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    printf("compressor: the one using the most ended, and the rest went on\n");
    CHECK(range_is_right(memory, mine, 11000));
    CHECK(munmap(memory, mine * page_size) == 0);
}

}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_comes_back();
    test_being_written_comes_back_too();
    test_zeros_are_let_go();
    test_what_will_not_compress_stays();
    test_fork_shares_it();
    test_the_kernel_reaches_it();
    test_protection_still_counts();
    test_unmapping_frees_it();
    test_threads_keep_going();
    u64 memory_size = 0;
    size_t size = sizeof(memory_size);
    CHECK(sysctlbyname("hw.memsize", &memory_size, &size, nullptr, 0) == 0);
    // (Not worth the time on a machine with this much: Meta/compressor-test.sh has a small one.)
    if (memory_size <= 2048ull * 1024 * 1024) {
        test_more_than_there_is(memory_size);
        test_running_out();
    }
    return finish();
}
