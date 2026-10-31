/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>
#include <Kernel/Console/TextConsole.h>
#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Debug/Symbols.h>

extern "C" u8 boot_stack_bottom[];
extern "C" u8 boot_stack_top[];

namespace Kernel {

static constexpr size_t max_backtrace_frames = 32;
static constexpr Color panic_background { 0x1c, 0x1c, 0x1e };
static constexpr Color panic_foreground { 0xf2, 0xf2, 0xf7 };

// Frame records live on the stack, 16-byte aligned. Following anything else
// could fault in the middle of a panic.
static bool is_valid_frame_pointer(FlatPtr frame_pointer)
{
    auto bottom = reinterpret_cast<FlatPtr>(boot_stack_bottom);
    auto top = reinterpret_cast<FlatPtr>(boot_stack_top);
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
    print_frame(0, pc, pc_is_return_address ? pc - 4 : pc);
    for (size_t index = 1; index < max_backtrace_frames && is_valid_frame_pointer(frame_pointer); ++index) {
        auto const& record = *reinterpret_cast<Arch::StackFrame const*>(frame_pointer);
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
    static bool s_panicking = false;
    if (s_panicking) {
        // A panic while reporting a panic: keep it short and stop.
        kernel_log().attach_console(nullptr);
        dmesgln("KERNEL PANIC while panicking: {}", reason);
        Arch::halt();
    }
    s_panicking = true;

    show_panic_console();
    dmesgln("vultureOS needs to restart: the kernel ran into a problem it cannot recover from.");
    dmesgln("");
    dmesgln("KERNEL PANIC: {}", reason);

    FixedStringBuilder<160> cpu;
    Arch::describe_cpu(cpu);
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
    dmesgln("System halted.");
    Arch::halt();
}

}
