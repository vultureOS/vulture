# Boot Protocol

How VultureBoot starts the kernel. **Status: M0 implemented; M1 handoff is a
draft and lands with the kernel.**

## Firmware and loader

- vultureOS boots through UEFI. On QEMU this is edk2 (`edk2-aarch64-code.fd`,
  UEFI 2.7), started by `Meta/run.sh`.
- The loader is `\EFI\BOOT\BOOTAA64.EFI` on the EFI System Partition (FAT),
  the default path firmware tries on removable media. Until `Tools/mkimage`
  exists (M5), QEMU exposes `build/esp/` as a read-only FAT disk.
- VultureBoot is a PE32+ EFI application built from `Boot/`. Its UEFI
  definitions in `Boot/UEFI/UEFI.h` are written from the specification and
  checked against its offsets with `static_assert`.

### What the firmware provides (QEMU `virt`, as measured in M0)

| Item | Value |
|---|---|
| Hardware description | Device tree (with `-machine virt,acpi=off`); no ACPI tables |
| Framebuffer | `ramfb`: linear BGRX8, 800×600 by default |
| Memory | All of `-m 4G` usable after boot services exit, minus firmware reservations |

## M0 behavior

The loader disarms the firmware watchdog. It then prints the firmware vendor
and UEFI revision, usable memory, the framebuffer mode, and which
configuration tables exist (device tree, ACPI 2.0, SMBIOS 3). It prints the
marker `VultureBoot: ready` (which `ninja test-boot` waits for), then waits
for a key and powers off through `ResetSystem`.

## M1 handoff (draft)

### Files on the EFI System Partition

| Path | Contents |
|---|---|
| `\EFI\BOOT\BOOTAA64.EFI` | VultureBoot |
| `\System\Library\Kernels\vulture` | Kernel, ELF64 AArch64, linked for the higher half |
| `\System\Library\Kernels\vulture.bootarchive` | Boot archive (initial root filesystem) |
| `\System\Library\Kernels\boot.json` | Kernel command line and loader options |

### Loader steps

1. Read the kernel ELF and boot archive into memory, loading `PT_LOAD`
   segments at their physical placement.
2. Pick the best graphics mode and record the linear framebuffer.
3. Find the device tree (and the ACPI RSDP, if present) in the configuration
   tables.
4. Build the kernel's page tables:
   - TTBR1: the kernel image at `0xFFFF'FFFF'8000'0000`, plus a direct map of
     all physical memory at `0xFFFF'0000'0000'0000`.
   - TTBR0: an identity map of the loader's trampoline, removed by the kernel.
5. Get the final memory map and call `ExitBootServices`.
6. Switch to the new tables and jump to the kernel entry with interrupts
   masked (DAIF), at EL1, with `x0` holding the virtual address of `BootInfo`.

### `BootInfo` (version 1)

| Field | Meaning |
|---|---|
| `magic`, `version`, `size` | `"VBOOTINF"`, 1, structure size, for compatibility checks |
| `memory_map` | Our own entries: base, length, type (Usable, Reserved, AcpiReclaimable, AcpiNvs, Mmio, LoaderReclaimable, KernelImage, BootArchive, Framebuffer, RuntimeServices) |
| `framebuffer` | Physical base, width, height, pitch, pixel format |
| `kernel` | Physical base, virtual base, size |
| `boot_archive` | Physical base, size |
| `command_line` | Null-terminated, up to 256 bytes |
| `device_tree` | Physical address of the DTB (0 if none) |
| `acpi_rsdp` | Physical address of the RSDP (0 if none) |
| `uefi_runtime` | Runtime services table, for reset and variables |
| `direct_map_base` | Virtual address where physical memory is mapped |
| `entropy` | 64 bytes from `EFI_RNG_PROTOCOL` when available (for KASLR in M13) |
