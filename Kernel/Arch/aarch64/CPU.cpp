/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/CPU.h>

namespace Kernel::Arch {

void halt()
{
    asm volatile("msr daifset, #0xf");
    for (;;)
        asm volatile("wfi");
}

void disable_lower_half_translation()
{
    constexpr u64 tcr_epd0 = 1ull << 7;
    u64 tcr = READ_SYSTEM_REGISTER(tcr_el1) | tcr_epd0;
    asm volatile("msr tcr_el1, %0\n"
                 "isb\n"
                 "tlbi vmalle1\n"
                 "dsb nsh\n"
                 "isb" ::"r"(tcr)
        : "memory");
}

u64 monotonic_counter_frequency()
{
    return READ_SYSTEM_REGISTER(cntfrq_el0);
}

static StringView implementer_name(u64 implementer)
{
    switch (implementer) {
    case 0x41:
        return "Arm"_sv;
    case 0x42:
        return "Broadcom"_sv;
    case 0x4e:
        return "NVIDIA"_sv;
    case 0x51:
        return "Qualcomm"_sv;
    case 0x61:
        return "Apple"_sv;
    default:
        return "Unknown"_sv;
    }
}

void describe_cpu(FormatSink& sink)
{
    u64 midr = READ_SYSTEM_REGISTER(midr_el1);
    u64 exception_level = (READ_SYSTEM_REGISTER(CurrentEL) >> 2) & 3;
    format_to(sink, "{} core (implementer {:#04x}, part {:#05x}, revision r{}p{}), EL{}",
        implementer_name((midr >> 24) & 0xff), (midr >> 24) & 0xff, (midr >> 4) & 0xfff, (midr >> 20) & 0xf, midr & 0xf, exception_level);
}

static StringView exception_class_name(u64 exception_class)
{
    switch (exception_class) {
    case 0x00:
        return "Undefined instruction"_sv;
    case 0x01:
        return "Trapped WFI/WFE"_sv;
    case 0x07:
        return "Trapped FP/SIMD access"_sv;
    case 0x0e:
        return "Illegal execution state"_sv;
    case 0x15:
        return "System call"_sv;
    case 0x18:
        return "Trapped system register access"_sv;
    case 0x20:
    case 0x21:
        return "Instruction abort"_sv;
    case 0x22:
        return "PC alignment fault"_sv;
    case 0x24:
    case 0x25:
        return "Data abort"_sv;
    case 0x26:
        return "SP alignment fault"_sv;
    case 0x2f:
        return "SError"_sv;
    case 0x3c:
        return "Breakpoint instruction"_sv;
    default:
        return "Exception"_sv;
    }
}

static StringView fault_status_name(u64 status)
{
    switch (status >> 2) {
    case 0b0001:
        return "translation fault"_sv;
    case 0b0010:
        return "access flag fault"_sv;
    case 0b0011:
        return "permission fault"_sv;
    default:
        break;
    }
    switch (status) {
    case 0b000000:
    case 0b000001:
    case 0b000010:
    case 0b000011:
        return "address size fault"_sv;
    case 0b010000:
        return "synchronous external abort"_sv;
    case 0b100001:
        return "alignment fault"_sv;
    default:
        return "fault"_sv;
    }
}

void describe_exception(FormatSink& sink, TrapFrame const& frame, u64 vector_index)
{
    static constexpr StringView kinds[] = { "synchronous"_sv, "IRQ"_sv, "FIQ"_sv, "SError"_sv };
    if (vector_index % 4 != 0) {
        format_to(sink, "Unexpected {} exception", kinds[vector_index % 4]);
        return;
    }

    u64 exception_class = (frame.esr >> 26) & 0x3f;
    format_to(sink, "{}", exception_class_name(exception_class));
    bool is_abort = exception_class == 0x20 || exception_class == 0x21 || exception_class == 0x24 || exception_class == 0x25;
    if (is_abort) {
        u64 status = frame.esr & 0x3f;
        bool is_write = (exception_class & ~1ull) == 0x24 && (frame.esr & (1ull << 6));
        if ((exception_class & ~1ull) == 0x24)
            format_to(sink, " ({})", is_write ? "write"_sv : "read"_sv);
        format_to(sink, " at {:#018x}: {}", frame.far, fault_status_name(status));
        if ((status >> 2) >= 0b0001 && (status >> 2) <= 0b0011)
            format_to(sink, ", level {}", status & 3);
    } else {
        format_to(sink, " (ESR {:#010x})", frame.esr);
    }
}

void dump_registers(FormatSink& sink, TrapFrame const& frame)
{
    for (size_t i = 0; i < 31; ++i) {
        format_to(sink, "  x{:<2} {:#018x}", i, frame.x[i]);
        if (i % 4 == 3 || i == 30)
            sink.write("\n"_sv);
    }
    format_to(sink, "  sp  {:#018x}  pc  {:#018x}  far {:#018x}\n", frame.sp, frame.elr, frame.far);
    format_to(sink, "  esr {:#010x}  spsr {:#010x}\n", frame.esr, frame.spsr);
}

}
