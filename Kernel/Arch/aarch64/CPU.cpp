/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/BootInfo.h>
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
    case 0x0d:
        return "Branch target exception"_sv;
    case 0x1c:
        return "Pointer authentication failure"_sv;
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

u64 hardware_capabilities()
{
    constexpr u64 capability_aes = 1 << 3;
    constexpr u64 capability_pmull = 1 << 4;
    constexpr u64 capability_sha2 = 1 << 6;
    u64 isar0;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    u64 capabilities = 0;
    u64 const aes = (isar0 >> 4) & 0xf;
    if (aes >= 1)
        capabilities |= capability_aes;
    if (aes >= 2)
        capabilities |= capability_pmull;
    if (((isar0 >> 12) & 0xf) >= 1)
        capabilities |= capability_sha2;
    return capabilities | hardware_capabilities_extra();
}

namespace {

bool has_pan()
{
    return ((READ_SYSTEM_REGISTER(id_aa64mmfr1_el1) >> 20) & 0xf) != 0;
}

}

bool has_pointer_authentication()
{
    u64 const isar1 = READ_SYSTEM_REGISTER(id_aa64isar1_el1);
    // APA and API: address authentication, by one algorithm or another. (ISAR2's APA3 is the
    // newest; not every assembler knows that register by name.)
    u64 const isar2 = READ_SYSTEM_REGISTER(S3_0_C0_C6_2);
    return ((isar1 >> 4) & 0xf) != 0 || ((isar1 >> 8) & 0xf) != 0 || ((isar2 >> 12) & 0xf) != 0;
}

bool has_branch_target_identification()
{
    return (READ_SYSTEM_REGISTER(id_aa64pfr1_el1) & 0xf) != 0;
}

}

// Read by UserAccess.S: whether there is a PAN to step around.
extern "C" {
u8 aarch64_has_pan = 0;
}

namespace Kernel::Arch {

void enable_hardening_features()
{
    if (has_pan()) {
        // SPAN clear: PAN goes on whenever an exception brings the processor to this level.
        constexpr u64 sctlr_span = 1ull << 23;
        WRITE_SYSTEM_REGISTER(sctlr_el1, READ_SYSTEM_REGISTER(sctlr_el1) & ~sctlr_span);
        asm volatile("isb\n"
                     ".arch_extension pan\n"
                     "msr pan, #1\n");
        aarch64_has_pan = 1;
    }
}

void describe_hardening_features(FormatSink& sink)
{
    sink.write(has_pan() ? "PAN"_sv : "no PAN"_sv);
    sink.write(has_pointer_authentication() ? ", PAC"_sv : ", no PAC"_sv);
    sink.write(has_branch_target_identification() ? ", BTI"_sv : ", no BTI"_sv);
}

void set_user_pointer_key(u64 low, u64 high)
{
    if (!has_pointer_authentication())
        return;
    WRITE_SYSTEM_REGISTER(S3_0_C2_C1_0, low);  // APIAKeyLo_EL1
    WRITE_SYSTEM_REGISTER(S3_0_C2_C1_1, high); // APIAKeyHi_EL1
    asm volatile("isb");
}

u64 hardware_capabilities_extra()
{
    constexpr u64 capability_paca = 1ull << 30;
    constexpr u64 capability_bti = 1ull << 40;
    u64 capabilities = 0;
    if (has_pointer_authentication())
        capabilities |= capability_paca;
    if (has_branch_target_identification())
        capabilities |= capability_bti;
    return capabilities;
}

// Before anything has signed a return address: this function, and what called it, aren't signed.
// The key comes from what the loader got of the firmware's random numbers, mixed with the time:
// the same on every processor, and new at every boot. It isn't used by programs, which have
// the other key, one for each process.
extern "C" [[gnu::target("branch-protection=none"), gnu::no_stack_protector]] void aarch64_enable_pointer_authentication(API::BootInfo const* boot_info)
{
    if (!has_pointer_authentication())
        return;
    u64 words[2];
    __builtin_memcpy(words, boot_info->entropy + 32, sizeof(words));
    u64 const counter = READ_SYSTEM_REGISTER(cntpct_el0);
    WRITE_SYSTEM_REGISTER(S3_0_C2_C1_2, words[0] ^ (counter * 0x9e3779b97f4a7c15ull));
    WRITE_SYSTEM_REGISTER(S3_0_C2_C1_3, words[1] ^ (counter * 0xc2b2ae3d27d4eb4full));
    constexpr u64 sctlr_enable_a = 1ull << 31;
    constexpr u64 sctlr_enable_b = 1ull << 30;
    WRITE_SYSTEM_REGISTER(sctlr_el1, READ_SYSTEM_REGISTER(sctlr_el1) | sctlr_enable_a | sctlr_enable_b);
    asm volatile("isb");
}

}
