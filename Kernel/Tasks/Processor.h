/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/CPU.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Time/TimerQueue.h>
#include <LibBase/Atomic.h>

namespace DeviceTree {
class FlattenedDeviceTree;
}

namespace Kernel {

enum class IPI : u8;

using ProcessorFunction = void (*)(void* argument);

// A request for another processor to run a function, from run_on_all().
struct ProcessorCall {
    ProcessorFunction function { nullptr };
    void* argument { nullptr };
    Atomic<u32>* remaining { nullptr };
    ProcessorCall* next { nullptr };
};

// Everything that belongs to one CPU. The current processor's object is
// always at hand through Processor::current() (the arch keeps a pointer in
// a CPU register), so per-CPU caches and queues need no locks: code only
// touches its own processor's, with interrupts masked.
class Processor {
public:
    static constexpr u32 max_count = 64;

    [[nodiscard]] static Processor& current() { return *static_cast<Processor*>(Arch::current_processor_pointer()); }

    // The processor the firmware started us on, running on the boot stack.
    static void initialize_boot_processor(FlatPtr stack_bottom, FlatPtr stack_top);

    // Starts every other CPU listed in the device tree and waits until each
    // is online (or gives up on it).
    static void start_secondary_processors(DeviceTree::FlattenedDeviceTree const&);

    // Processors that are up and taking work, and how many the device tree lists.
    [[nodiscard]] static u32 online_count() { return s_online_count.load(MemoryOrder::Acquire); }
    [[nodiscard]] static u32 listed_count() { return s_listed_count; }
    [[nodiscard]] static Processor& with_id(u32 id);

    // Runs function(argument) on every online processor at once, this one
    // included, and returns when all have finished. Other processors run it
    // from their idle loop, with interrupts enabled. Must be called with
    // interrupts enabled, from the boot processor.
    static void run_on_all(ProcessorFunction, void* argument);

    // Halts every other processor. For panics: does not wait for them.
    static void stop_others();

    // From the interrupt controller, when an IPI arrives.
    static void handle_ipi(IPI);

    [[nodiscard]] u32 id() const { return m_id; }
    [[nodiscard]] u64 hardware_id() const { return m_hardware_id; }
    [[nodiscard]] FlatPtr stack_bottom() const { return m_stack_bottom; }
    [[nodiscard]] FlatPtr stack_top() const { return m_stack_top; }
    [[nodiscard]] bool is_online() const { return m_online.load(MemoryOrder::Acquire); }

    [[nodiscard]] bool is_in_interrupt() const { return m_interrupt_depth > 0; }
    void enter_interrupt()
    {
        ++m_interrupt_depth;
        ++m_interrupt_count;
    }
    void leave_interrupt() { --m_interrupt_depth; }
    [[nodiscard]] u64 interrupt_count() const { return m_interrupt_count; }

    [[nodiscard]] PhysicalPageCache& page_cache() { return m_page_cache; }
    [[nodiscard]] HeapProcessorCache& heap_cache() { return m_heap_cache; }
    [[nodiscard]] TimerQueue& timer_queue() { return m_timer_queue; }

    // Finishes bringing up the current processor: its part of the interrupt
    // controller, and its timer.
    void initialize_this_processor();

    // Waits for interrupts and runs work other processors hand over.
    [[noreturn]] void idle_loop();

private:
    friend void secondary_processor_main(void*);

    void initialize(u32 id, u64 hardware_id, FlatPtr stack_bottom, FlatPtr stack_top);
    void enqueue_call(ProcessorCall&);
    // Runs queued calls; returns false if there were none.
    bool run_pending_calls();

    static Processor* s_processors[max_count];
    static Atomic<u32> s_online_count;
    static u32 s_listed_count;

    u32 m_id { 0 };
    u64 m_hardware_id { 0 };
    FlatPtr m_stack_bottom { 0 };
    FlatPtr m_stack_top { 0 };
    Atomic<bool> m_online { false };

    u32 m_interrupt_depth { 0 };
    u64 m_interrupt_count { 0 };

    Atomic<ProcessorCall*> m_pending_calls { nullptr };

    PhysicalPageCache m_page_cache;
    HeapProcessorCache m_heap_cache;
    TimerQueue m_timer_queue;
};

}
