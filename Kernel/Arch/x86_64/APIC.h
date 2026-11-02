/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Memory/PhysicalAddress.h>
#include <LibBase/Error.h>

namespace Kernel::X86_64 {

// A vector to give a device for its message interrupts, and what to program it with to raise it.
[[nodiscard]] ErrorOr<u32> allocate_message_vector();
[[nodiscard]] u64 message_address();
[[nodiscard]] u32 message_data(u32 vector);

// A vector for an old ISA interrupt, by way of the I/O APIC (masked until enable_interrupt).
[[nodiscard]] ErrorOr<u32> allocate_isa_vector(u8 isa_interrupt);

// The starting of processors: INIT, then startup at this page (below 1 MiB).
void send_init_ipi(u32 apic_id);
void send_startup_ipi(u32 apic_id, u64 page);

}

namespace Kernel::Arch {

// The pages below 1 MiB that the loader kept (BootInfo.low_memory).
void set_low_memory(PhysicalAddress);

}
