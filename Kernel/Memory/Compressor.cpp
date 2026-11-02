/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/signal_numbers.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/SpinLock.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/AddressSpace.h>
#include <Kernel/Memory/Compressor.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/Process.h>
#include <Kernel/Tasks/Thread.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Atomic.h>
#include <LibBase/Vector.h>
#include <LibCompress/PageCompression.h>

namespace Kernel::Compressor {

namespace {

static_assert(page_size == Compress::compressible_page_size);

// ---- The pool: pages cut into equal pieces, a size of piece to a page. A page holds some whole
// number of pieces, so nothing is lost at the end of it; what is lost is the difference between a
// compressed page and the next size up.

struct PoolPage {
    PoolPage* previous;
    PoolPage* next;
    u16 first_free; // An offset into this page; 0: none.
    u16 used;
    u16 pieces;
    u16 size_class;
    u64 unused;
};
static_assert(sizeof(PoolPage) == 32);

struct Piece {
    Atomic<u32> references;
    u16 length;
    u16 next_free; // On the page's free list.
};
static_assert(sizeof(Piece) == 8);

constexpr size_t pool_capacity = page_size - sizeof(PoolPage);
constexpr size_t pieces_per_page[] = { 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 32, 48 };
constexpr size_t size_class_count = sizeof(pieces_per_page) / sizeof(pieces_per_page[0]);

constexpr size_t piece_size(size_t size_class)
{
    return (pool_capacity / pieces_per_page[size_class]) & ~static_cast<size_t>(7);
}

// What's worth keeping: anything bigger wouldn't get a page down to half.
constexpr size_t largest_stored = piece_size(0) - sizeof(Piece);

SpinLock s_pool_lock;
PoolPage* s_partial[size_class_count];
Atomic<u64> s_pool_pages { 0 };
Atomic<u64> s_stored_pieces { 0 };
Atomic<u64> s_stored_bytes { 0 };
Atomic<u64> s_compressions { 0 };
Atomic<u64> s_decompressions { 0 };
Atomic<u64> s_zero_pages { 0 };
Atomic<u64> s_rejected { 0 };

size_t class_for(size_t bytes)
{
    size_t const needed = bytes + sizeof(Piece);
    for (size_t i = size_class_count; i > 0; --i) {
        if (piece_size(i - 1) >= needed)
            return i - 1;
    }
    VERIFY_NOT_REACHED();
}

void unlink_page(PoolPage& page)
{
    if (page.previous)
        page.previous->next = page.next;
    else
        s_partial[page.size_class] = page.next;
    if (page.next)
        page.next->previous = page.previous;
    page.previous = page.next = nullptr;
}

void link_page(PoolPage& page)
{
    page.previous = nullptr;
    page.next = s_partial[page.size_class];
    if (page.next)
        page.next->previous = &page;
    s_partial[page.size_class] = &page;
}

// A new page, with all of it on its free list, and on the list of those with room.
PoolPage* grow_pool(size_t size_class)
{
    auto block = physical_memory().allocate_page(PageUse::General);
    if (block.is_error())
        return nullptr;
    auto* page = reinterpret_cast<PoolPage*>(physical_to_virtual(block.value()));
    size_t const size = piece_size(size_class);
    size_t const pieces = pieces_per_page[size_class];
    page->previous = page->next = nullptr;
    page->used = 0;
    page->pieces = static_cast<u16>(pieces);
    page->size_class = static_cast<u16>(size_class);
    page->unused = 0;
    page->first_free = sizeof(PoolPage);
    auto* base = reinterpret_cast<u8*>(page);
    for (size_t i = 0; i < pieces; ++i) {
        size_t const offset = sizeof(PoolPage) + i * size;
        reinterpret_cast<Piece*>(base + offset)->next_free = i + 1 < pieces ? static_cast<u16>(offset + size) : 0;
    }
    s_pool_pages.fetch_add(1);
    return page;
}

Piece* allocate_piece(size_t bytes)
{
    size_t const size_class = class_for(bytes);
    SpinLockGuard guard(s_pool_lock);
    auto* page = s_partial[size_class];
    if (!page) {
        page = grow_pool(size_class);
        if (!page)
            return nullptr;
        link_page(*page);
    }
    auto* piece = reinterpret_cast<Piece*>(reinterpret_cast<u8*>(page) + page->first_free);
    page->first_free = piece->next_free;
    ++page->used;
    if (page->first_free == 0)
        unlink_page(*page);
    return piece;
}

void free_piece(Piece& piece)
{
    PoolPage* empty = nullptr;
    {
        SpinLockGuard guard(s_pool_lock);
        auto* page = reinterpret_cast<PoolPage*>(align_down<FlatPtr>(reinterpret_cast<FlatPtr>(&piece), page_size));
        bool const was_full = page->first_free == 0;
        piece.next_free = page->first_free;
        page->first_free = static_cast<u16>(reinterpret_cast<u8*>(&piece) - reinterpret_cast<u8*>(page));
        --page->used;
        if (page->used == 0) {
            if (!was_full)
                unlink_page(*page);
            empty = page;
        } else if (was_full) {
            link_page(*page);
        }
    }
    if (empty) {
        physical_memory().free_pages(virtual_to_physical(reinterpret_cast<u8*>(empty)));
        s_pool_pages.fetch_sub(1);
    }
}

// ---- When to do something, and doing it.

// How much of a process to go over in one turn, before moving on to the next one.
constexpr size_t pages_per_turn = 128;
// Rounds in a row where nothing could be done, before saying that nothing can be.
constexpr unsigned idle_rounds_before_giving_up = 32;
// And where nothing got freed, even if pages were being looked at: a program that's busy has its pages
// go through their second chance over and over, which isn't anyone getting any memory. This is
// enough to go round all of memory twice, as a round is 1/64 of it, however that is shared out.
constexpr unsigned rounds_without_freeing_before_giving_up = 128;

Atomic<bool> s_running { false };
Atomic<bool> s_low { false };
Atomic<bool> s_demand { false };
Atomic<bool> s_exhausted { false };
Atomic<u64> s_rounds { 0 };
WaitQueue s_wake;
WaitQueue s_round_finished;
size_t s_pages_per_round { 0 };

Mutex s_workspace_lock;
Compress::PageCompressionWorkspace s_workspace;
u8 s_output[largest_stored];

struct RoundResult {
    size_t freed { 0 };
    size_t aged { 0 };
};

RoundResult run_round()
{
    RoundResult result;
    auto processes = Process::all();
    for (auto& process : processes) {
        auto space = process->address_space_reference();
        if (!space)
            continue;
        size_t remaining = s_pages_per_round;
        while (remaining > 0) {
            auto const turn = space->reclaim(min(remaining, pages_per_turn));
            result.freed += turn.freed;
            result.aged += turn.aged;
            // (A process with nothing in it, or that is busy, can wait until next time.)
            if (turn.examined == 0)
                break;
            remaining -= min(remaining, turn.examined);
            if (physical_memory().has_plenty_free() && !s_demand.load())
                return result;
        }
    }
    // What was freed here may be sitting in this processor's cache, where nobody counts it.
    physical_memory().drain_processor_cache();
    return result;
}

void finish_round()
{
    s_rounds.fetch_add(1);
    s_round_finished.wake_all();
}

void pause(u64 milliseconds)
{
    WaitQueue queue;
    (void)queue.wait_until_interruptible([] { return false; }, Time::now() + Time::ticks_from_milliseconds(milliseconds));
}

// The last resort, with nothing left to compress and programs waiting. Whoever is using the most,
// of the people's own programs if there are any, as the session's own services are mostly root's; and
// never init, as the system doesn't go on without it.
RefPtr<Process> end_the_biggest_process()
{
    RefPtr<Process> biggest;
    size_t most = 0;
    bool biggest_is_root = true;
    for (auto& process : Process::all()) {
        if (process->pid() == 1 || process->is_zombie())
            continue;
        auto space = process->address_space_reference();
        if (!space)
            continue;
        size_t const pages = space->footprint_pages();
        bool const is_root = process->credentials().uid == 0;
        if (pages == 0 || (is_root && !biggest_is_root))
            continue;
        if ((biggest_is_root && !is_root) || pages > most) {
            biggest = process;
            most = pages;
            biggest_is_root = is_root;
        }
    }
    if (!biggest)
        return nullptr;
    dmesgln("Compressor: out of memory, with nothing left to compress: ending {} ({}), which is using {} MiB", biggest->pid(), biggest->name(), most * page_size / MiB);
    vulture_siginfo info {};
    info.si_code = SI_KERNEL;
    biggest->send_signal(SIGKILL, info);
    return biggest;
}

void reclaim_thread(void*)
{
    unsigned idle_rounds = 0;
    unsigned rounds_without_freeing = 0;
    bool backing_off = false;
    for (;;) {
        // Now and then regardless: something else could have run out without telling anyone. And after
        // getting nowhere, there's no use going straight back unless someone is waiting.
        (void)s_wake.wait_until_interruptible([&backing_off] { return s_demand.load() || (s_low.load() && !backing_off); },
            Time::now() + Time::ticks_from_milliseconds(1000));
        backing_off = false;
        s_low.store(false);
        if (!physical_memory().is_nearly_out())
            s_exhausted.store(false);
        if (!physical_memory().is_low() && !s_demand.load())
            continue;

        while (!physical_memory().has_plenty_free() || s_demand.exchange(false)) {
            auto const result = run_round();
            if (result.freed > 0)
                rounds_without_freeing = 0;
            else
                ++rounds_without_freeing;
            if (result.freed > 0 || result.aged > 0)
                idle_rounds = 0;
            else
                ++idle_rounds;
            if (idle_rounds < idle_rounds_before_giving_up && rounds_without_freeing < rounds_without_freeing_before_giving_up) {
                s_exhausted.store(false);
                finish_round();
                continue;
            }
            idle_rounds = 0;
            rounds_without_freeing = 0;
            // All that could be compressed, has been. Is anyone in trouble because of it?
            if (physical_memory().is_nearly_out()) {
                if (auto ending = end_the_biggest_process()) {
                    // Those waiting go on trying, which is also how it finds out that it is ending,
                    // if it's one of them. Its memory goes when it does.
                    for (int waited = 0; waited < 100 && !ending->is_zombie(); ++waited) {
                        finish_round();
                        pause(50);
                    }
                    finish_round();
                    continue;
                }
                s_exhausted.store(true);
            }
            backing_off = true;
            finish_round();
            break;
        }
    }
}

}

void initialize()
{
    auto const statistics = physical_memory().statistics();
    physical_memory().set_pressure_levels(statistics.managed_pages);
    s_pages_per_round = max<size_t>(statistics.managed_pages / 64, pages_per_turn);
    auto thread = Thread::create("compressor"_sv, reclaim_thread, nullptr, { .qos = QoS::UserInitiated });
    if (thread.is_error()) {
        dmesgln("Compressor: no thread: {}", thread.error());
        return;
    }
    s_running.store(true);
    thread.value()->start();
}

Handle store(u8 const* page)
{
    MutexLocker locker(s_workspace_lock);
    size_t const size = Compress::compress_page(page, { s_output, sizeof(s_output) }, s_workspace);
    if (size == 0) {
        s_rejected.fetch_add(1);
        return nullptr;
    }
    auto* piece = allocate_piece(size);
    if (!piece) {
        s_rejected.fetch_add(1);
        return nullptr;
    }
    piece->references.store(1);
    piece->length = static_cast<u16>(size);
    __builtin_memcpy(piece + 1, s_output, size);
    s_stored_pieces.fetch_add(1);
    s_stored_bytes.fetch_add(size);
    s_compressions.fetch_add(1);
    return piece;
}

void load(Handle handle, u8* page)
{
    auto const& piece = *static_cast<Piece const*>(handle);
    bool const whole = Compress::decompress_page({ reinterpret_cast<u8 const*>(&piece + 1), piece.length }, page);
    // This was written by the code above, into kernel memory. If that isn't what comes back, then
    // a program's memory has been damaged, and there's no carrying on from that.
    VERIFY(whole);
    s_decompressions.fetch_add(1);
}

void ref(Handle handle)
{
    u32 const previous = static_cast<Piece*>(handle)->references.fetch_add(1);
    VERIFY(previous > 0);
}

void unref(Handle handle)
{
    auto& piece = *static_cast<Piece*>(handle);
    u32 const previous = piece.references.fetch_sub(1);
    VERIFY(previous > 0);
    if (previous > 1)
        return;
    s_stored_pieces.fetch_sub(1);
    s_stored_bytes.fetch_sub(piece.length);
    free_piece(piece);
}

void note_zero_page()
{
    s_zero_pages.fetch_add(1);
}

Statistics statistics()
{
    return {
        .pages = s_stored_pieces.load(),
        .pool_bytes = s_pool_pages.load() * page_size,
        .stored_bytes = s_stored_bytes.load(),
        .compressions = s_compressions.load(),
        .decompressions = s_decompressions.load(),
        .zero_pages = s_zero_pages.load(),
        .rejected = s_rejected.load(),
    };
}

void memory_is_low()
{
    if (!s_running.load() || s_low.exchange(true))
        return;
    s_wake.wake_all();
}

bool wait_for_memory()
{
    if (!s_running.load() || s_exhausted.load())
        return false;
    u64 const seen = s_rounds.load();
    s_demand.store(true);
    s_wake.wake_all();
    s_round_finished.wait_until([&seen] { return s_rounds.load() != seen; });
    return !s_exhausted.load();
}

}
