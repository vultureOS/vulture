# Roadmap

vultureOS is a Unix operating system that looks and feels like macOS
(Sonoma/Sequoia era). Every part of it is our own code. This document lists
the milestones in order. Each one ends in a bootable, demoable image, and a
milestone only counts as done when its "Done when" check passes.

See [Architecture](Architecture.md) for the design behind these steps.

## Milestones

| # | Status | Delivers | Done when |
|---|---|---|---|
| **M0 Clean slate** | ✅ 2026-10-31 | Rust tree archived (tag `v0.1-rust`, branch `legacy/rust`); CMake/Ninja superbuild with host, UEFI and target parts; `Meta/run.sh`; LibBase core (types, `Span`, `Optional`, `ErrorOr`/`TRY`, `StringView`, checked `format`) + LibTest; VultureBoot v0 | QEMU+HVF shows "VultureBoot" on screen and serial; `ninja test-host` passes. Verified by `ninja check`. |
| **M1 Boot + early kernel** | ✅ 2026-10-31 | VultureBoot loads the kernel ELF (and boot archive, once one exists), the boot-args, framebuffer, memory map and DTB, builds the first page tables and hands over; higher-half kernel; global constructors; framebuffer console (built-in 8x16 font) and boot screen (logo, progress bar); PL011 serial; exception vectors; symbolized, demangled panic backtraces; LibELF and LibDeviceTree | Kernel prints the memory map, draws the boot logo; a deliberate fault shows a symbolized backtrace. Verified by `ninja check` (`test-boot`, `test-panic`). |
| **M2 Core kernel** | Next | GICv3, generic timer (tickless), PSCI SMP bring-up and power-off, buddy allocator + slab `kmalloc`, kernel-owned page tables and direct map, IRQ-safe `SpinLock` vs sleeping `Mutex`, per-CPU data (TPIDR_EL1), `ktest` | All 8 vCPUs online; allocator/timer/lock stress `ktest` passes; QEMU exits with a pass marker |
| **M3 Threads + scheduler** | | Threads, kernel stacks, context switch with FP/SIMD, per-CPU queues + work stealing, QoS, wait queues, mutex/semaphore, sleep | 1000 threads ping-pong across 8 CPUs for 60 s with no hang; context-switch benchmark recorded |
| **M4 Userspace** | | Address spaces (TTBR0+ASID), regions/VMObjects, demand paging, CoW, ELF loader, SVC entry, generated syscall table, fault-safe user copies, fork/exec/exit/wait, signals, boot archive as root; LibC v0, LibCxxRT, crt0 | `/sbin/init` runs at EL0, forks/execs tests; a user segfault kills only that process and prints a crash report |
| **M5 Storage + vultureFS** | | VFS, file descriptors, pipes, devfs, tmpfs; PCIe ECAM + virtio core, virtio-blk, NVMe; GPT; FAT32; page cache + `mmap`; vultureFS v1; `Tools/mkvfs`, `fsck.vfs`, `mkimage` | Root on vultureFS persists across reboots; 100 forced kills mid-write always remount clean and pass fsck |
| **M6 Unix userland** | | vlaunchd (JSON plists, on-demand start, restart policy); TTY/PTY + line discipline; text console; **vsh** (pipes, redirects, globs, job control, history, completion, scripts); ~40 CLI tools; users/groups/login; permissions; sysctl; dynamic linker + shared cache; LibCore run loop | Text login, `ls -l /System \| grep Kernel \| wc -l`, Ctrl-Z/`fg`; our POSIX conformance subset passes |
| **M7 IPC + services** | | Kernel ports with direct handoff; LibIPC + `ipcc` IDL compiler; logd + `log`, notifyd, configd, pasteboard; name lookup via vlaunchd | IPC round trip within budget; `log stream` shows multiple services |
| **M8 Graphics + PranaWindowServer** | | virtio-gpu 2D + virtio-input; LibGfx (AA path rasterizer, strokes, rounded rects, gradients, blur, transforms, SIMD, tiles); LibText (TrueType/OpenType, glyph atlas, subpixel positioning); LibImage (inflate, PNG, JPEG, `.vicns`); WindowServer (shared-memory surfaces, damage compositing, shadows, vibrancy, input routing, window management, 2x) | 10 client windows with shadows and blur at 60 fps at 2560×1600 while dragging; golden screenshot passes |
| **M9 pranaUI** | | App/Window/View, responder chain, layout, LibAnimation, appearance (light/dark, accent, materials), the full control set, scrolling, drag & drop, accessibility tree, declarative builder layer | Widget Gallery within ±1 px of the measured metrics; light/dark × 1x/2x golden tests pass |
| **M10 Desktop** | | LoginWindow, menu bar, Dock, Finder, Spotlight + Indexer, Mission Control/Spaces, Launchpad, Notification + Control Center, wallpapers, System Settings, boot progress screen | Scripted UI walkthrough passes with golden screenshots |
| **M11 Apps + audio** | | Terminal, TextEdit, Preview, Calculator, Activity Monitor, Console, Disk Utility, Archive Utility, Software Center; virtio-sound + AudioServer + UI sounds | Each app's UI test passes; sound plays through QEMU |
| **M12 Network** | | virtio-net; TCP/IP; BSD sockets; DHCP; DNS; LibCrypto; LibTLS 1.3 + X.509; LibHTTP; `curl`, `ping`; signed packages over HTTPS | `curl https://example.com` works; crypto test vectors pass; a package installs from a test repo |
| **M13 Hardening** | | W^X, ASLR + KASLR, PAN/PXN/UXN, PAC/BTI, stack protector, kernel UBSan + KASAN-style shadow, sandbox profiles, privacy prompts, code signing at exec, sealed System volume, memory compressor | Mitigation suite passes |
| **M14 x86_64 + real hardware** | | `Arch/x86_64`, ACPI + AML subset, BOOTX64.EFI; AHCI, xHCI + USB HID/storage, e1000e, Intel HDA, PS/2; Raspberry Pi 4/5; Installer | One image boots in QEMU x86_64, on a real PC and on a Pi |

Rules that apply from M1 onward:

- Arch-specific code lives only in `Kernel/Arch/<arch>`, behind `Kernel/Arch/*.h` interfaces.
- Every phase adds tests and benchmarks.
- Every milestone reruns all lower-level suites before it counts as done.

Out of scope for now: porting third-party software, 3D GPU drivers
(rendering stays software/SIMD), a web browser engine, Wi-Fi/Bluetooth and
sleep/wake. These come after M14.

## Performance budgets

Measured on QEMU + HVF with 8 vCPUs, tracked by `ninja bench` from M3 on.

| Metric | Target |
|---|---|
| Boot, firmware handoff to login window | ≤ 2 s |
| Null syscall | < 150 ns |
| Context switch | < 1 µs |
| IPC round trip | < 3 µs |
| Window drag at 2560×1600 | 60 fps |
| Input to photon | ≤ 1 frame |
| TextEdit cold launch | < 300 ms |

## Risks

| Risk | Mitigation |
|---|---|
| Scope (this is a multi-year system) | Strict milestone order, a demo at every step, no polish ahead of its phase |
| Software rendering at 2x | Damage tracking, cached blur and shadows, SIMD and tiles; 1x fallback |
| HVF quirks (GIC, ITS, timers) | INTx fallback when ITS is missing; `--tcg` mode and GDB stub |
| vultureFS corruption | CoW + checksums from day one, host fsck, crash-kill harness in CI |
| C++ memory bugs | Smart pointers, `ErrorOr`, bounds checks, sanitizers, a kernel UBSan runtime, broad tests |
| Drift from the macOS look | Token-only theming and golden tests |
| Legal | Own name and logo, no Apple assets, OFL licenses shipped with the fonts |

## Findings so far

- **M0:** With `-machine virt,acpi=off` the firmware publishes a device tree
  (no ACPI tables), as planned. `virtio-gpu-pci` only offers a blit-only GOP,
  so `Meta/run.sh` uses `ramfb`, which gives a linear BGRX8 framebuffer for
  the boot console.
- **M1:**
  - The device tree parser moved up from M2 into a shared library
    (LibDeviceTree), because the loader needs it to find the serial port.
  - The loader must switch graphics modes after its last console output:
    resetting the firmware's text console to adapt to a new mode switches the
    mode back. Done that way, `ramfb` gives 1024×768.
  - A framebuffer in RAM (as `ramfb`'s is) is mapped cacheable. An uncached
    alias of memory the host reads through its cache risks stale pixels under
    virtualization.
  - Under HVF the CPU reports Apple as implementer but hides the part number.
  - From handoff to "boot complete" takes about 12 ms.
