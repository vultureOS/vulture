/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/DiskLog.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/FileSystem/VirtualFileSystem.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Tasks/Thread.h>

namespace Kernel::DiskLog {

namespace {

struct Target {
    RefPtr<BlockDevice> device;
    u64 first_block { 0 };
    u64 bytes { 0 };
};

Target s_boot;
Target s_panic;
// How much of the log is in the boot file so far.
u64 s_boot_written { 0 };
bool s_boot_finished { false };

// Allocated ahead of time: a panic can't count on the heap.
constexpr size_t chunk_size = 64 * 1024;
u8* s_chunk;

constexpr u64 flush_interval_microseconds = 1'000'000;

ErrorOr<Target> find(StringView path)
{
    auto resolved = TRY(VirtualFileSystem::resolve(path, "/"_sv, false));
    auto extent = TRY(resolved.inode->contiguous_extent());
    if (extent.device->is_read_only())
        return Error::from_errno(EROFS);
    Target target;
    target.device = *extent.device;
    target.first_block = extent.first_block;
    target.bytes = extent.bytes;
    return target;
}

// From `position` in the log to `position_in_file` in the file, up to `length` bytes, and whatever's left of the last
// block filled with spaces: what was there before should not show through.
ErrorOr<size_t> write_part(Target const& target, u64 position, u64 position_in_file, size_t length)
{
    constexpr size_t block = BlockDevice::block_size;
    VERIFY(position_in_file % block == 0);
    size_t const wanted = min(length, chunk_size);
    size_t const got = kernel_log().read_history(position, { s_chunk, wanted });
    size_t const blocks = (got + block - 1) / block;
    if (blocks == 0)
        return 0;
    __builtin_memset(s_chunk + got, ' ', blocks * block - got);
    TRY(target.device->write_blocks(target.first_block + position_in_file / block, blocks, { s_chunk, blocks * block }));
    return got;
}

void flush_boot_log()
{
    constexpr size_t block = BlockDevice::block_size;
    if (s_boot_finished)
        return;
    u64 const total = min(kernel_log().history_total(), s_boot.bytes);
    bool any = false;
    while (s_boot_written < total) {
        // From the start of the block it's in: part of that block was written last time.
        u64 const start = s_boot_written - s_boot_written % block;
        auto written = write_part(s_boot, start, start, total - start);
        if (written.is_error() || written.value() == 0) {
            // Either it won't take writes, or what's next has already gone from memory. Either way, it's as far as this goes.
            s_boot_finished = true;
            break;
        }
        s_boot_written = start + written.value();
        any = true;
    }
    if (s_boot_written >= s_boot.bytes)
        s_boot_finished = true;
    if (any)
        (void)s_boot.device->flush();
}

void keep_boot_log_up_to_date(void*)
{
    while (!s_boot_finished) {
        flush_boot_log();
        (void)Thread::sleep_for_microseconds(flush_interval_microseconds);
    }
}

}

void start()
{
    auto boot = find("/Volumes/EFI/Logs/boot.txt"_sv);
    auto panic_file = find("/Volumes/EFI/Logs/panic.txt"_sv);
    if (boot.is_error() || panic_file.is_error())
        return;
    s_chunk = static_cast<u8*>(kmalloc(chunk_size));
    if (!s_chunk)
        return;
    s_boot = boot.release_value();
    s_panic = panic_file.release_value();
    // (The thread only ever writes to the first of them.)
    auto thread = Thread::create("disk-log"_sv, keep_boot_log_up_to_date, nullptr, { .qos = QoS::Utility });
    if (thread.is_error()) {
        // Then it can at least keep what a panic says.
        s_boot.device = nullptr;
        return;
    }
    thread.value()->start();
    dmesgln("Log: keeping this boot's in Logs/boot.txt on the EFI System Partition");
}

void write_panic()
{
    if (!s_panic.device)
        return;
    constexpr size_t block = BlockDevice::block_size;
    // What's in the boot file so far first, in case that's all that works.
    if (s_boot.device) {
        // (The thread that does this is stopped. Where it had got to may be the middle of a write; going over it again is fine.)
        flush_boot_log();
    }

    u64 const total = kernel_log().history_total();
    u64 const kept = min<u64>(total, min<u64>(KernelLog::history_size, s_panic.bytes));
    u64 const start = total - kept;
    u64 in_file = 0;
    while (in_file < kept) {
        auto written = write_part(s_panic, start + in_file, in_file, kept - in_file);
        if (written.is_error() || written.value() == 0)
            break;
        // Whole blocks, unless it's the end of what there is.
        in_file += written.value();
        if (written.value() % block != 0)
            break;
    }
    // The rest of the file: what the last panic said beyond this one's length shouldn't look like part of it.
    __builtin_memset(s_chunk, ' ', chunk_size);
    u64 clear_from = (in_file + block - 1) / block * block;
    while (clear_from < s_panic.bytes) {
        size_t const blocks = min<u64>(chunk_size, s_panic.bytes - clear_from) / block;
        if (blocks == 0 || s_panic.device->write_blocks(s_panic.first_block + clear_from / block, blocks, { s_chunk, blocks * block }).is_error())
            break;
        clear_from += blocks * block;
    }
    (void)s_panic.device->flush();
    dmesgln("Log: this is in Logs/panic.txt on the EFI System Partition");
}

}
