/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/MMIO.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/Interrupts/InterruptHandlers.h>
#include <Kernel/Locking/WaitQueue.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Time/Time.h>
#include <LibBase/New.h>
#include <LibBase/OwnPtr.h>

// NVM Express over PCIe: one admin queue pair to set things up, one I/O
// queue pair for reads, writes and flushes of namespace 1. Requests go one
// at a time through a 64 KiB bounce buffer, described by a PRP list.

namespace Kernel {

using Arch::mmio_read32;
using Arch::mmio_read64;
using Arch::mmio_write32;
using Arch::mmio_write64;

namespace {

// Controller registers (NVMe base specification, "Controller Registers").
constexpr FlatPtr register_capabilities = 0x00;
constexpr FlatPtr register_configuration = 0x14;
constexpr FlatPtr register_status = 0x1c;
constexpr FlatPtr register_admin_queue_attributes = 0x24;
constexpr FlatPtr register_admin_submission_queue = 0x28;
constexpr FlatPtr register_admin_completion_queue = 0x30;
constexpr FlatPtr register_doorbells = 0x1000;

constexpr u32 configuration_enable = 1;
// 64-byte submission and 16-byte completion entries; 4 KiB pages.
constexpr u32 configuration_entry_sizes = (6u << 16) | (4u << 20);
constexpr u32 status_ready = 1;
constexpr u32 status_fatal = 2;

constexpr u8 admin_create_submission_queue = 0x01;
constexpr u8 admin_create_completion_queue = 0x05;
constexpr u8 admin_identify = 0x06;
constexpr u8 io_flush = 0x00;
constexpr u8 io_write = 0x01;
constexpr u8 io_read = 0x02;

constexpr u16 queue_entries = 64;
constexpr size_t transfer_bytes = 64 * KiB;
constexpr unsigned bounce_order = 4;

struct Command {
    u32 dword0; // Opcode, and the command identifier in the top half.
    u32 namespace_id;
    u32 reserved[2];
    u64 metadata;
    u64 prp1;
    u64 prp2;
    u32 dword10;
    u32 dword11;
    u32 dword12;
    u32 dword13;
    u32 dword14;
    u32 dword15;
};
static_assert(sizeof(Command) == 64);

struct Completion {
    u32 result;
    u32 reserved;
    u16 submission_head;
    u16 submission_queue;
    u16 command_id;
    u16 status; // Bit 0 is the phase tag.
};
static_assert(sizeof(Completion) == 16);

void barrier_before_doorbell()
{
    // The entry (in normal memory) must be visible before the device
    // learns of it.
    Arch::device_barrier();
}

class QueuePair {
public:
    QueuePair(u16 id, PhysicalAddress submissions, PhysicalAddress completions, FlatPtr registers, u32 doorbell_stride)
        : m_id(id)
        , m_submissions(submissions)
        , m_completions(completions)
        , m_submission_doorbell(registers + register_doorbells + (2u * id) * doorbell_stride)
        , m_completion_doorbell(registers + register_doorbells + (2u * id + 1) * doorbell_stride)
    {
    }

    [[nodiscard]] u16 id() const { return m_id; }
    [[nodiscard]] PhysicalAddress submissions() const { return m_submissions; }
    [[nodiscard]] PhysicalAddress completions() const { return m_completions; }

    u16 submit(Command command)
    {
        u16 identifier = m_next_identifier++;
        command.dword0 |= static_cast<u32>(identifier) << 16;
        auto* slot = reinterpret_cast<Command*>(physical_to_virtual(m_submissions)) + m_tail;
        __builtin_memcpy(slot, &command, sizeof(command));
        m_tail = static_cast<u16>((m_tail + 1) % queue_entries);
        barrier_before_doorbell();
        mmio_write32(m_submission_doorbell, m_tail);
        return identifier;
    }

    Optional<Completion> take_completion()
    {
        auto const* slot = reinterpret_cast<Completion const volatile*>(physical_to_virtual(m_completions)) + m_head;
        u16 status = slot->status;
        if ((status & 1) != (m_phase ? 1 : 0))
            return {};
        Arch::memory_barrier();
        Completion completion;
        completion.result = slot->result;
        completion.submission_head = slot->submission_head;
        completion.submission_queue = slot->submission_queue;
        completion.command_id = slot->command_id;
        completion.status = status;
        m_head = static_cast<u16>((m_head + 1) % queue_entries);
        if (m_head == 0)
            m_phase = !m_phase;
        mmio_write32(m_completion_doorbell, m_head);
        return Optional<Completion>(completion);
    }

private:
    u16 m_id;
    PhysicalAddress m_submissions;
    PhysicalAddress m_completions;
    FlatPtr m_submission_doorbell;
    FlatPtr m_completion_doorbell;
    u16 m_tail { 0 };
    u16 m_head { 0 };
    bool m_phase { true };
    u16 m_next_identifier { 0 };
};

ErrorOr<void> check_status(Completion const& completion)
{
    // Status code and type, past the phase bit.
    if ((completion.status >> 1) & 0x7ff)
        return Error::from_errno(EIO);
    return {};
}

class NVMeDevice final : public BlockDevice {
public:
    static ErrorOr<NonnullRefPtr<BlockDevice>> create(PCI::Device const& pci, String name);

    ErrorOr<void> read_blocks(u64 first, size_t count, Bytes buffer) override
    {
        for (size_t done = 0; done < count;) {
            size_t blocks = min(count - done, transfer_bytes / block_size);
            // There's one bounce buffer: it's only ours from before the request until what's in it has been dealt with.
            MutexLocker locker(m_lock);
            TRY(transfer(io_read, first + done, blocks));
            __builtin_memcpy(buffer.data() + done * block_size, physical_to_virtual(m_bounce), blocks * block_size);
            done += blocks;
        }
        return {};
    }

    ErrorOr<void> write_blocks(u64 first, size_t count, ReadonlyBytes buffer) override
    {
        for (size_t done = 0; done < count;) {
            size_t blocks = min(count - done, transfer_bytes / block_size);
            MutexLocker locker(m_lock);
            __builtin_memcpy(physical_to_virtual(m_bounce), buffer.data() + done * block_size, blocks * block_size);
            TRY(transfer(io_write, first + done, blocks));
            done += blocks;
        }
        return {};
    }

    ErrorOr<void> flush() override
    {
        MutexLocker locker(m_lock);
        Command command {};
        command.dword0 = io_flush;
        command.namespace_id = 1;
        return run_io(command);
    }

    [[nodiscard]] StringView model() const { return { m_model, m_model_length }; }

private:
    NVMeDevice(String name, u64 block_count, FlatPtr registers, u32 doorbell_stride, PhysicalAddress bounce, PhysicalAddress prp_list)
        : BlockDevice(move(name), block_count, false)
        , m_registers(registers)
        , m_doorbell_stride(doorbell_stride)
        , m_bounce(bounce)
        , m_prp_list(prp_list)
    {
    }

    ErrorOr<void> enable(PhysicalAddress admin_submissions, PhysicalAddress admin_completions);
    ErrorOr<Completion> run_admin(Command);
    ErrorOr<void> run_io(Command);
    ErrorOr<void> identify(u32 namespace_id, u32 controller_or_namespace, PhysicalAddress page);
    ErrorOr<void> create_io_queues(bool with_interrupts);
    void handle_interrupt();

    // With the lock held.
    ErrorOr<void> transfer(u8 opcode, u64 lba, size_t blocks)
    {
        size_t bytes = blocks * block_size;
        Command command {};
        command.dword0 = opcode;
        command.namespace_id = 1;
        command.prp1 = m_bounce.get();
        if (bytes > page_size * 2) {
            // Pages after the first are listed in the PRP list page.
            auto* list = reinterpret_cast<u64*>(physical_to_virtual(m_prp_list));
            for (size_t page = 1; page * page_size < bytes; ++page)
                list[page - 1] = m_bounce.get() + page * page_size;
            command.prp2 = m_prp_list.get();
        } else if (bytes > page_size) {
            command.prp2 = m_bounce.get() + page_size;
        }
        command.dword10 = static_cast<u32>(lba);
        command.dword11 = static_cast<u32>(lba >> 32);
        command.dword12 = static_cast<u32>(blocks - 1);
        return run_io(command);
    }

    FlatPtr m_registers;
    u32 m_doorbell_stride;
    PhysicalAddress m_bounce;
    PhysicalAddress m_prp_list;
    OwnPtr<QueuePair> m_admin;
    OwnPtr<QueuePair> m_io;
    bool m_interrupts { false };
    Mutex m_lock;
    Atomic<bool> m_done { false };
    Completion m_last_completion {};
    WaitQueue m_completed;
    char m_model[41] {};
    size_t m_model_length { 0 };
};

ErrorOr<void> wait_for_status(FlatPtr registers, u32 mask, u32 wanted)
{
    u64 deadline = Time::now() + Time::ticks_from_microseconds(5'000'000);
    while ((mmio_read32(registers + register_status) & mask) != wanted) {
        if (mmio_read32(registers + register_status) & status_fatal)
            return Error::from_string_literal("NVMe controller reports a fatal error");
        if (Time::now() > deadline)
            return Error::from_string_literal("NVMe controller does not become ready");
    }
    return {};
}

ErrorOr<void> NVMeDevice::enable(PhysicalAddress admin_submissions, PhysicalAddress admin_completions)
{
    // Off first (it may have been left on by the firmware), then on with
    // the admin queues in place.
    mmio_write32(m_registers + register_configuration, 0);
    TRY(wait_for_status(m_registers, status_ready, 0));
    mmio_write32(m_registers + register_admin_queue_attributes, ((queue_entries - 1u) << 16) | (queue_entries - 1u));
    mmio_write64(m_registers + register_admin_submission_queue, admin_submissions.get());
    mmio_write64(m_registers + register_admin_completion_queue, admin_completions.get());
    mmio_write32(m_registers + register_configuration, configuration_entry_sizes | configuration_enable);
    return wait_for_status(m_registers, status_ready, status_ready);
}

// Admin commands run at set-up only, and are polled.
ErrorOr<Completion> NVMeDevice::run_admin(Command command)
{
    u16 identifier = m_admin->submit(command);
    u64 deadline = Time::now() + Time::ticks_from_microseconds(5'000'000);
    for (;;) {
        if (auto completion = m_admin->take_completion(); completion.has_value()) {
            // A stale completion from before (there is none at set-up).
            if (completion->command_id != identifier)
                continue;
            TRY(check_status(completion.value()));
            return completion.value();
        }
        if (Time::now() > deadline)
            return Error::from_string_literal("NVMe admin command timed out");
    }
}

ErrorOr<void> NVMeDevice::run_io(Command command)
{
    // In a panic, there may be one under way: its completion shouldn't be taken for this one's.
    if (is_panicking()) {
        u64 const wait_until = Time::now() + Time::ticks_from_microseconds(2'000'000);
        while (!m_done.load() && Time::now() < wait_until)
            handle_interrupt();
    }
    m_done.store(false);
    (void)m_io->submit(command);
    if (m_interrupts && !is_panicking()) {
        m_completed.wait_until([&] { return m_done.load(); });
    } else {
        u64 deadline = Time::now() + Time::ticks_from_microseconds(30'000'000);
        while (!m_done.load()) {
            handle_interrupt();
            if (Time::now() > deadline)
                return Error::from_string_literal("NVMe command timed out");
        }
    }
    return check_status(m_last_completion);
}

// Called by the interrupt (or polled): takes the completion of the one
// request in flight.
void NVMeDevice::handle_interrupt()
{
    bool any = false;
    for (auto completion = m_io->take_completion(); completion.has_value(); completion = m_io->take_completion()) {
        m_last_completion = completion.value();
        any = true;
    }
    if (any) {
        m_done.store(true);
        m_completed.wake_all();
    }
}

ErrorOr<void> NVMeDevice::identify(u32 namespace_id, u32 structure, PhysicalAddress page)
{
    Command command {};
    command.dword0 = admin_identify;
    command.namespace_id = namespace_id;
    command.prp1 = page.get();
    command.dword10 = structure;
    (void)TRY(run_admin(command));
    return {};
}

ErrorOr<void> NVMeDevice::create_io_queues(bool with_interrupts)
{
    auto submissions = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    auto completions = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    m_io = TRY(try_make<QueuePair>(1, submissions, completions, m_registers, m_doorbell_stride));

    Command completion_queue {};
    completion_queue.dword0 = admin_create_completion_queue;
    completion_queue.prp1 = completions.get();
    completion_queue.dword10 = ((queue_entries - 1u) << 16) | 1;
    // Physically contiguous; interrupts (vector 0, the pin) if wanted.
    completion_queue.dword11 = 1 | (with_interrupts ? 2 : 0);
    (void)TRY(run_admin(completion_queue));

    Command submission_queue {};
    submission_queue.dword0 = admin_create_submission_queue;
    submission_queue.prp1 = submissions.get();
    submission_queue.dword10 = ((queue_entries - 1u) << 16) | 1;
    submission_queue.dword11 = (1u << 16) | 1; // Completions to queue 1; contiguous.
    (void)TRY(run_admin(submission_queue));
    return {};
}

ErrorOr<NonnullRefPtr<BlockDevice>> NVMeDevice::create(PCI::Device const& pci, String name)
{
    auto bar = pci.bar(0);
    if (!bar.is_memory || bar.size < 0x2000)
        return Error::from_string_literal("NVMe controller without its register BAR");
    pci.enable_memory_and_bus_mastering();
    FlatPtr registers = TRY(memory_manager().map_mmio(PhysicalAddress(bar.address), min<u64>(bar.size, 0x4000)));
    u64 capabilities = mmio_read64(registers + register_capabilities);
    u32 doorbell_stride = 4u << ((capabilities >> 32) & 0xf);
    u32 minimum_page_shift = 12 + ((capabilities >> 48) & 0xf);
    if (minimum_page_shift > 12)
        return Error::from_string_literal("NVMe controller does not do 4 KiB pages");
    if (!((capabilities >> 37) & 1))
        return Error::from_string_literal("NVMe controller lacks the NVM command set");

    auto bounce = TRY(physical_memory().allocate_pages(bounce_order, PageUse::General));
    auto prp_list = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    auto identify_page = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    auto admin_submissions = TRY(physical_memory().allocate_zeroed_page(PageUse::General));
    auto admin_completions = TRY(physical_memory().allocate_zeroed_page(PageUse::General));

    auto* raw = new (Base::nothrow) NVMeDevice(move(name), 0, registers, doorbell_stride, bounce, prp_list);
    if (!raw)
        return Error::from_errno(ENOMEM);
    auto device = adopt_ref(*raw);
    device->m_admin = TRY(try_make<QueuePair>(0, admin_submissions, admin_completions, registers, doorbell_stride));
    TRY(device->enable(admin_submissions, admin_completions));

    // The controller: its model name.
    TRY(device->identify(0, 1, identify_page));
    auto const* controller = physical_to_virtual(identify_page);
    __builtin_memcpy(device->m_model, controller + 24, 40);
    device->m_model_length = 40;
    while (device->m_model_length > 0 && (device->m_model[device->m_model_length - 1] == ' ' || device->m_model[device->m_model_length - 1] == 0))
        --device->m_model_length;

    // Namespace 1: its size and block format.
    __builtin_memset(physical_to_virtual(identify_page), 0, page_size);
    TRY(device->identify(1, 0, identify_page));
    auto const* ns = physical_to_virtual(identify_page);
    u64 blocks;
    __builtin_memcpy(&blocks, ns, sizeof(blocks));
    u8 format = ns[26] & 0xf;
    u32 format_descriptor;
    __builtin_memcpy(&format_descriptor, ns + 128 + format * 4, sizeof(format_descriptor));
    u32 block_shift = (format_descriptor >> 16) & 0xff;
    (void)physical_memory().free_pages(identify_page);
    if (blocks == 0)
        return Error::from_string_literal("NVMe namespace 1 is empty");
    if (block_shift != 9)
        return Error::from_string_literal("NVMe namespace 1 does not use 512-byte blocks");
    device->set_block_count(blocks);

    auto interrupt = pci.interrupt();
    TRY(device->create_io_queues(interrupt.has_value()));
    if (interrupt.has_value()) {
        register_interrupt_handler(interrupt.value(), [](void* context) { static_cast<NVMeDevice*>(context)->handle_interrupt(); }, raw);
        Arch::enable_interrupt(interrupt.value());
        device->m_interrupts = true;
    }
    return NonnullRefPtr<BlockDevice>(*device);
}

}

ErrorOr<NonnullRefPtr<BlockDevice>> create_nvme_device(PCI::Device const& pci, String name)
{
    return NVMeDevice::create(pci, move(name));
}

StringView nvme_model(BlockDevice& device)
{
    return static_cast<NVMeDevice&>(device).model();
}

}
