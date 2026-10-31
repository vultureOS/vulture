# Boot Protocol

How VultureBoot starts the kernel. Implemented in M1: `Boot/` is the loader,
`Kernel/API/BootInfo.h` is the contract, and `Kernel/Arch/aarch64/Boot.S` is
the kernel's side of the handoff.

## Firmware and loader

- vultureOS boots through UEFI. On QEMU this is edk2 (`edk2-aarch64-code.fd`,
  UEFI 2.7), started by `Meta/run.sh`.
- The loader is `\EFI\BOOT\BOOTAA64.EFI` on the EFI System Partition (FAT),
  the default path firmware tries on removable media. Until `Tools/mkimage`
  exists (M5), QEMU exposes `build/esp/` as a read-only FAT disk.
- VultureBoot is a PE32+ EFI application built from `Boot/`. Its UEFI
  definitions (`Boot/UEFI/UEFI.h`) are written from the specification and
  checked against its offsets with `static_assert`.
- It requires EL1 and 4 KiB translation granules. Entry at EL2 (some real
  machines) is reported as unsupported for now; it comes with M14.

### What QEMU's firmware provides (measured)

| Item | Value |
|---|---|
| Hardware description | Device tree (with `-machine virt,acpi=off`); no ACPI tables |
| Framebuffer | `ramfb`: linear BGRX8, up to 1024×768, inside a "reserved" RAM region |
| Serial | PL011 at `0x09000000`, named by `/chosen/stdout-path` |
| Entropy | `EFI_RNG_PROTOCOL`, backed by `-device virtio-rng-pci` |

## Files on the EFI System Partition

| Path | Contents | Required |
|---|---|---|
| `\EFI\BOOT\BOOTAA64.EFI` | VultureBoot | yes |
| `\System\Library\Kernels\vulture` | The kernel: ELF64, AArch64, `ET_EXEC`, linked for the upper half | yes |
| `\System\Library\Kernels\vulture.bootarchive` | Boot archive (initial root filesystem, from M4) | no |
| `\System\Library\Kernels\boot-args` | Kernel command line as plain text, like macOS's `boot-args` | no |

`Meta/run.sh --boot-args "..."` writes the `boot-args` file.

## What the loader does

1. Reads the kernel file and checks it: AArch64, a static executable, every
   loadable segment page-aligned and in the upper half, the entry point in an
   executable segment.
2. Copies the segments into one physically contiguous block (laid out as
   their virtual addresses are; `.bss` zeroed). The ELF file stays in memory
   too, for the kernel's symbol table.
3. Reads the optional boot archive and `boot-args`.
4. Copies the device tree into memory it owns, and finds the serial port in
   it (`stdout-path`, otherwise the first `arm,pl011`). Records the ACPI RSDP
   if there is one, and 64 bytes from the firmware RNG if it has one.
5. Picks the largest graphics mode with a linear 32-bit framebuffer, prints a
   summary, and only then switches to that mode (see "Findings" in the
   Roadmap).
6. Builds the kernel's page tables (below), then calls `ExitBootServices`.
   On a stale map key it refreshes the map and retries, as the specification
   requires.
7. Converts the final memory map, writes caches back to memory, and jumps to
   the kernel.

### Memory types

The loader tags its own allocations with types from the range UEFI reserves
for OS loaders, so the final memory map tells the kernel what each is:

| UEFI type | Region in `BootInfo` | Contents |
|---|---|---|
| `0x80000000` | `KernelImage` | Loaded kernel segments |
| `0x80000001` | `KernelFile` | The kernel ELF file (symbols) |
| `0x80000002` | `BootArchive` | The boot archive |
| `0x80000003` | `LoaderReclaimable` | `BootInfo`, the memory map array, the device tree copy, `boot-args`, page tables |

Firmware types map as follows:

- Conventional memory, boot services code and data, and loader code and data
  become `Usable`.
- Runtime services, ACPI and MMIO keep their meaning.
- Everything else is `Reserved`.

The map is sorted by address, with adjacent regions of the same type merged.

The kernel copies `BootInfo`, the memory map and the device tree early on,
switches to page tables of its own, and then hands every `LoaderReclaimable`
region to its page allocator. Nothing the loader allocated with that type
may be needed after the handoff.

### Page tables (4 KiB granule, 48-bit addresses)

| Range | Maps | Attributes |
|---|---|---|
| `0xffff'0000'0000'0000 + P` (direct map) | Every memory-map region except `Reserved` | Normal write-back, read-write, never executable; MMIO regions as Device-nGnRE |
| Direct map | The framebuffer, if not already covered | Normal write-back if it is in RAM, Normal non-cacheable otherwise |
| Direct map | The serial port's page | Device-nGnRE |
| The kernel's link addresses (`0xffffffff80000000` and up) | Each segment | Code read-execute, constants read-only, data read-write and never executable |
| Lower half (TTBR0), identity | The two pages holding the handoff trampoline | Normal, read-execute |

2 MiB blocks are used wherever alignment allows; 4 KiB pages elsewhere.
`Reserved` memory is never mapped wholesale: on real machines it can be memory
that faults when touched, even by speculation.

MAIR_EL1 layout, shared with the kernel (`Kernel/Arch/aarch64/PageTables.h`):
index 0 = Normal write-back (`0xff`), 1 = Device-nGnRE (`0x04`), 2 = Normal
non-cacheable (`0x44`).

## The handoff

The trampoline runs from its identity-mapped physical address:

1. Masks interrupts.
2. Turns the MMU and data cache off. From here it touches registers only.
3. Installs MAIR, TCR, TTBR0 and TTBR1.
4. Invalidates the TLB.
5. Turns the MMU and caches back on, and branches to the kernel entry.

Before that, the loader cleans the kernel image, page tables and `BootInfo`
to the point of coherency, and synchronizes the instruction cache for the
copied code.

### State on kernel entry

- EL1, MMU on with the tables above, interrupts masked (DAIF set).
- `x0` = virtual address of `BootInfo` (in the direct map).
- No usable stack: the kernel's `_start` installs its exception vectors, sets
  `sp` to its own boot stack, clears the frame pointer and calls
  `kernel_main`.
- The kernel then turns off lower-half translation (TCR_EL1.EPD0), so null
  and other low addresses fault.

## `BootInfo` (version 1)

All fields are fixed-width, because the PE loader and the ELF kernel must lay
the structure out identically; `static_assert(sizeof(BootInfo) == 496)` pins
it. Addresses are physical unless named otherwise.

| Field | Meaning |
|---|---|
| `magic`, `version`, `size` | `"VBOOTINF"`, 1, 496; the kernel halts on a mismatch |
| `memory_map`, `memory_map_entries` | The `MemoryRegion` array (base, length, type) |
| `kernel_image`, `kernel_virtual_base` | Where the segments are, and the lowest segment's address |
| `kernel_file` | The ELF file, for symbolizing backtraces |
| `boot_archive` | Empty when there is none |
| `device_tree` | The loader's copy; empty when the firmware has none |
| `acpi_rsdp` | 0 when there is none |
| `uefi_system_table` | For runtime services later |
| `framebuffer` | Base, size, width, height, pitch, format (`None` without a linear framebuffer) |
| `serial` | Base and type (`PL011`, or `None`) |
| `entropy_size`, `entropy` | Up to 64 random bytes (for KASLR in M13) |
| `command_line` | Null-terminated contents of `boot-args`, at most 255 characters |

## Kernel command line

| Argument | Effect |
|---|---|
| `-v` | Verbose boot: the kernel log on screen instead of the boot screen |
| `ktest`, `ktest=PREFIX` | Run the in-kernel tests (all, or those whose `suite.name` starts with PREFIX), print `ktest: PASS` or `ktest: FAIL`, and power off |
| `fault=KIND` | End the boot with a deliberate failure, to test crash reporting: `data-abort`, `trap`, `verify`, `panic`, `interrupt` (a panic in a timer interrupt) or `processor` (a panic on processor 1) |
