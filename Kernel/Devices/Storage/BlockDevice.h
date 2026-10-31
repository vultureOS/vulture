/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/DevFS.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace Kernel {

// A device of fixed-size blocks: a disk, or a partition of one. Appears in
// /dev ("disk0", "disk0s1"); programs may read and write it like a file.
class BlockDevice : public Device {
public:
    static constexpr size_t block_size = 512;

    [[nodiscard]] StringView name() const { return m_name.view(); }
    [[nodiscard]] u64 block_count() const { return m_block_count; }
    [[nodiscard]] bool is_read_only() const { return m_read_only; }

    // Into or from kernel memory.
    virtual ErrorOr<void> read_blocks(u64 first, size_t count, Bytes) = 0;
    virtual ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes) = 0;
    // Waits until everything written is on stable storage.
    virtual ErrorOr<void> flush() { return {}; }

    [[nodiscard]] InodeMetadata metadata() const override;
    ErrorOr<size_t> read(u64 offset, size_t count, IOBuffer const&) override;
    ErrorOr<size_t> write(u64 offset, size_t count, IOBuffer const&) override;

    static Vector<NonnullRefPtr<BlockDevice>>& all();
    // Adds it to /dev and to all().
    static ErrorOr<void> register_device(NonnullRefPtr<BlockDevice>);

protected:
    BlockDevice(String name, u64 block_count, bool read_only);

private:
    String m_name;
    u64 m_block_count;
    bool m_read_only;
};

// A slice of another block device, found in its partition table.
class PartitionDevice final : public BlockDevice {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<PartitionDevice>> create(BlockDevice& disk, String name, u64 first, u64 count, u8 const (&type)[16]);

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes) override;
    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes) override;
    ErrorOr<void> flush() override { return m_disk->flush(); }

    // The GPT partition type (MBR types become 00..00tt).
    [[nodiscard]] u8 const* type() const { return m_type; }

private:
    PartitionDevice(NonnullRefPtr<BlockDevice> disk, String name, u64 first, u64 count, u8 const (&type)[16]);

    NonnullRefPtr<BlockDevice> m_disk;
    u64 m_first;
    u8 m_type[16];
};

// Reads the disk's MBR or GPT and registers its partitions.
ErrorOr<size_t> scan_partitions(BlockDevice&);

// Finds the storage controllers on the PCI bus and registers their disks.
void initialize_storage();

}
