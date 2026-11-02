/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/Power.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Debug/Symbols.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tasks/Thread.h>

extern "C" u8 boot_stack_bottom[];
extern "C" u8 boot_stack_top[];

namespace Kernel {

static constexpr size_t max_backtrace_frames = 32;
static constexpr Color panic_background { 0x1c, 0x1c, 0x1e };
static constexpr Color panic_foreground { 0xf2, 0xf2, 0xf7 };

static Atomic<bool> s_power_off_on_panic { false };

void set_power_off_on_panic(bool enabled)
{
    s_power_off_on_panic.store(enabled);
}

// Frame records live on the current processor's stack, 16-byte aligned.
// Following anything else could fault in the middle of a panic.
static bool is_valid_frame_pointer(FlatPtr frame_pointer)
{
    auto bottom = reinterpret_cast<FlatPtr>(boot_stack_bottom);
    auto top = reinterpret_cast<FlatPtr>(boot_stack_top);
    if (Arch::current_processor_pointer()) {
        bottom = Processor::current().stack_bottom();
        top = Processor::current().stack_top();
        if (auto* thread = Processor::current().current_thread()) {
            bottom = thread->stack().bottom;
            top = thread->stack().top;
        }
    }
    return frame_pointer % 16 == 0 && frame_pointer >= bottom && frame_pointer + sizeof(Arch::StackFrame) <= top;
}

static void print_frame(size_t index, FlatPtr address, FlatPtr lookup_address)
{
    FixedStringBuilder<256> line;
    format_to(line, "  #{:<2} {:#018x}  ", index, address);
    Symbols::write_symbolicated(line, lookup_address);
    dmesgln("{}", line.view());
}

// `pc_is_return_address` is true when `pc` points just past a call: the
// call itself is the instruction before it.
static void print_backtrace(FlatPtr pc, bool pc_is_return_address, FlatPtr frame_pointer)
{
    dmesgln("Backtrace:");
#if defined(__aarch64__)
    // (A return address may be signed: see below.)
    pc |= 0xffff'0000'0000'0000;
#endif
    print_frame(0, pc, pc_is_return_address ? pc - 4 : pc);
    for (size_t index = 1; index < max_backtrace_frames && is_valid_frame_pointer(frame_pointer); ++index) {
        auto record = *reinterpret_cast<Arch::StackFrame const*>(frame_pointer);
#if defined(__aarch64__)
        // A signed return address has the signature in its top bits: an address up there has
        // those all set otherwise.
        if (record.return_address != 0)
            record.return_address |= 0xffff'0000'0000'0000;
#endif
        if (record.return_address == 0)
            break;
        print_frame(index, record.return_address, record.return_address - 4);
        // The stack grows down, so callers' records are at higher addresses.
        if (record.previous_frame_pointer <= frame_pointer)
            break;
        frame_pointer = record.previous_frame_pointer;
    }
}

static void show_panic_console()
{
    auto& framebuffer = boot_framebuffer();
    if (!framebuffer.is_present())
        return;
    auto& console = boot_text_console();
    console.set_colors(panic_foreground, panic_background);
    console.initialize(framebuffer);
    kernel_log().attach_console(&console);
}

void panic(StringView reason, TrapFrame const* frame)
{
    // The first processor to panic reports; the token is its hardware id + 1.
    static Atomic<u64> s_panicking_processor { 0 };
    [[maybe_unused]] auto interrupt_state = Arch::disable_interrupts();
    u64 self = Arch::processor_hardware_id() + 1;
    u64 expected = 0;
    if (!s_panicking_processor.compare_exchange_strong(expected, self)) {
        if (expected == self) {
            // A panic while reporting a panic: keep it short and stop.
            kernel_log().attach_console(nullptr);
            dmesgln("KERNEL PANIC while panicking: {}", reason);
        }
        // Another processor is already reporting.
        Arch::halt();
    }

    Processor::stop_others();
    kernel_log().enter_panic_mode();
    show_panic_console();
    dmesgln("vultureOS needs to restart: the kernel ran into a problem it cannot recover from.");
    dmesgln("");
    dmesgln("KERNEL PANIC: {}", reason);

    FixedStringBuilder<160> cpu;
    Arch::describe_cpu(cpu);
    if (Arch::current_processor_pointer() && Processor::current().current_thread()) {
        auto& thread = *Processor::current().current_thread();
        dmesgln("Processor {}: {}", Processor::current().id(), cpu.view());
        dmesgln("Thread {} \"{}\"", thread.id(), thread.name());
    } else if (Arch::current_processor_pointer())
        dmesgln("Processor {}: {}", Processor::current().id(), cpu.view());
    else
        dmesgln("CPU: {}", cpu.view());

    if (frame) {
        FixedStringBuilder<2048> registers;
        Arch::dump_registers(registers, *frame);
        kernel_log().write(registers.view());
        print_backtrace(frame->program_counter(), false, frame->frame_pointer());
    } else {
        // Start from whoever called panic().
        auto const& own_record = *reinterpret_cast<Arch::StackFrame const*>(__builtin_frame_address(0));
        print_backtrace(own_record.return_address, true, own_record.previous_frame_pointer);
    }

    dmesgln("");
    if (s_power_off_on_panic.load()) {
        dmesgln("Powering off.");
        Arch::power_off();
    }
    dmesgln("System halted.");
    Arch::halt();
}

}
