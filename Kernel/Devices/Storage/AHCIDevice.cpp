/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/MMIO.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/Mutex.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Time/Time.h>
#include <LibBase/Atomic.h>
#include <LibBase/New.h>
#include <LibBase/String.h>

// AHCI: how SATA disks are attached to most PCs. The controller is one PCI function with up to 32 ports,
// each with its own list of commands to run. This uses one command slot on each port at a time, which
// is simple, and for a disk that does one thing at a time is about as fast; data goes through a buffer
// here, as what's asked for may not be contiguous in physical memory.

namespace Kernel {

namespace {

using Arch::mmio_read32;
using Arch::mmio_write32;

// The controller.
constexpr FlatPtr hba_capabilities = 0x00;
constexpr FlatPtr hba_control = 0x04;
constexpr FlatPtr hba_interrupt_status = 0x08;
constexpr FlatPtr hba_ports_implemented = 0x0c;
constexpr FlatPtr hba_capabilities_2 = 0x24;
constexpr FlatPtr hba_handoff = 0x28;

constexpr u32 capability_64_bit = 1u << 31;
constexpr u32 control_reset = 1u << 0;
constexpr u32 control_interrupts = 1u << 1;
constexpr u32 control_ahci = 1u << 31;
constexpr u32 capability2_handoff = 1u << 0;
constexpr u32 handoff_bios_owned = 1u << 0;
constexpr u32 handoff_os_owned = 1u << 1;
constexpr u32 handoff_bios_busy = 1u << 4;

// A port, from its own place in the registers.
constexpr FlatPtr first_port = 0x100;
constexpr FlatPtr port_size = 0x80;
constexpr FlatPtr port_command_list = 0x00;
constexpr FlatPtr port_receive_area = 0x08;
constexpr FlatPtr port_interrupt_status = 0x10;
constexpr FlatPtr port_interrupt_enable = 0x14;
constexpr FlatPtr port_command = 0x18;
constexpr FlatPtr port_task_file = 0x20;
constexpr FlatPtr port_signature = 0x24;
constexpr FlatPtr port_status = 0x28;
constexpr FlatPtr port_error = 0x30;
constexpr FlatPtr port_issue = 0x38;

constexpr u32 command_start = 1u << 0;
constexpr u32 command_spin_up = 1u << 1;
constexpr u32 command_power_on = 1u << 2;
constexpr u32 command_receive = 1u << 4;
constexpr u32 command_receive_running = 1u << 14;
constexpr u32 command_list_running = 1u << 15;
constexpr u32 command_active = 1u << 28;

constexpr u32 task_busy = 1u << 7;
constexpr u32 task_request = 1u << 3;
constexpr u32 task_error = 1u << 0;

constexpr u32 interrupt_register_d2h = 1u << 0;
constexpr u32 interrupt_task_file_error = 1u << 30;
constexpr u32 interrupt_errors = 0x7d800000;

constexpr u32 signature_disk = 0x00000101;

constexpr u8 fis_register_h2d = 0x27;
constexpr u8 ata_identify = 0xec;
constexpr u8 ata_read_extended = 0x25;
constexpr u8 ata_write_extended = 0x35;
constexpr u8 ata_flush_extended = 0xea;

struct CommandHeader {
    u16 flags; // Length of the command, in words; bit 6: a write.
    u16 table_entries;
    u32 bytes_transferred;
    u32 table_low;
    u32 table_high;
    u32 reserved[4];
};
static_assert(sizeof(CommandHeader) == 32);

struct TableEntry {
    u32 data_low;
    u32 data_high;
    u32 reserved;
    u32 count_minus_one;
};
static_assert(sizeof(TableEntry) == 16);

struct CommandTable {
    u8 command[64];
    u8 atapi[16];
    u8 reserved[48];
    TableEntry entries[1];
};
static_assert(__builtin_offsetof(CommandTable, entries) == 128);

// How much one command moves: 64 KiB, as the order of an allocation of the buffer.
constexpr unsigned buffer_order = 4;
constexpr size_t buffer_size = page_size << buffer_order;
constexpr size_t blocks_per_command = buffer_size / BlockDevice::block_size;

class AHCIController;

class AHCIDisk final : public BlockDevice {
public:
    AHCIDisk(String name, u64 block_count, FlatPtr port, PhysicalAddress memory, PhysicalAddress buffer)
        : BlockDevice(move(name), block_count, false)
        , m_port(port)
        , m_memory(memory)
        , m_buffer(buffer)
    {
    }

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes destination) override
    {
        MutexLocker locker(m_lock);
        size_t done = 0;
        while (done < count) {
            size_t const now = min(count - done, blocks_per_command);
            TRY(run(ata_read_extended, first + done, now, false));
            __builtin_memcpy(destination.data() + done * block_size, physical_to_virtual(m_buffer), now * block_size);
            done += now;
        }
        return {};
    }

    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes source) override
    {
        MutexLocker locker(m_lock);
        size_t done = 0;
        while (done < count) {
            size_t const now = min(count - done, blocks_per_command);
            __builtin_memcpy(physical_to_virtual(m_buffer), source.data() + done * block_size, now * block_size);
            TRY(run(ata_write_extended, first + done, now, true));
            done += now;
        }
        return {};
    }

    ErrorOr<void> flush() override
    {
        MutexLocker locker(m_lock);
        return run(ata_flush_extended, 0, 0, false);
    }

    void handle_interrupt()
    {
        u32 const status = mmio_read32(m_port + port_interrupt_status);
        if (status == 0)
            return;
        mmio_write32(m_port + port_interrupt_status, status);
        if (status & interrupt_errors)
            m_failed.store(true);
        m_changed.store(m_changed.load() + 1);
        m_wait.wake_all();
    }

    // The first command to run, before this is registered. Polled: there may be no interrupts yet.
    ErrorOr<void> run(u8 ata_command, u64 lba, size_t blocks, bool write)
    {
        auto* header = physical_to_virtual<CommandHeader>(m_memory);
        auto* table = physical_to_virtual<CommandTable>(m_memory.offset(page_size));
        __builtin_memset(table, 0, sizeof(CommandTable));
        table->command[0] = fis_register_h2d;
        table->command[1] = 1 << 7; // A command.
        table->command[2] = ata_command;
        table->command[4] = static_cast<u8>(lba);
        table->command[5] = static_cast<u8>(lba >> 8);
        table->command[6] = static_cast<u8>(lba >> 16);
        table->command[7] = 1 << 6; // LBA mode.
        table->command[8] = static_cast<u8>(lba >> 24);
        table->command[9] = static_cast<u8>(lba >> 32);
        table->command[10] = static_cast<u8>(lba >> 40);
        table->command[12] = static_cast<u8>(blocks);
        table->command[13] = static_cast<u8>(blocks >> 8);
        size_t const bytes = (ata_command == ata_identify ? 1 : blocks) * block_size;
        if (bytes) {
            table->entries[0].data_low = static_cast<u32>(m_buffer.get());
            table->entries[0].data_high = static_cast<u32>(m_buffer.get() >> 32);
            table->entries[0].count_minus_one = static_cast<u32>(bytes - 1);
        }
        __builtin_memset(header, 0, sizeof(CommandHeader));
        header->flags = static_cast<u16>(5 | (write ? 1 << 6 : 0)); // The command is five words long.
        header->table_entries = bytes ? 1 : 0;
        header->table_low = static_cast<u32>(m_memory.get() + page_size);
        header->table_high = static_cast<u32>((m_memory.get() + page_size) >> 32);

        m_failed.store(false);
        Arch::device_barrier();
        mmio_write32(m_port + port_issue, 1);

        // A command that hangs is as good as a failed one, after long enough: a disk that has to spin up
        // may well take a while.
        u64 const deadline = Time::now() + Time::ticks_from_milliseconds(30'000);
        while (mmio_read32(m_port + port_issue) & 1) {
            if (m_use_interrupts) {
                u32 const seen = m_changed.load();
                if (!(mmio_read32(m_port + port_issue) & 1))
                    break;
                (void)m_wait.wait_until_interruptible([&] { return m_changed.load() != seen; }, Time::now() + Time::ticks_from_milliseconds(100));
            }
            if (mmio_read32(m_port + port_interrupt_status) & interrupt_task_file_error)
                m_failed.store(true);
            if (m_failed.load())
                break;
            if (Time::now() > deadline) {
                dmesgln("AHCI: {}: command {:#x} timed out", name(), ata_command);
                return Error::from_errno(EIO);
            }
            Arch::pause();
        }
        Arch::memory_barrier();
        if (m_failed.load() || (mmio_read32(m_port + port_task_file) & task_error)) {
            dmesgln("AHCI: {}: command {:#x} failed: task file {:#x}, error {:#x}", name(), ata_command, mmio_read32(m_port + port_task_file), mmio_read32(m_port + port_error));
            return Error::from_errno(EIO);
        }
        return {};
    }

    [[nodiscard]] u8 const* buffer() const { return physical_to_virtual(m_buffer); }
    void set_size(u64 blocks) { set_block_count(blocks); }
    void use_interrupts() { m_use_interrupts = true; }

private:
    FlatPtr m_port;
    PhysicalAddress m_memory; // Two pages: the command list and FIS area, and the command table.
    PhysicalAddress m_buffer;
    Mutex m_lock;
    Atomic<bool> m_failed { false };
    Atomic<u32> m_changed { 0 };
    WaitQueue m_wait;
    bool m_use_interrupts { false };
};

bool wait_until_clear(FlatPtr address, u32 bits, u64 milliseconds)
{
    u64 const deadline = Time::now() + Time::ticks_from_milliseconds(milliseconds);
    while (mmio_read32(address) & bits) {
        if (Time::now() > deadline)
            return false;
        Arch::pause();
    }
    return true;
}

// Takes the port from whatever state the firmware left it in to ready for commands.
ErrorOr<void> start_port(FlatPtr port, PhysicalAddress memory)
{
    u32 command = mmio_read32(port + port_command);
    if (command & (command_start | command_receive)) {
        mmio_write32(port + port_command, command & ~(command_start | command_receive));
        if (!wait_until_clear(port + port_command, command_list_running | command_receive_running, 500))
            return Error::from_string_literal("the port won't stop");
    }
    // The list and the area for what arrives, at the start of their page: 1 KiB, and 256 bytes.
    PhysicalAddress const receive = memory.offset(page_size / 2);
    mmio_write32(port + port_command_list, static_cast<u32>(memory.get()));
    mmio_write32(port + port_command_list + 4, static_cast<u32>(memory.get() >> 32));
    mmio_write32(port + port_receive_area, static_cast<u32>(receive.get()));
    mmio_write32(port + port_receive_area + 4, static_cast<u32>(receive.get() >> 32));
    mmio_write32(port + port_error, 0xffff'ffff);
    mmio_write32(port + port_interrupt_status, 0xffff'ffff);
    command = mmio_read32(port + port_command) | command_power_on | command_spin_up | command_receive;
    mmio_write32(port + port_command, command);
    if (!wait_until_clear(port + port_task_file, task_busy | task_request, 1000))
        return Error::from_string_literal("the disk stays busy");
    mmio_write32(port + port_command, mmio_read32(port + port_command) | command_start | command_active);
    return {};
}

}

namespace {

// What the one interrupt of a controller, with up to 32 disks on it, goes to.
struct Controller {
    Vector<AHCIDisk*> disks;
};

void handle_controller_interrupt(void* context)
{
    auto& controller = *static_cast<Controller*>(context);
    for (auto* disk : controller.disks)
        disk->handle_interrupt();
}

}

ErrorOr<void> create_ahci_disks(PCI::Device const& pci, u32& next_disk, Vector<NonnullRefPtr<BlockDevice>>& disks)
{
    auto const bar = pci.bar(5);
    if (!bar.is_memory || bar.address == 0)
        return Error::from_string_literal("no registers");
    pci.enable_memory_and_bus_mastering();
    FlatPtr const registers = TRY(memory_manager().map_mmio(PhysicalAddress(bar.address), bar.size ? bar.size : 0x1100));

    // On some machines, the firmware has to be asked for it.
    if ((mmio_read32(registers + hba_capabilities_2) & capability2_handoff)) {
        mmio_write32(registers + hba_handoff, mmio_read32(registers + hba_handoff) | handoff_os_owned);
        (void)wait_until_clear(registers + hba_handoff, handoff_bios_owned | handoff_bios_busy, 2000);
    }
    mmio_write32(registers + hba_control, control_ahci);
    mmio_write32(registers + hba_control, control_ahci | control_reset);
    if (!wait_until_clear(registers + hba_control, control_reset, 1000))
        return Error::from_string_literal("the controller won't reset");
    mmio_write32(registers + hba_control, control_ahci);

    u32 const capabilities = mmio_read32(registers + hba_capabilities);
    u32 const ports = mmio_read32(registers + hba_ports_implemented);
    if (!(capabilities & capability_64_bit))
        dmesgln("AHCI: this controller only reaches the first 4 GiB of memory");

    // After the reset, a disk takes a moment to say it's there.
    u64 const link_deadline = Time::now() + Time::ticks_from_milliseconds(200);
    auto* controller = new (Base::nothrow) Controller;
    if (!controller)
        return Error::from_errno(ENOMEM);
    for (u32 index = 0; index < 32; ++index) {
        if (!(ports & (1u << index)))
            continue;
        FlatPtr const port = registers + first_port + index * port_size;
        while ((mmio_read32(port + port_status) & 0xf) != 3 && Time::now() < link_deadline)
            Arch::pause();
        if ((mmio_read32(port + port_status) & 0xf) != 3)
            continue;
        // Two pages: the list and what arrives, and the command itself.
        auto const memory = TRY(physical_memory().allocate_pages(1, PageUse::General));
        __builtin_memset(physical_to_virtual(memory), 0, 2 * page_size);
        auto const buffer = TRY(physical_memory().allocate_pages(buffer_order, PageUse::General));
        if (!(capabilities & capability_64_bit) && (buffer.get() + buffer_size > 0x1'0000'0000 || memory.get() + 2 * page_size > 0x1'0000'0000)) {
            dmesgln("AHCI: port {}: its memory is out of this controller's reach", index);
            physical_memory().free_pages(memory);
            physical_memory().free_pages(buffer);
            continue;
        }
        if (auto started = start_port(port, memory); started.is_error()) {
            dmesgln("AHCI: port {}: {}", index, started.error());
            physical_memory().free_pages(memory);
            physical_memory().free_pages(buffer);
            continue;
        }
        // (Only valid now: it comes from what the disk first sent, which needed somewhere to go.)
        if (mmio_read32(port + port_signature) != signature_disk) {
            mmio_write32(port + port_command, mmio_read32(port + port_command) & ~(command_start | command_receive));
            continue;
        }
        StringBuilder name;
        name.appendff("disk{}", next_disk);
        auto* disk = new (Base::nothrow) AHCIDisk(MUST(name.to_string()), 0, port, memory, buffer);
        if (!disk)
            return Error::from_errno(ENOMEM);
        auto reference = adopt_ref(*disk);
        if (auto identified = disk->run(ata_identify, 0, 0, false); identified.is_error()) {
            dmesgln("AHCI: port {}: identify failed", index);
            continue;
        }
        auto const* identity = reinterpret_cast<u16 const*>(disk->buffer());
        // Words 100 to 103: the size, with 48-bit addresses.
        u64 blocks = 0;
        for (int word = 0; word < 4; ++word)
            blocks |= static_cast<u64>(identity[100 + word]) << (16 * word);
        if (blocks == 0) {
            dmesgln("AHCI: port {}: not a disk this can address", index);
            continue;
        }
        disk->set_size(blocks);
        ++next_disk;
        controller->disks.append(disk);
        TRY(disks.try_append(reference));
    }
    auto const interrupt = pci.interrupt();
    if (interrupt.has_value() && !controller->disks.is_empty()) {
        register_interrupt_handler(interrupt.value(), handle_controller_interrupt, controller);
        for (auto* disk : controller->disks)
            disk->use_interrupts();
        mmio_write32(registers + hba_interrupt_status, 0xffff'ffff);
        for (u32 index = 0; index < 32; ++index) {
            if (ports & (1u << index))
                mmio_write32(registers + first_port + index * port_size + port_interrupt_enable, interrupt_register_d2h | interrupt_errors);
        }
        mmio_write32(registers + hba_control, control_ahci | control_interrupts);
        Arch::enable_interrupt(interrupt.value());
    }
    return {};
}

}
