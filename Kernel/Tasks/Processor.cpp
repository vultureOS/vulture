/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/InterruptController.h>
#include <Kernel/Arch/SMP.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Time/Time.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

namespace Kernel {

// How long a secondary processor gets to report in.
static constexpr u64 startup_timeout_milliseconds = 1000;

Processor* Processor::s_processors[max_count];
Atomic<u32> Processor::s_online_count { 0 };
u32 Processor::s_listed_count = 1;

static Processor s_boot_processor;
static u32 s_processor_count = 0;

void Processor::initialize(u32 id, u64 hardware_id, FlatPtr stack_bottom, FlatPtr stack_top)
{
    m_id = id;
    m_hardware_id = hardware_id;
    m_stack_bottom = stack_bottom;
    m_stack_top = stack_top;
}

void Processor::initialize_boot_processor(FlatPtr stack_bottom, FlatPtr stack_top)
{
    s_boot_processor.initialize(0, Arch::processor_hardware_id(), stack_bottom, stack_top);
    s_processors[0] = &s_boot_processor;
    s_processor_count = 1;
    Arch::set_current_processor_pointer(&s_boot_processor);
    s_boot_processor.m_online.store(true, MemoryOrder::Release);
    s_online_count.store(1, MemoryOrder::Release);
}

Processor& Processor::with_id(u32 id)
{
    VERIFY(id < s_processor_count && s_processors[id]);
    return *s_processors[id];
}

void Processor::initialize_this_processor()
{
    if (auto result = Arch::initialize_interrupt_controller_on_this_processor(); result.is_error()) {
        FixedStringBuilder<128> reason;
        format_to(reason, "processor {}: interrupt controller: {}", m_id, result.error());
        panic(reason.view());
    }
    Time::initialize_on_this_processor();
}

void secondary_processor_main(void* argument)
{
    auto& processor = *static_cast<Processor*>(argument);
    Arch::set_current_processor_pointer(&processor);
    processor.initialize_this_processor();
    processor.m_online.store(true, MemoryOrder::Release);
    Processor::s_online_count.fetch_add(1, MemoryOrder::AcquireRelease);
    processor.idle_loop();
}

// The hardware ids of the processors in /cpus, in device tree order.
static size_t find_processors(DeviceTree::FlattenedDeviceTree const& device_tree, u64 (&hardware_ids)[Processor::max_count])
{
    size_t count = 0;
    bool in_cpus = false;
    auto result = device_tree.for_each_node([&](DeviceTree::Node const& node) {
        if (node.depth == 1) {
            if (in_cpus)
                return DeviceTree::IterationDecision::Break;
            in_cpus = node.name == "cpus"_sv;
            return DeviceTree::IterationDecision::Continue;
        }
        if (!in_cpus || node.depth != 2)
            return DeviceTree::IterationDecision::Continue;
        auto device_type = device_tree.find_property(node, "device_type"_sv);
        if (!device_type.has_value() || device_type->as_string() != "cpu"_sv)
            return DeviceTree::IterationDecision::Continue;
        auto reg = device_tree.reg(node, 0);
        if (reg.has_value() && count < Processor::max_count)
            hardware_ids[count++] = reg->address;
        return DeviceTree::IterationDecision::Continue;
    });
    if (result.is_error())
        dmesgln("SMP: unreadable device tree ({})", result.error());
    return count;
}

void Processor::start_secondary_processors(DeviceTree::FlattenedDeviceTree const& device_tree)
{
    u64 hardware_ids[max_count];
    size_t count = find_processors(device_tree, hardware_ids);
    s_listed_count = max<u32>(static_cast<u32>(count), 1);
    bool all_reported = true;

    for (size_t i = 0; i < count; ++i) {
        if (hardware_ids[i] == s_boot_processor.hardware_id())
            continue;
        auto stack = memory_manager().allocate_kernel_stack();
        if (stack.is_error()) {
            dmesgln("SMP: no stack for processor {:#x}: {}", hardware_ids[i], stack.error());
            continue;
        }
        u32 id = s_processor_count;
        auto* processor = new Processor;
        processor->initialize(id, hardware_ids[i], stack.value().bottom, stack.value().top);

        if (auto result = Arch::start_processor(hardware_ids[i], stack.value().top, secondary_processor_main, processor); result.is_error()) {
            dmesgln("SMP: could not start processor {:#x}: {}", hardware_ids[i], result.error());
            memory_manager().free_kernel_stack(stack.value());
            delete processor;
            continue;
        }
        s_processors[id] = processor;
        ++s_processor_count;

        // One at a time: simple, and fast enough (well under a millisecond each).
        u64 deadline = Time::now() + Time::ticks_from_milliseconds(startup_timeout_milliseconds);
        while (!processor->is_online() && Time::now() < deadline)
            Arch::pause();
        if (!processor->is_online()) {
            dmesgln("SMP: processor {:#x} did not come online", hardware_ids[i]);
            all_reported = false;
        }
    }

    // A processor that never reported in may still be reading its startup
    // state; keep it around in that case.
    if (all_reported)
        Arch::finish_starting_processors();
}

void Processor::idle_loop()
{
    for (;;) {
        // Mask, check, then wait: an IPI arriving after the check still
        // ends the wait, since WFI returns for pending interrupts.
        [[maybe_unused]] auto state = Arch::disable_interrupts();
        if (m_pending_calls.load(MemoryOrder::Relaxed)) {
            Arch::enable_interrupts();
            run_pending_calls();
            continue;
        }
        Arch::wait_for_interrupt();
        Arch::enable_interrupts();
    }
}

void Processor::enqueue_call(ProcessorCall& call)
{
    auto* head = m_pending_calls.load(MemoryOrder::Relaxed);
    do {
        call.next = head;
    } while (!m_pending_calls.compare_exchange_strong(head, &call, MemoryOrder::Release));
}

bool Processor::run_pending_calls()
{
    auto* call = m_pending_calls.exchange(nullptr, MemoryOrder::Acquire);
    if (!call)
        return false;
    while (call) {
        // The caller may return (and its stack, holding `call`, go away) as
        // soon as `remaining` drops; read everything needed first.
        auto* next = call->next;
        call->function(call->argument);
        call->remaining->fetch_sub(1, MemoryOrder::Release);
        call = next;
    }
    return true;
}

void Processor::run_on_all(ProcessorFunction function, void* argument)
{
    VERIFY(Arch::are_interrupts_enabled());
    auto& self = current();
    VERIFY(&self == &s_boot_processor);

    ProcessorCall calls[max_count];
    Atomic<u32> remaining { 0 };
    for (u32 id = 0; id < s_processor_count; ++id) {
        auto* processor = s_processors[id];
        if (processor == &self || !processor->is_online())
            continue;
        calls[id] = { function, argument, &remaining, nullptr };
        remaining.fetch_add(1, MemoryOrder::Relaxed);
        processor->enqueue_call(calls[id]);
    }
    // Every other processor is a target: one broadcast instead of one IPI
    // each (sending one can trap to the hypervisor).
    if (remaining.load(MemoryOrder::Relaxed) > 0)
        Arch::send_ipi_to_others(IPI::Wakeup);
    function(argument);
    while (remaining.load(MemoryOrder::Acquire) != 0)
        Arch::pause();
}

void Processor::stop_others()
{
    if (s_online_count.load(MemoryOrder::Relaxed) > 1)
        Arch::send_ipi_to_others(IPI::Stop);
}

void Processor::handle_ipi(IPI ipi)
{
    switch (ipi) {
    case IPI::Wakeup:
        // The idle loop finds the work once the interrupt returns.
        break;
    case IPI::Stop:
        Arch::halt();
    }
}

}
