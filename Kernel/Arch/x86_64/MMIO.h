/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// Device register access with single plain moves, and port I/O, which only this architecture has.

namespace Kernel::Arch {

inline u8 mmio_read8(FlatPtr address)
{
    u8 value;
    asm volatile("movb (%1), %0" : "=q"(value) : "r"(address) : "memory");
    return value;
}

inline void mmio_write8(FlatPtr address, u8 value)
{
    asm volatile("movb %0, (%1)" ::"q"(value), "r"(address) : "memory");
}

inline u16 mmio_read16(FlatPtr address)
{
    u16 value;
    asm volatile("movw (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
}

inline void mmio_write16(FlatPtr address, u16 value)
{
    asm volatile("movw %0, (%1)" ::"r"(value), "r"(address) : "memory");
}

inline u32 mmio_read32(FlatPtr address)
{
    u32 value;
    asm volatile("movl (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
}

inline void mmio_write32(FlatPtr address, u32 value)
{
    asm volatile("movl %0, (%1)" ::"r"(value), "r"(address) : "memory");
}

inline u64 mmio_read64(FlatPtr address)
{
    u64 value;
    asm volatile("movq (%1), %0" : "=r"(value) : "r"(address) : "memory");
    return value;
}

inline void mmio_write64(FlatPtr address, u64 value)
{
    asm volatile("movq %0, (%1)" ::"r"(value), "r"(address) : "memory");
}

inline u8 port_read8(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void port_write8(u16 port, u8 value)
{
    asm volatile("outb %0, %1" ::"a"(value), "Nd"(port));
}

inline u16 port_read16(u16 port)
{
    u16 value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void port_write16(u16 port, u16 value)
{
    asm volatile("outw %0, %1" ::"a"(value), "Nd"(port));
}

inline u32 port_read32(u16 port)
{
    u32 value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void port_write32(u16 port, u32 value)
{
    asm volatile("outl %0, %1" ::"a"(value), "Nd"(port));
}

inline u64 read_msr(u32 msr)
{
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<u64>(high) << 32) | low;
}

inline void write_msr(u32 msr, u64 value)
{
    asm volatile("wrmsr" ::"a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32)), "c"(msr) : "memory");
}

struct CpuidResult {
    u32 eax, ebx, ecx, edx;
};

inline CpuidResult cpuid(u32 leaf, u32 subleaf = 0)
{
    CpuidResult result;
    asm volatile("cpuid" : "=a"(result.eax), "=b"(result.ebx), "=c"(result.ecx), "=d"(result.edx) : "a"(leaf), "c"(subleaf));
    return result;
}

}
