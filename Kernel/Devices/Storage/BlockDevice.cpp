/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/BlockDevice.h>
#include <Kernel/Memory/Heap.h>
#include <LibBase/New.h>

namespace Kernel {

static Vector<NonnullRefPtr<BlockDevice>> s_devices;
static u32 s_next_minor = 0;

BlockDevice::BlockDevice(String name, u64 block_count, bool read_only)
    : Device(InodeType::BlockDevice, 0600, (static_cast<u64>(8) << 8) | s_next_minor++)
    , m_name(move(name))
    , m_block_count(block_count)
    , m_read_only(read_only)
{
}

Vector<NonnullRefPtr<BlockDevice>>& BlockDevice::all()
{
    return s_devices;
}

ErrorOr<void> BlockDevice::register_device(NonnullRefPtr<BlockDevice> device)
{
    TRY(DevFS::the().add_device(device->name(), device));
    return s_devices.try_append(move(device));
}

InodeMetadata BlockDevice::metadata() const
{
    auto metadata = Device::metadata();
    metadata.size = m_block_count * block_size;
    metadata.block_size = block_size;
    return metadata;
}

// Byte-granular access for programs: whole blocks through a bounce buffer.
ErrorOr<size_t> BlockDevice::read(u64 offset, size_t count, IOBuffer const& buffer)
{
    u64 size = m_block_count * block_size;
    if (offset >= size)
        return 0;
    count = min<u64>(count, size - offset);
    constexpr size_t chunk_blocks = 64;
    auto* bounce = static_cast<u8*>(kmalloc(chunk_blocks * block_size));
    if (!bounce)
        return Error::from_errno(ENOMEM);
    size_t done = 0;
    ErrorOr<void> result {};
    while (done < count && !result.is_error()) {
        u64 position = offset + done;
        u64 first = position / block_size;
        size_t skip = position % block_size;
        size_t blocks = min<u64>(chunk_blocks, (skip + (count - done) + block_size - 1) / block_size);
        result = read_blocks(first, blocks, { bounce, blocks * block_size });
        if (result.is_error())
            break;
        size_t length = min<size_t>(blocks * block_size - skip, count - done);
        result = buffer.write(done, { bounce + skip, length });
        done += length;
    }
    kfree(bounce);
    TRY(result);
    return done;
}

ErrorOr<size_t> BlockDevice::write(u64 offset, size_t count, IOBuffer const& buffer)
{
    if (m_read_only)
        return Error::from_errno(EROFS);
    u64 size = m_block_count * block_size;
    if (offset >= size)
        return Error::from_errno(ENOSPC);
    count = min<u64>(count, size - offset);
    constexpr size_t chunk_blocks = 64;
    auto* bounce = static_cast<u8*>(kmalloc(chunk_blocks * block_size));
    if (!bounce)
        return Error::from_errno(ENOMEM);
    size_t done = 0;
    ErrorOr<void> result {};
    // Whole blocks go together; one that's only partly written is read first, to keep the rest of it.
    while (done < count && !result.is_error()) {
        u64 position = offset + done;
        u64 block = position / block_size;
        size_t skip = position % block_size;
        size_t length;
        size_t blocks;
        if (skip || count - done < block_size) {
            blocks = 1;
            length = min<size_t>(block_size - skip, count - done);
            result = read_blocks(block, 1, { bounce, block_size });
        } else {
            blocks = min<u64>(chunk_blocks, (count - done) / block_size);
            length = blocks * block_size;
        }
        if (!result.is_error())
            result = buffer.read(done, { bounce + skip, length });
        if (!result.is_error())
            result = write_blocks(block, blocks, { bounce, blocks * block_size });
        done += length;
    }
    kfree(bounce);
    TRY(result);
    return done;
}

PartitionDevice::PartitionDevice(NonnullRefPtr<BlockDevice> disk, String name, u64 first, u64 count, u8 const (&type)[16])
    : BlockDevice(move(name), count, disk->is_read_only())
    , m_disk(move(disk))
    , m_first(first)
{
    __builtin_memcpy(m_type, type, sizeof(m_type));
}

ErrorOr<NonnullRefPtr<PartitionDevice>> PartitionDevice::create(BlockDevice& disk, String name, u64 first, u64 count, u8 const (&type)[16])
{
    if (first + count > disk.block_count() || count == 0)
        return Error::from_errno(EINVAL);
    auto* partition = new (Base::nothrow) PartitionDevice(disk, move(name), first, count, type);
    if (!partition)
        return Error::from_errno(ENOMEM);
    return adopt_ref(*partition);
}

ErrorOr<void> PartitionDevice::read_blocks(u64 first, size_t count, Bytes buffer)
{
    if (first + count > block_count())
        return Error::from_errno(EIO);
    return m_disk->read_blocks(m_first + first, count, buffer);
}

ErrorOr<void> PartitionDevice::write_blocks(u64 first, size_t count, ReadonlyBytes buffer)
{
    if (first + count > block_count())
        return Error::from_errno(EIO);
    return m_disk->write_blocks(m_first + first, count, buffer);
}

namespace {

struct [[gnu::packed]] MBREntry {
    u8 status;
    u8 chs_first[3];
    u8 type;
    u8 chs_last[3];
    u32 first_lba;
    u32 sector_count;
};

struct [[gnu::packed]] GPTHeader {
    char signature[8];
    u32 revision;
    u32 header_size;
    u32 header_crc;
    u32 reserved;
    u64 current_lba;
    u64 backup_lba;
    u64 first_usable;
    u64 last_usable;
    u8 disk_guid[16];
    u64 entries_lba;
    u32 entry_count;
    u32 entry_size;
    u32 entries_crc;
};

struct [[gnu::packed]] GPTEntry {
    u8 type[16];
    u8 unique[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u16 name[36];
};

}

ErrorOr<size_t> scan_partitions(BlockDevice& disk)
{
    auto* sector = static_cast<u8*>(kmalloc(BlockDevice::block_size));
    if (!sector)
        return Error::from_errno(ENOMEM);
    auto free_sector = [&] { kfree(sector); };
    if (auto result = disk.read_blocks(0, 1, { sector, BlockDevice::block_size }); result.is_error()) {
        free_sector();
        return result.release_error();
    }
    size_t found = 0;
    auto add = [&](u64 first, u64 count, u8 const(&type)[16]) -> ErrorOr<void> {
        StringBuilder name;
        name.appendff("{}s{}", disk.name(), found + 1);
        auto partition = TRY(PartitionDevice::create(disk, TRY(name.to_string()), first, count, type));
        TRY(BlockDevice::register_device(partition));
        ++found;
        return {};
    };

    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        free_sector();
        return 0;
    }
    auto const* entries = reinterpret_cast<MBREntry const*>(sector + 446);
    bool protective = entries[0].type == 0xee;
    if (!protective) {
        for (size_t i = 0; i < 4; ++i) {
            if (entries[i].type == 0 || entries[i].sector_count == 0)
                continue;
            u8 type[16] {};
            type[15] = entries[i].type;
            if (auto result = add(entries[i].first_lba, entries[i].sector_count, type); result.is_error()) {
                free_sector();
                return result.release_error();
            }
        }
        free_sector();
        return found;
    }

    // GPT: the header in block 1, then the entry array.
    if (auto result = disk.read_blocks(1, 1, { sector, BlockDevice::block_size }); result.is_error()) {
        free_sector();
        return result.release_error();
    }
    auto header = *reinterpret_cast<GPTHeader const*>(sector);
    free_sector();
    if (__builtin_memcmp(header.signature, "EFI PART", 8) != 0 || header.entry_size < sizeof(GPTEntry) || header.entry_count > 1024)
        return Error::from_string_literal("bad GPT header");
    size_t bytes = align_up<size_t>(static_cast<size_t>(header.entry_count) * header.entry_size, BlockDevice::block_size);
    auto* table = static_cast<u8*>(kmalloc(bytes));
    if (!table)
        return Error::from_errno(ENOMEM);
    ErrorOr<void> result = disk.read_blocks(header.entries_lba, bytes / BlockDevice::block_size, { table, bytes });
    for (u32 i = 0; i < header.entry_count && !result.is_error(); ++i) {
        auto const& entry = *reinterpret_cast<GPTEntry const*>(table + static_cast<size_t>(i) * header.entry_size);
        bool used = false;
        for (u8 byte : entry.type)
            used = used || byte;
        if (used && entry.last_lba >= entry.first_lba)
            result = add(entry.first_lba, entry.last_lba - entry.first_lba + 1, entry.type);
    }
    kfree(table);
    TRY(result);
    return found;
}

}
