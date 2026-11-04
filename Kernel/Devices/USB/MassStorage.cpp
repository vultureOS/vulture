/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Devices/USB/USB.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <LibBase/New.h>
#include <LibBase/String.h>

// USB sticks and disks: SCSI commands, each in a little wrapper on one endpoint and its answer on the
// other, with the data between them ("bulk only" transport). They have a size of block of their own to
// report, and the rest of this system only has the one: those that differ aren't used.

namespace Kernel::USB {

namespace {

constexpr u8 subclass_scsi = 0x06;
constexpr u8 protocol_bulk_only = 0x50;

constexpr u32 wrapper_signature = 0x43425355;
constexpr u32 status_signature = 0x53425355;

constexpr u8 scsi_test_unit_ready = 0x00;
constexpr u8 scsi_request_sense = 0x03;
constexpr u8 scsi_read_capacity = 0x25;
constexpr u8 scsi_read = 0x28;
constexpr u8 scsi_write = 0x2a;
constexpr u8 scsi_synchronize = 0x35;
constexpr u8 scsi_start_stop = 0x1b;

struct [[gnu::packed]] CommandWrapper {
    u32 signature;
    u32 tag;
    u32 data_length;
    u8 flags; // Bit 7: data coming in.
    u8 unit;
    u8 command_length;
    u8 command[16];
};
static_assert(sizeof(CommandWrapper) == 31);

struct [[gnu::packed]] StatusWrapper {
    u32 signature;
    u32 tag;
    u32 residue;
    u8 status;
};
static_assert(sizeof(StatusWrapper) == 13);

// Within a 64 KiB boundary, which the allocator's blocks of that size are naturally.
constexpr unsigned buffer_order = 4;
constexpr size_t buffer_size = page_size << buffer_order;
constexpr size_t blocks_per_command = buffer_size / BlockDevice::block_size;

class Disk final : public BlockDevice {
public:
    Disk(String name, USB::Device& device, Endpoint& in, Endpoint& out, PhysicalAddress buffer, PhysicalAddress small)
        : BlockDevice(move(name), 0, false)
        , m_device(device)
        , m_in(in)
        , m_out(out)
        , m_buffer(buffer)
        , m_small(small)
    {
    }

    [[nodiscard]] StringView bus_name() const override { return "USB"_sv; }
    [[nodiscard]] bool is_removable() const override { return true; }

    ErrorOr<void> eject() override
    {
        {
            MutexLocker locker(m_lock);
            u8 const synchronize[10] = { scsi_synchronize };
            (void)run_command(synchronize, sizeof(synchronize), 0, false);
            // Load/eject: and not to start again.
            u8 const stop[6] = { scsi_start_stop, 0, 0, 0, 0x02, 0 };
            (void)run_command(stop, sizeof(stop), 0, false);
        }
        BlockDevice::remove_disk(*this);
        return {};
    }

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes destination) override
    {
        if (m_device.has_gone())
            return Error::from_errno(ENXIO);
        MutexLocker locker(m_lock);
        finish_what_a_panic_interrupted();
        size_t done = 0;
        while (done < count) {
            size_t const now = min(count - done, blocks_per_command);
            TRY(transfer_blocks(scsi_read, first + done, now, true));
            __builtin_memcpy(destination.data() + done * block_size, physical_to_virtual(m_buffer), now * block_size);
            done += now;
        }
        return {};
    }

    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes source) override
    {
        if (m_device.has_gone())
            return Error::from_errno(ENXIO);
        MutexLocker locker(m_lock);
        finish_what_a_panic_interrupted();
        size_t done = 0;
        while (done < count) {
            size_t const now = min(count - done, blocks_per_command);
            __builtin_memcpy(physical_to_virtual(m_buffer), source.data() + done * block_size, now * block_size);
            TRY(transfer_blocks(scsi_write, first + done, now, false));
            done += now;
        }
        return {};
    }

    ErrorOr<void> flush() override
    {
        if (m_device.has_gone())
            return Error::from_errno(ENXIO);
        MutexLocker locker(m_lock);
        finish_what_a_panic_interrupted();
        u8 const command[10] = { scsi_synchronize };
        // (Not all of them have one, and not having one isn't something to fail over.)
        (void)run_command(command, sizeof(command), 0, false);
        return {};
    }

    // Until it's ready, as some take a moment; then how big it is. False if it can't be used.
    ErrorOr<bool> read_size()
    {
        for (int attempt = 0; attempt < 20; ++attempt) {
            u8 const ready[6] = { scsi_test_unit_ready };
            if (!run_command(ready, sizeof(ready), 0, false).is_error())
                break;
            // Reading what's wrong is what clears it.
            u8 const sense[6] = { scsi_request_sense, 0, 0, 0, 18, 0 };
            (void)run_command(sense, sizeof(sense), 18, true);
        }
        u8 const capacity[10] = { scsi_read_capacity };
        TRY(run_command(capacity, sizeof(capacity), 8, true));
        auto const* answer = physical_to_virtual(m_buffer);
        u32 const last = (static_cast<u32>(answer[0]) << 24) | (static_cast<u32>(answer[1]) << 16) | (static_cast<u32>(answer[2]) << 8) | answer[3];
        u32 const size = (static_cast<u32>(answer[4]) << 24) | (static_cast<u32>(answer[5]) << 16) | (static_cast<u32>(answer[6]) << 8) | answer[7];
        if (size != block_size) {
            dmesgln("USB: a disk with {}-byte blocks isn't something this can use", size);
            return false;
        }
        set_block_count(static_cast<u64>(last) + 1);
        return true;
    }

private:
    ErrorOr<void> transfer_blocks(u8 operation, u64 first, size_t count, bool in)
    {
        u8 const command[10] = {
            operation, 0,
            static_cast<u8>(first >> 24), static_cast<u8>(first >> 16), static_cast<u8>(first >> 8), static_cast<u8>(first),
            0,
            static_cast<u8>(count >> 8), static_cast<u8>(count),
            0
        };
        return run_command(command, sizeof(command), count * block_size, in);
    }

    // A command is three steps, and the device won't hear of a new one until the last of them is done. The thread that
    // was in the middle of one was stopped by a panic, and what's in the panic log depends on it being finished for it.
    void finish_what_a_panic_interrupted()
    {
        if (!is_panicking() || m_step == Step::Idle)
            return;
        if (m_step == Step::WrapperSent && m_data_length > 0)
            (void)m_device.bulk(m_data_in ? m_in : m_out, m_buffer, m_data_length);
        (void)m_device.bulk(m_in, m_small.offset(64), sizeof(StatusWrapper));
        m_step = Step::Idle;
    }

    ErrorOr<void> run_command(u8 const* command, size_t command_length, size_t data_length, bool in)
    {
        auto* wrapper = reinterpret_cast<CommandWrapper*>(physical_to_virtual(m_small));
        __builtin_memset(wrapper, 0, sizeof(*wrapper));
        wrapper->signature = wrapper_signature;
        wrapper->tag = ++m_tag;
        wrapper->data_length = static_cast<u32>(data_length);
        wrapper->flags = in ? 0x80 : 0;
        wrapper->command_length = static_cast<u8>(command_length);
        __builtin_memcpy(wrapper->command, command, command_length);
        m_data_length = data_length;
        m_data_in = in;
        m_step = Step::WrapperSent;
        TRY(m_device.bulk(m_out, m_small, sizeof(*wrapper)));

        if (data_length > 0) {
            auto const moved = m_device.bulk(in ? m_in : m_out, m_buffer, data_length);
            if (moved.is_error()) {
                if (moved.error().code() != EPIPE)
                    return moved.error();
                TRY(m_device.clear_stall(in ? m_in : m_out));
            }
        }
        m_step = Step::DataMoved;

        PhysicalAddress const status_address = m_small.offset(64);
        auto const answered = m_device.bulk(m_in, status_address, sizeof(StatusWrapper));
        if (answered.is_error()) {
            if (answered.error().code() != EPIPE)
                return answered.error();
            TRY(m_device.clear_stall(m_in));
            TRY(m_device.bulk(m_in, status_address, sizeof(StatusWrapper)));
        }
        auto const* status = reinterpret_cast<StatusWrapper const*>(physical_to_virtual(status_address));
        if (status->signature != status_signature || status->tag != m_tag)
            return Error::from_errno(EIO);
        m_step = Step::Idle;
        if (status->status != 0)
            return Error::from_errno(EIO);
        return {};
    }

    USB::Device& m_device;
    Endpoint& m_in;
    Endpoint& m_out;
    PhysicalAddress m_buffer;
    PhysicalAddress m_small;
    Mutex m_lock;
    u32 m_tag { 0 };
    enum class Step : u8 {
        Idle,
        WrapperSent,
        DataMoved,
    };
    Step m_step { Step::Idle };
    size_t m_data_length { 0 };
    bool m_data_in { false };
};

}

ErrorOr<bool> attach_mass_storage(Device& device, InterfaceDescription const& interface)
{
    if (interface.subclass != subclass_scsi || interface.protocol != protocol_bulk_only)
        return false;
    Optional<EndpointDescription> in;
    Optional<EndpointDescription> out;
    for (u8 i = 0; i < interface.endpoint_count; ++i) {
        auto const& endpoint = interface.endpoints[i];
        if (endpoint.kind != EndpointKind::Bulk)
            continue;
        if (endpoint.is_in())
            in = endpoint;
        else
            out = endpoint;
    }
    if (!in.has_value() || !out.has_value())
        return false;
    auto* in_endpoint = TRY(device.open_endpoint(in.value()));
    auto* out_endpoint = TRY(device.open_endpoint(out.value()));
    auto const buffer = TRY(physical_memory().allocate_pages(buffer_order, PageUse::General));
    auto const small = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    StringBuilder name;
    name.appendff("disk{}", take_disk_number());
    auto* disk = new (Base::nothrow) Disk(MUST(name.to_string()), device, *in_endpoint, *out_endpoint, buffer, small);
    if (!disk)
        return Error::from_errno(ENOMEM);
    auto reference = adopt_ref(*disk);
    if (!TRY(disk->read_size()))
        return true;
    // (There's nothing left of it to tell it to, and a disk has no more than that to do.)
    // (A reference of its own, for as long as the device lasts: it may have been ejected long before it's taken out.)
    auto* kept = new (Base::nothrow) RefPtr<Disk>(reference);
    if (kept)
        device.set_removal_callback([](void* context) { BlockDevice::remove_disk(**static_cast<RefPtr<Disk>*>(context)); }, kept);
    add_disk(reference, "USB"_sv);
    return true;
}

}
