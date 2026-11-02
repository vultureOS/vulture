/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/BootInfo.h>
#include <Kernel/Arch/CPU.h>
#include <Kernel/Arch/x86_64/MMIO.h>
#include <Kernel/Arch/x86_64/ProcessorArea.h>
#include <Kernel/Tasks/Processor.h>

extern "C" u8 x86_64_interrupt_stubs[];
extern "C" u8 x86_64_syscall_entry[];
extern "C" u8 x86_64_stop_processor[];

// Read by UserAccess.S: whether there is a SMAP to step around.
extern "C" {
u8 x86_64_has_smap = 0;
}

namespace Kernel::X86_64 {

namespace {

constexpr u32 msr_efer = 0xc000'0080;
constexpr u32 msr_star = 0xc000'0081;
constexpr u32 msr_lstar = 0xc000'0082;
constexpr u32 msr_fmask = 0xc000'0084;
constexpr u32 msr_fs_base = 0xc000'0100;
constexpr u32 msr_gs_base = 0xc000'0101;
constexpr u32 msr_kernel_gs_base = 0xc000'0102;
constexpr u32 msr_pat = 0x277;

constexpr u64 efer_syscall = 1 << 0;
constexpr u64 efer_no_execute = 1 << 11;

constexpr u64 cr0_write_protect = 1 << 16;
constexpr u64 cr0_emulation = 1 << 2;
constexpr u64 cr0_monitor_coprocessor = 1 << 1;
constexpr u64 cr0_task_switched = 1 << 3;
constexpr u64 cr4_global_pages = 1 << 7;
constexpr u64 cr4_fxsave = 1 << 9;
constexpr u64 cr4_simd_exceptions = 1 << 10;
constexpr u64 cr4_smep = 1 << 20;
constexpr u64 cr4_smap = 1 << 21;

constexpr u64 rflags_trap = 1 << 8;
constexpr u64 rflags_direction = 1 << 10;
constexpr u64 rflags_nested_task = 1 << 14;
constexpr u64 rflags_alignment_check = 1 << 18;

constexpr u32 ist_nmi = 1;
constexpr u32 ist_double_fault = 2;

struct [[gnu::packed]] DescriptorPointer {
    u16 limit;
    u64 base;
};

struct InterruptGate {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 attributes;
    u16 offset_middle;
    u32 offset_high;
    u32 reserved;
};
static_assert(sizeof(InterruptGate) == 16);

alignas(16) InterruptGate s_idt[256];
bool s_idt_ready = false;
ProcessorArea s_areas[Processor::max_count];

u64 read_cr0()
{
    u64 value;
    asm volatile("movq %%cr0, %0" : "=r"(value));
    return value;
}

u64 read_cr4()
{
    u64 value;
    asm volatile("movq %%cr4, %0" : "=r"(value));
    return value;
}

void write_cr0(u64 value)
{
    asm volatile("movq %0, %%cr0" ::"r"(value) : "memory");
}

void write_cr4(u64 value)
{
    asm volatile("movq %0, %%cr4" ::"r"(value) : "memory");
}

void set_gate(u32 vector, void const* handler, u8 ist)
{
    auto const address = reinterpret_cast<u64>(handler);
    s_idt[vector] = {
        .offset_low = static_cast<u16>(address),
        .selector = selector_kernel_code,
        .ist = ist,
        .attributes = 0x8e, // Present, in the kernel, an interrupt gate: entered with interrupts off.
        .offset_middle = static_cast<u16>(address >> 16),
        .offset_high = static_cast<u32>(address >> 32),
        .reserved = 0,
    };
}

void build_interrupt_table()
{
    for (u32 vector = 0; vector < 256; ++vector)
        set_gate(vector, x86_64_interrupt_stubs + vector * 16, 0);
    // A panic stops the other processors with this, whatever they are doing.
    set_gate(2, x86_64_stop_processor, ist_nmi);
    // A kernel stack that has overflowed is no place to report that.
    set_gate(8, x86_64_interrupt_stubs + 8 * 16, ist_double_fault);
    s_idt_ready = true;
}

void load_segments(ProcessorArea& area)
{
    area.tss = {};
    area.tss.io_bitmap_offset = sizeof(TaskStateSegment);
    area.tss.ist[ist_nmi - 1] = reinterpret_cast<u64>(area.nmi_stack) + sizeof(area.nmi_stack);
    area.tss.ist[ist_double_fault - 1] = reinterpret_cast<u64>(area.double_fault_stack) + sizeof(area.double_fault_stack);
    area.tss_pointer = &area.tss;

    area.gdt[0] = 0;
    area.gdt[1] = 0x00af'9a00'0000'ffff; // Kernel code.
    area.gdt[2] = 0x00cf'9200'0000'ffff; // Kernel data.
    area.gdt[3] = 0x00cf'f200'0000'ffff; // User data.
    area.gdt[4] = 0x00af'fa00'0000'ffff; // User code.
    auto const tss = reinterpret_cast<u64>(&area.tss);
    constexpr u64 tss_limit = sizeof(TaskStateSegment) - 1;
    area.gdt[5] = tss_limit | ((tss & 0xff'ffff) << 16) | (0x89ull << 40) | (((tss >> 24) & 0xff) << 56);
    area.gdt[6] = tss >> 32;

    DescriptorPointer const gdt { sizeof(area.gdt) - 1, reinterpret_cast<u64>(area.gdt) };
    asm volatile("lgdtq %0\n"
                 "pushq %1\n"
                 "leaq 1f(%%rip), %%rax\n"
                 "pushq %%rax\n"
                 "lretq\n"
                 "1:\n"
                 "movl %2, %%eax\n"
                 "movl %%eax, %%ds\n"
                 "movl %%eax, %%es\n"
                 "movl %%eax, %%ss\n"
                 "xorl %%eax, %%eax\n"
                 "movl %%eax, %%fs\n"
                 "movl %%eax, %%gs\n"
                 ::"m"(gdt),
        "i"(static_cast<u64>(selector_kernel_code)), "i"(static_cast<u32>(selector_kernel_data))
        : "rax", "memory");
    asm volatile("ltr %w0" ::"r"(selector_tss));

    DescriptorPointer const idt { sizeof(s_idt) - 1, reinterpret_cast<u64>(s_idt) };
    asm volatile("lidtq %0" ::"m"(idt));
}

}

ProcessorArea& area_at(u32 index)
{
    return s_areas[index];
}

void initialize_this_processor(ProcessorArea& area)
{
    if (!s_idt_ready)
        build_interrupt_table();
    load_segments(area);

    // Segment loads clear these bases, so this comes after.
    Arch::write_msr(msr_gs_base, reinterpret_cast<u64>(&area));
    Arch::write_msr(msr_kernel_gs_base, 0);
    Arch::write_msr(msr_fs_base, 0);

    u64 const apic_leaf = Arch::cpuid(0).eax >= 0xb ? Arch::cpuid(0xb).edx : 0;
    area.apic_id = apic_leaf ? apic_leaf : Arch::cpuid(1).ebx >> 24;

    // SSE for programs, with no lazy tricks: Entry.S saves it at every switch.
    write_cr0((read_cr0() | cr0_write_protect | cr0_monitor_coprocessor) & ~(cr0_emulation | cr0_task_switched));
    write_cr4(read_cr4() | cr4_global_pages | cr4_fxsave | cr4_simd_exceptions);
    asm volatile("fninit");

    // Index 1 of the memory types becomes write-combining, for framebuffers. The others stay as they
    // come from reset: write-back, (then this), uncached-minus, uncached. PageTable.cpp relies on these.
    constexpr u64 pat = 0x0007'0406'0007'0106ull;
    Arch::write_msr(msr_pat, pat);

    Arch::write_msr(msr_efer, Arch::read_msr(msr_efer) | efer_syscall | efer_no_execute);
    // The selector the syscall instruction loads, and the one sysret works down from. (sysret isn't
    // used, but this is also what makes syscall itself valid.)
    Arch::write_msr(msr_star, (static_cast<u64>(selector_kernel_data) << 48) | (static_cast<u64>(selector_kernel_code) << 32));
    Arch::write_msr(msr_lstar, reinterpret_cast<u64>(x86_64_syscall_entry));
    Arch::write_msr(msr_fmask, Arch::rflags_interrupt_enable | rflags_direction | rflags_trap | rflags_nested_task | rflags_alignment_check);
}

}

extern "C" [[gnu::no_stack_protector]] void x86_64_initialize_boot_processor()
{
    Kernel::X86_64::initialize_this_processor(Kernel::X86_64::area_at(0));
}

namespace Kernel::Arch {

StringView machine_name()
{
    return "x86_64"_sv;
}

StringView architecture_name()
{
    return "x86_64"_sv;
}

bool is_emulated_in_software()
{
    constexpr u32 feature_hypervisor = 1u << 31;
    if (!(cpuid(1).ecx & feature_hypervisor))
        return false;
    auto const leaf = cpuid(0x4000'0000);
    constexpr u32 tcg = 0x54474354; // "TCGT"
    return leaf.ebx == tcg;
}

void halt()
{
    asm volatile("cli");
    for (;;)
        asm volatile("hlt");
}

void disable_lower_half_translation()
{
    // The loader's tables, of which this half only held its own way into the kernel.
    u64 root;
    asm volatile("movq %%cr3, %0" : "=r"(root));
    auto* table = reinterpret_cast<u64*>(API::direct_map_base + (root & 0x000f'ffff'ffff'f000));
    for (size_t i = 0; i < 256; ++i)
        table[i] = 0;
    asm volatile("movq %0, %%cr3" ::"r"(root) : "memory");
}

void describe_cpu(FormatSink& sink)
{
    auto const extended = cpuid(0x8000'0000).eax;
    char name[49] {};
    if (extended >= 0x8000'0004) {
        for (u32 i = 0; i < 3; ++i) {
            auto const leaf = cpuid(0x8000'0002 + i);
            __builtin_memcpy(name + i * 16, &leaf, 16);
        }
        char const* start = name;
        while (*start == ' ')
            ++start;
        size_t length = 0;
        while (start[length])
            ++length;
        while (length > 0 && start[length - 1] == ' ')
            --length;
        format_to(sink, "{}, ", StringView(start, length));
    }
    auto const leaf = cpuid(1);
    format_to(sink, "family {:#x}, model {:#x}, stepping {}", ((leaf.eax >> 8) & 0xf) + ((leaf.eax >> 20) & 0xff),
        ((leaf.eax >> 4) & 0xf) | ((leaf.eax >> 12) & 0xf0), leaf.eax & 0xf);
}

static StringView vector_name(u64 vector)
{
    switch (vector) {
    case 0:
        return "Division by zero"_sv;
    case 1:
        return "Debug trap"_sv;
    case 3:
        return "Breakpoint instruction"_sv;
    case 4:
        return "Overflow"_sv;
    case 5:
        return "Bound range exceeded"_sv;
    case 6:
        return "Invalid instruction"_sv;
    case 7:
        return "No floating-point unit"_sv;
    case 8:
        return "Double fault"_sv;
    case 10:
        return "Invalid task state"_sv;
    case 11:
        return "Segment not present"_sv;
    case 12:
        return "Stack fault"_sv;
    case 13:
        return "General protection fault"_sv;
    case 14:
        return "Page fault"_sv;
    case 16:
        return "Floating-point error"_sv;
    case 17:
        return "Alignment check"_sv;
    case 18:
        return "Machine check"_sv;
    case 19:
        return "SIMD floating-point error"_sv;
    default:
        return "Exception"_sv;
    }
}

void describe_exception(FormatSink& sink, TrapFrame const& frame, u64)
{
    format_to(sink, "{}", vector_name(frame.vector));
    if (frame.vector == 14) {
        bool const write = frame.error_code & (1 << 1);
        bool const fetch = frame.error_code & (1 << 4);
        format_to(sink, " ({}) at {:#018x}: {}", fetch ? "execute"_sv : (write ? "write"_sv : "read"_sv), frame.cr2,
            (frame.error_code & 1) ? "permission fault"_sv : "not mapped"_sv);
    } else {
        format_to(sink, " (vector {}, error {:#x})", frame.vector, frame.error_code);
    }
}

void dump_registers(FormatSink& sink, TrapFrame const& frame)
{
    format_to(sink, "  rax {:#018x}  rbx {:#018x}  rcx {:#018x}  rdx {:#018x}\n", frame.rax, frame.rbx, frame.rcx, frame.rdx);
    format_to(sink, "  rsi {:#018x}  rdi {:#018x}  rbp {:#018x}  r8  {:#018x}\n", frame.rsi, frame.rdi, frame.rbp, frame.r8);
    format_to(sink, "  r9  {:#018x}  r10 {:#018x}  r11 {:#018x}  r12 {:#018x}\n", frame.r9, frame.r10, frame.r11, frame.r12);
    format_to(sink, "  r13 {:#018x}  r14 {:#018x}  r15 {:#018x}\n", frame.r13, frame.r14, frame.r15);
    format_to(sink, "  rsp {:#018x}  rip {:#018x}  cr2 {:#018x}\n", frame.rsp, frame.rip, frame.cr2);
    format_to(sink, "  cs {:#x}  ss {:#x}  rflags {:#010x}  error {:#x}\n", frame.cs, frame.ss, frame.rflags, frame.error_code);
}

u64 hardware_capabilities()
{
    return cpuid(1).edx;
}

static bool has_smep()
{
    return cpuid(0).eax >= 7 && (cpuid(7).ebx & (1 << 7));
}

static bool has_smap()
{
    return cpuid(0).eax >= 7 && (cpuid(7).ebx & (1 << 20));
}

bool has_pointer_authentication()
{
    return false;
}

bool has_branch_target_identification()
{
    return false;
}

void enable_hardening_features()
{
    u64 cr4 = X86_64::read_cr4();
    if (has_smep())
        cr4 |= X86_64::cr4_smep;
    if (has_smap()) {
        cr4 |= X86_64::cr4_smap;
        x86_64_has_smap = 1;
    }
    X86_64::write_cr4(cr4);
}

void describe_hardening_features(FormatSink& sink)
{
    sink.write(has_smap() ? "SMAP"_sv : "no SMAP"_sv);
    sink.write(has_smep() ? ", SMEP"_sv : ", no SMEP"_sv);
}

void set_user_pointer_key(u64, u64)
{
}

u64 hardware_capabilities_extra()
{
    return 0;
}

}
