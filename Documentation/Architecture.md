# Architecture

## Goals

- A Unix operating system that looks and feels like macOS (Sonoma/Sequoia
  era): global menu bar, Dock, Finder, translucent materials, rounded
  windows with traffic lights.
- **Everything inside the disk image is our own code**: bootloader, kernel,
  C library, C++ runtime, graphics, font rasterizer, UI framework, window
  server, desktop and apps.
- Fast by design: per-CPU kernel structures, zero-copy IPC, damage-based
  compositing, quality-of-service scheduling (see [Roadmap](Roadmap.md) for
  the budgets).

## Decisions

| Decision | Choice |
|---|---|
| Language | C++23 (clang), freestanding, no exceptions, no RTTI, plus a thin layer of assembly |
| Toolchain | Homebrew `llvm@22` (clang), `lld@22` (ld.lld, lld-link), CMake + Ninja, QEMU + edk2 |
| First architecture | AArch64 on QEMU `virt` with HVF; x86_64 follows in M14 |
| Look | macOS Sonoma/Sequoia; see [Design System](DesignSystem.md) |
| Assets | Inter (SIL OFL) as the system font, JetBrains Mono (OFL) for Terminal; all icons, wallpapers and sounds original |

### The zero-external-code rule

Nothing in the image comes from outside this repository. Allowed on the host
only: the compiler and linker, CMake/Ninja, QEMU with its edk2 firmware, and
git. Data files are allowed: fonts (OFL, licenses shipped next to them),
Unicode Character Database tables (converted by our own generator) and TLS
root certificates.

In practice:

- Every part builds with `-nostdinc -nostdinc++`: no system headers, no C++
  standard library. LibBase provides the vocabulary types instead.
- Freestanding parts link `LibBaseFreestanding`, which supplies the few
  functions the compiler may call by itself (`memcpy`, `memmove`, `memset`,
  `memcmp`).
- Host unit tests link only libSystem's C functions (`malloc`, `write`,
  `abort`); LibTest supplies `operator new`/`delete` and `__cxa_pure_virtual`.
- SIMD code uses clang vector extensions (`ext_vector_type`), which compile
  to NEON or SSE from one source, instead of vendor intrinsic headers.

## Build

The tree builds in three **parts**, each with its own toolchain file in
`Meta/CMake/Toolchain/`:

| Part | Toolchain | Output |
|---|---|---|
| `host` | `host.cmake`, macOS clang | Unit tests and host tools, built with ASan and UBSan |
| `uefi` | `vulture-uefi-aarch64.cmake`, `aarch64-w64-windows-gnu` + lld-link | `BOOTAA64.EFI` (PE32+) installed into `build/esp/EFI/BOOT/` |
| `target` | `vulture-aarch64.cmake`, `aarch64-unknown-none-elf` + ld.lld | The kernel, installed into `build/esp/System/Library/Kernels/` |
| `userland` | `vulture-aarch64.cmake` with user flags | LibC, LibCxxRT, programs and tests, installed into the root filesystem staging tree `build/root/` |

After the parts, `boot-archive` packs `build/root/` with the host tool
`mkarchive` into `build/esp/System/Library/Kernels/vulture.bootarchive`.

The top-level `CMakeLists.txt` configured without `VULTURE_PART` is the
**superbuild** (`Meta/CMake/Superbuild.cmake`). It runs each part as an
ExternalProject over the same source tree and adds these targets:

| Target | What it does |
|---|---|
| `ninja -C build` | Builds every part and merges `build/compile_commands.json` for editors |
| `ninja -C build run` | Boots in QEMU with a window (`Meta/run.sh`) |
| `ninja -C build test-host` | Runs host unit tests through CTest |
| `ninja -C build test-boot` | Boots headless; the kernel must print its memory map and finish booting |
| `ninja -C build test-panic` | Boots with `fault=data-abort`; the panic report must decode the fault and symbolize the backtrace |
| `ninja -C build test-kernel` | Boots with `ktest`: the in-kernel tests run on all processors, print `ktest: PASS`, and power the machine off |
| `ninja -C build test-userland` | Boots with `utest`: `/sbin/init --test` runs the programs in `/usr/tests`, prints `userland: PASS`, and powers off |
| `ninja -C build check` | All of the above tests |
| `ninja -C build test-soak` | 1000 threads ping-ponging for 60 s (not part of `check`) |
| `ninja -C build bench` | Boots with `ktest=bench` and prints the measurements |

`Meta/build.sh` wraps the configure step and `ninja`, checks that the tools
are installed, and takes ninja targets as arguments (`--check` adds `check`).
`Meta/run.sh` also takes `--boot-args` (the kernel command line, for example
`-v` for a verbose boot) and, with `--headless`, `--screenshot FILE` to save
the screen as PNG and `--wait-for-exit` to require that the guest powers
itself off.

Kernel command line words:

| Word | Effect |
|---|---|
| `-v` | Verbose boot: the kernel log on screen instead of the boot screen |
| `ktest`, `ktest=PREFIX` | Run the in-kernel tests (those whose `suite.name` starts with PREFIX), then power off |
| `ktest-seconds=N` | How long soak tests run (default 2) |
| `utest` | Start `/sbin/init --test`, which runs the userland tests and powers off |
| `fault=KIND` | End the boot with a deliberate crash: `data-abort`, `trap`, `verify`, `panic`, `interrupt` (in a timer interrupt) or `processor` (on processor 1) | Kernel code that does not touch hardware (such as
`Kernel/BootArguments.cpp`) is compiled into host tests too; see
`Tests/Kernel`.

UEFI uses PE/COFF, so the loader compiles for a Windows target. We use the
GNU flavor (`windows-gnu`) because it keeps the Itanium C++ ABI the rest of
vultureOS uses. With the MSVC flavor, CMake would also drop `--target` and
header dependency tracking.

## System layers

```
 Apps         Finder · Terminal · TextEdit · Preview · Settings · Activity Monitor · Software Center …
 Desktop      LoginWindow · SystemUI (menu bar) · Dock · Spotlight · Mission Control · Notification/Control Center
 Frameworks   pranaUI (AppKit-like) · LibAnimation · LibGfx (Quartz-2D-like) · LibText (CoreText-like) · LibImage
 Core libs    LibCore (run loop, JSON plists) · LibIPC (+ipcc IDL) · LibCrypto/TLS/HTTP · LibUnicode · LibVT · LibAudio
 Runtime      LibC · LibM · LibCxxRT · LibSystem (syscalls) · DynamicLoader · LibBase (shared with kernel)
 Services     vlaunchd (PID 1) · logd · notifyd · configd · PranaWindowServer · AudioServer · Indexer · Pasteboard
 ─────────────────────────────── syscalls (SVC) + Mach-style ports ───────────────────────────────
 Kernel       Hybrid (XNU-style): monolithic core for speed + port IPC for services
              Memory · Tasks/Scheduler · VFS (vultureFS, FAT32, tmpfs, devfs) · Net (TCP/IP) · Security · DeviceKit drivers
 Arch         aarch64 (EL1, GICv3, generic timer, PSCI) · x86_64 later
 Boot         VultureBoot (our UEFI loader) → BootInfo → kernel
```

## Repository layout

Directories appear as their milestone lands; nothing is created ahead of time
as a stub.

```
Meta/          build system (CMake parts, toolchains), run.sh, CI
Tools/         host C++ tools: mkarchive (M4); ipcc, mkimage, mkvfs, fsck.vfs, imgdiff, generators later
Boot/          VultureBoot UEFI loader: Arch/aarch64 (page tables, handoff), UEFI definitions
Kernel/        API (BootInfo) · Arch (interfaces; aarch64: entry, vectors, page tables, GICv3, timer, PSCI, SMP)
               Console (framebuffer, font, boot screen) · Debug (log, panic, symbols) · Devices/Serial
               Interrupts · Locking · Memory (buddy, physical memory, heap, memory manager)
               Tasks (Processor, Thread, Scheduler, Process) · Syscalls · FileSystem (boot archive)
               Time (timer queues) · Tests (ktest) · Library (C++ runtime) · Init.cpp
Libraries/     LibBase · LibArchive · LibC · LibCxxRT · LibSystem · LibDeviceTree · LibELF · LibTest
               LibCore, LibGfx, LibUI (pranaUI) … later
Services/      Init (process 1, M4); vlaunchd · logd · PranaWindowServer · Dock …                    (M6+)
Applications/  Finder · Terminal · TextEdit · Preview · SystemSettings · WidgetGallery …              (M9+)
Utilities/     vsh and command-line tools                                                             (M6+)
Base/          root filesystem skeleton, themes, fonts, icons, wallpapers, sounds                    (M5+)
Tests/         unit, integration and UI golden tests
Documentation/ these documents
```

### On-disk hierarchy

macOS-style at the top, real Unix underneath:

- `/Applications/*.app`, `/System/Library/{Kernels,CoreServices,Frameworks,LaunchDaemons,LaunchAgents,Fonts,Themes,Sounds,Desktop Pictures}`, `/Library`, `/Users/<name>/{Desktop,Documents,Downloads,Library}`, `/Volumes`
- `/bin`, `/sbin`, `/usr/{bin,lib,share/man}`, `/private/{etc,var,tmp}` with symlinks at `/etc`, `/var` and `/tmp`, and `/dev` (devfs)
- App bundles: `Name.app/Contents/{Info.json, Executable/, Resources/}`
- The System volume is read-only and sealed; user data lives on the Data volume.

## Kernel design

- **Boot.** VultureBoot hands the kernel a versioned `BootInfo`; see [Boot Protocol](BootProtocol.md).
- **Arch boundary.** Generic kernel code includes only `Kernel/Arch/*.h`
  (`CPU.h`, `TrapFrame.h`, `PageTable.h`, `MemoryLayout.h`,
  `InterruptController.h`, `Timer.h`, `Power.h`, `SMP.h`); each architecture
  implements them in `Kernel/Arch/<arch>/`.
- **Address space (M2).** The kernel builds its own tables at boot, replacing
  the loader's, then reclaims the loader's memory. Kernel half on AArch64:

  | Range | Contents |
  |---|---|
  | `0xffff'0000'0000'0000` | Direct map of all RAM (128 TiB window), 1 GiB/2 MiB blocks where possible |
  | `0xffff'8000'0000'0000` | MMIO window: device mappings, an unmapped page after each |
  | `0xffff'8100'0000'0000` | Kernel stacks: 64 KiB each at the top of a 128 KiB slot; the rest is a guard |
  | `0xffff'ffff'8000'0000` | Kernel image: code read-execute, constants read-only, data read-write |
- **Processors (M2).** Every CPU has a `Processor` object, reached through
  TPIDR_EL1, holding its page cache, heap magazines, timer queue, run queue
  and current thread. Secondary processors start through PSCI `CPU_ON` into
  `secondary_entry`, which turns the MMU on under a temporary identity map.
  `Processor::call_on_all` runs a short function on every processor in
  interrupt context (an IPI); `Processor::run_on_all` runs one in a thread
  pinned to each processor.
- **Interrupts and time (M2).** GICv3: SGIs are IPIs, PPIs per-processor
  interrupts, SPIs devices, all at one priority so handlers never nest. Each
  processor has a deadline-ordered timer queue driving the virtual timer
  one-shot for the earliest deadline only; there is no periodic tick.
- **Crash reports (M1).** Every exception lands in `handle_exception`. A
  panic switches the screen to a text console and prints the decoded fault,
  the registers and a backtrace. The backtrace follows frame pointers (kept in
  every function) and is symbolized from the kernel's own ELF file, with C++
  names demangled by LibELF.
- **Memory.** Buddy page allocator (M2: zones of contiguous RAM, 2 bytes
  of state per page, free lists inside free memory) with per-CPU page
  caches; slab `kmalloc` (M2: 16-2048 byte classes in 16 KiB slabs, per-CPU
  magazines; larger sizes take whole blocks); VMObjects (anonymous, file-backed, MMIO, shared), demand paging,
  copy-on-write `fork`, one page cache shared by `read` and `mmap`, ASIDs,
  guard pages on kernel stacks.
- **Threads (M3).** A kernel thread has a 64 KiB guard-paged stack and a
  saved context: callee-saved registers plus all FP/SIMD registers (saved at
  every switch, since kernel code never uses them, they hold the user state).
  Threads are reference counted; the scheduler holds a reference until the
  thread has exited and the next thread on its processor has freed its
  stack, after which `join()` returns. The boot code becomes the "boot"
  thread; every processor has an idle thread.
- **Scheduling (M3).** Per-CPU run queues with one FIFO per QoS class
  (UserInteractive > UserInitiated > Default > Utility > Background); the
  most urgent class always runs first. A woken or new thread goes to an idle
  processor if any (its last one first), else to its last processor; a
  processor that runs dry steals from another's queue before idling. Time
  slices (4 ms) are armed only while other threads wait, so a lone thread
  runs untimed and the kernel stays tickless. Switches happen when a thread
  blocks, yields or exits, and on return from an interrupt that asked for
  one (a slice ended, or a more urgent thread woke). Later: boosts for input
  and the window server, coalescing for background work.
- **Processes (M4).** A process has an address space, open files, signal
  state and one thread (several in M6). Its address space is a list of
  regions over anonymous VMObjects, mapped lazily by the page fault handler:
  a first touch gets a zeroed page; after `fork` both processes share every
  page, counted per page, and a write to a shared page copies it. User TLB
  entries are tagged with a 16-bit ASID, so switching processes flushes
  nothing. Programs are static ELF files loaded from the boot archive (the
  root filesystem until M5); `/sbin/init` is process 1 and adopts orphans.
  A fatal signal prints a crash report: the fault, registers and a backtrace
  symbolized from the program's own symbol table.
- **User memory layout (M4).** Programs at 0x400000, `mmap` from
  0x0000'1000'0000'0000 upwards, the stack (8 MiB) below
  0x0000'8000'0000'0000, nothing below 0x10000 (null pointers fault).
- **Syscalls.** `SVC #0`, negative-errno returns, ELF64 binaries. Defined
  once in `Kernel/API/Syscalls.def`, from which the kernel's dispatcher and
  LibSystem's numbers come; see [System Calls](Syscalls.md). Port calls join
  in M7.
- **IPC.** Mach-style ports with send/receive rights in a per-task handle
  table. Messages carry inline bytes, port rights and out-of-line memory moved
  by page remapping. Request/reply uses direct-handoff scheduling. Pipes, Unix
  sockets, signals, futexes and shared memory sit alongside.
- **Filesystems.** Vnodes, mount table and path-lookup cache; tmpfs, devfs,
  FAT32 (for the EFI System Partition), the boot archive, and **vultureFS**:
  copy-on-write B-trees with checksummed metadata, atomic checkpoints,
  snapshots, clones, xattrs, POSIX permissions + ACLs, and a container holding
  System (sealed) and Data volumes. The same code builds for the host as
  `mkvfs` and `fsck.vfs`.
- **Drivers (DeviceKit).** IOKit-like C++ driver classes with a registry
  tree, driver matching, INTx via the device tree's `interrupt-map` first and
  MSI-X through the GICv3 ITS when available, and DMA buffers.
- **Locks.** `SpinLock` is a fair ticket lock that masks interrupts while
  held, so an interrupt handler can never spin on a lock its own processor
  holds. This removes the class of deadlock the Rust kernel had. Taking a
  lock twice, or waiting on one for 5 s, panics. Threads sleep on `Mutex`
  (FIFO hand-over to the longest waiter), `Semaphore` and `WaitQueue`
  (condition checked under its lock, so no wakeup is lost).
- **Debugging.** Serial log, embedded symbol table, symbolized backtraces
  (continuing through exceptions via a frame record in each trap frame),
  `VERIFY`, QEMU's GDB stub (`Meta/run.sh --gdb`), and the `ktest` boot mode.
  A panic stops the other processors and bypasses the log lock.

## What became of the Rust kernel

The Rust tree is preserved at tag `v0.1-rust` (branch `legacy/rust`). Its
ideas carry over; its code does not.

| Rust | C++ replacement |
|---|---|
| `bootloader` crate, `boot/`, `linker.ld` | `Boot/` (VultureBoot) and per-arch linker scripts |
| Phased init in `kernel/src/lib.rs` | `Kernel/Init.cpp` with the real memory map |
| `gdt.rs`, `interrupts.rs`, PIC/PIT | AArch64 vectors, GICv3, generic timer (x86 in M14) |
| `scheduler.rs`, `process.rs` (bookkeeping only) | Real context switch, per-CPU queues, QoS |
| `syscall.rs` (unreachable) | `SVC` entry and a generated syscall table |
| `memory/` (model + buggy heap) | Buddy allocator, slab, VMObjects, address spaces |
| `fs-vulturefs/` (in RAM) | On-disk vultureFS keeping CoW, snapshots, xattrs and ACLs |
| `security/` (allow-all stubs) | Enforced credentials, sandbox profiles, code signing |
| `ipc/`, `networking/`, `services/` | Kernel ports + LibIPC, kernel TCP/IP, daemons under `Services/` |
| `graphics/`, `ui/`, `pkg/` | PranaWindowServer, pranaUI, vulturePkg (names kept) |
| Kernel-mode shell | `vsh` in userspace, plus a kernel debug console on panic |
| About 80 orphan stub files | Dropped; this roadmap replaces them |
