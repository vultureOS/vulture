/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/errno.h>
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
    [[nodiscard]] StringView block_device_name() const override { return m_name.view(); }
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

    // This, if it is a partition.
    [[nodiscard]] virtual class PartitionDevice* as_partition() { return nullptr; }

    // What it's on, for what sort of thing to call it.
    [[nodiscard]] virtual StringView bus_name() const { return "internal"_sv; }
    [[nodiscard]] virtual bool is_removable() const { return false; }
    // Asks it to come out; it's gone when this is done, if it could.
    virtual ErrorOr<void> eject() { return Error::from_errno(ENOTTY); }
    // It's come out, whether it was asked or not. Reading and writing it say so from now on.
    [[nodiscard]] bool has_gone() const { return m_gone; }

    [[nodiscard]] ErrorOr<FlatPtr> ioctl(FileDescription&, unsigned request, FlatPtr argument) override;

    // As they are now: which may be different by the time anyone looks.
    [[nodiscard]] static Vector<NonnullRefPtr<BlockDevice>> all();
    // Adds it to /dev and to all().
    static ErrorOr<void> register_device(NonnullRefPtr<BlockDevice>);
    // Takes away a disk, and whatever it's divided into, from both. Whatever has them mounted or open finds out by
    // what it reads and writes.
    static void remove_disk(BlockDevice&);

protected:
    BlockDevice(String name, u64 block_count, bool read_only);
    // For drivers that learn the size after construction.
    void set_block_count(u64 count) { m_block_count = count; }

private:
    String m_name;
    u64 m_block_count;
    bool m_read_only;
    bool m_gone { false };
};

// A slice of another block device, found in its partition table.
class PartitionDevice final : public BlockDevice {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<PartitionDevice>> create(BlockDevice& disk, String name, u64 first, u64 count, u8 const (&type)[16], u8 const* unique = nullptr);

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes) override;
    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes) override;
    ErrorOr<void> flush() override { return m_disk->flush(); }
    [[nodiscard]] PartitionDevice* as_partition() override { return this; }
    [[nodiscard]] BlockDevice& disk() { return *m_disk; }
    [[nodiscard]] StringView bus_name() const override { return m_disk->bus_name(); }
    [[nodiscard]] bool is_removable() const override { return m_disk->is_removable(); }
    ErrorOr<void> eject() override { return m_disk->eject(); }

    // The GPT partition type (MBR types become 00..00tt).
    [[nodiscard]] u8 const* type() const { return m_type; }
    // Its own GUID, on a disk that has them.
    [[nodiscard]] u8 const* unique() const { return m_unique; }

private:
    PartitionDevice(NonnullRefPtr<BlockDevice> disk, String name, u64 first, u64 count, u8 const (&type)[16]);

    NonnullRefPtr<BlockDevice> m_disk;
    u64 m_first;
    u8 m_type[16];
    u8 m_unique[16] {};
};

// Memory, as a disk that can only be read: the system image the loader brought in.
[[nodiscard]] ErrorOr<NonnullRefPtr<BlockDevice>> create_memory_block_device(String name, ReadonlyBytes);

// Reads the disk's MBR or GPT and registers its partitions.
ErrorOr<size_t> scan_partitions(BlockDevice&);

// Finds the storage controllers on the PCI bus and registers their disks.
void initialize_storage();

}
