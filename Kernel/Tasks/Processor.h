/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Arch/CPU.h>
#include <Kernel/Memory/Heap.h>
#include <Kernel/Memory/PhysicalMemory.h>
#include <Kernel/Tasks/RunQueue.h>
#include <Kernel/Time/TimerQueue.h>
#include <LibBase/Atomic.h>

namespace DeviceTree {
class FlattenedDeviceTree;
}

namespace Kernel {

enum class IPI : u8;

using ProcessorFunction = void (*)(void* argument);

// A request for another processor to run a function, from call_on_all().
struct ProcessorCall {
    ProcessorFunction function { nullptr };
    void* argument { nullptr };
    Atomic<u32>* remaining { nullptr };
    ProcessorCall* next { nullptr };
};

// Everything that belongs to one CPU. The current processor's object is
// always at hand through Processor::current() (the arch keeps a pointer in
// a CPU register), so per-CPU caches and queues need no locks: code only
// touches its own processor's, with interrupts masked. (With interrupts
// enabled, a thread may move to another processor at any moment.)
class Processor {
public:
    static constexpr u32 max_count = 64;

    [[nodiscard]] static Processor& current() { return *static_cast<Processor*>(Arch::current_processor_pointer()); }

    // The processor the firmware started us on, running on the boot stack.
    static void initialize_boot_processor(FlatPtr stack_bottom, FlatPtr stack_top);

    // Starts every other CPU listed in the device tree and waits until each
    // is online (or gives up on it).
    static void start_secondary_processors(DeviceTree::FlattenedDeviceTree const&);

    // Processors that are up and taking work, how many the device tree
    // lists, and how many have a Processor object (valid ids).
    [[nodiscard]] static u32 online_count() { return s_online_count.load(MemoryOrder::Acquire); }
    [[nodiscard]] static u32 listed_count() { return s_listed_count; }
    [[nodiscard]] static u32 count() { return s_processor_count.load(MemoryOrder::Acquire); }
    [[nodiscard]] static Processor& with_id(u32 id);

    // Runs function(argument) on every online processor at once, in
    // interrupt context (interrupts masked), and returns when all are done.
    // For short work on per-processor state. Call with interrupts enabled.
    static void call_on_all(ProcessorFunction, void* argument);

    // Runs function(argument) in a thread pinned to each online processor
    // and returns when all have finished. The function may block. Panics if
    // the threads cannot be created.
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
    [[nodiscard]] u64 context_switch_count() const { return m_context_switches; }

    [[nodiscard]] PhysicalPageCache& page_cache() { return m_page_cache; }
    [[nodiscard]] HeapProcessorCache& heap_cache() { return m_heap_cache; }
    [[nodiscard]] TimerQueue& timer_queue() { return m_timer_queue; }
    [[nodiscard]] RunQueue& run_queue() { return m_run_queue; }

    // Null before the scheduler starts.
    [[nodiscard]] Thread* current_thread() const { return m_current_thread; }
    [[nodiscard]] Thread* idle_thread() const { return m_idle_thread; }

    // Finishes bringing up the current processor: floating point, its part
    // of the interrupt controller, and its timer.
    void initialize_this_processor();

private:
    friend class Scheduler;
    friend void secondary_processor_main(void*);

    void initialize(u32 id, u64 hardware_id, FlatPtr stack_bottom, FlatPtr stack_top);
    void enqueue_call(ProcessorCall&);
    void run_pending_calls();

    static Processor* s_processors[max_count];
    static Atomic<u32> s_processor_count;
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

    // Scheduling. Only this processor writes these, with interrupts masked;
    // the atomics are read by others as hints.
    Thread* m_current_thread { nullptr };
    Thread* m_idle_thread { nullptr };
    Thread* m_switching_from { nullptr };
    RunQueue m_run_queue;
    Timer m_time_slice;
    bool m_need_reschedule { false };
    Atomic<u8> m_running_rank { 0 };
    Atomic<bool> m_time_slice_armed { false };
    u64 m_context_switches { 0 };

    PhysicalPageCache m_page_cache;
    HeapProcessorCache m_heap_cache;
    TimerQueue m_timer_queue;
};

}
