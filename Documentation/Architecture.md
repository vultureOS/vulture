# Architecture

## Goals

- A Unix operating system that looks and feels like macOS (Sonoma/Sequoia
  era): global menu bar, Dock, Files, translucent materials, rounded
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
| Architectures | AArch64 first, on QEMU `virt` with HVF; x86_64 since M14. A build directory holds one: `cmake -S . -B build-x86_64 -G Ninja -DVULTURE_ARCH=x86_64` |
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
| `ninja -C build run` | Boots to the desktop in QEMU's window (`Meta/run.sh`; `Meta/run_debug.sh` for logs in the terminal, `Meta/run_console.sh` for the text console) |
| `ninja -C build test-host` | Runs host unit tests through CTest |
| `ninja -C build test-boot` | Boots headless; the kernel must print its memory map and finish booting |
| `ninja -C build test-panic` | Boots with `fault=data-abort`; the panic report must decode the fault and symbolize the backtrace |
| `ninja -C build test-kernel` | Boots with `ktest`: the in-kernel tests run on all processors, print `ktest: PASS`, and power the machine off |
| `ninja -C build test-userland` | Boots with `utest`: `/sbin/vlaunchd --test` runs the programs in `/usr/tests`, prints `userland: PASS`, and powers off |
| `ninja -C build test-login` | Boots normally and logs in on the console over the serial line (`Meta/run.sh --chat Tests/Console/login.chat`), then powers off with `sudo` |
| `ninja -C build test-persistence` | Three boots in a row on one disk, each finding what the last committed |
| `ninja -C build test-desktop-boot` | Boots with no flags, as an image does: the desktop's login window must come up |
| `ninja -C build test-debug-boot` | Boots with `debug`: processes, launchd's jobs and the unified log must all reach the console |
| `ninja -C build test-ui` | Golden screenshots: the window server's scene, the Widget Gallery, the desktop and every application, driven by the real keyboard and pointer (`Meta/ui-test.sh`) |
| `ninja -C build test-sound` | Records what the sound device plays, and looks for a tone sent through the sound server in it (`Meta/sound-test.sh`) |
| `ninja -C build test-network` | DHCP, DNS, HTTP, TLS (each cipher and group, and each way a certificate can be wrong), signed packages by `pkg` and by Software, against servers on this machine (`Meta/network-test.sh`) |
| `ninja -C build test-hardening` | Each deliberate attack on the kernel ends in the panic that catches it; each change to a sealed file or a signed program is refused; PAC and BTI under emulation (`Meta/hardening-test.sh`) |
| `ninja -C build test-hardware` | Devices real machines have, through the drivers that use them, with no virtio: AHCI disks, a USB stick behind a hub, USB and PS/2 input, e1000e, Intel HD Audio, the firmware's framebuffer; and the Installer, from its window onto a blank disk, which then boots alone (`Meta/hardware-test.sh`, either architecture) |
| `ninja -C build test-raspberry-pi` | QEMU's Raspberry Pi 4, from `kernel8.img`: the kernel's and userland's tests, a serial login, the login window against a golden screenshot, and the SD card across a restart (`Meta/raspberry-pi-test.sh`). AArch64 builds only |
| `ninja -C build test-compressor` | Programs using half as much memory again as a 1 GiB and a 512 MiB machine have, every byte checked; and then more than will compress, which ends the biggest one and nothing else (`Meta/compressor-test.sh`) |
| `ninja -C build test-sanitized-kernel` | The kernel built with UBSan and KASAN-style heap checks: these catch what is planted, and find nothing in the kernel's tests or userland's (`Meta/sanitized-kernel-test.sh`) |
| `ninja -C build check` | All of the above tests |
| `ninja -C build test-internet` | `ping` and `curl https://example.com`, over the real internet (not part of `check`) |
| `ninja -C build test-crash` | 100 power cuts while writing; fsck clean after each (not part of `check`) |
| `ninja -C build test-soak` | 1000 threads ping-ponging for 60 s (not part of `check`) |
| `ninja -C build bench` | Boots with `ktest=bench` and prints the measurements |

`Meta/build.sh` wraps the configure step and `ninja`, checks that the tools
are installed, and takes ninja targets as arguments (`--check` adds `check`).
`Meta/run.sh` also takes `--boot-args` (the kernel command line, for example
`-v` for a verbose boot), `--memory` (how much the machine has) and, with `--headless`, `--screenshot FILE` to save
the screen as PNG and `--wait-for-exit` to require that the guest powers
itself off. `--real-devices` boots with what real machines have instead of virtio, `--image FILE` from a whole
disk, and `--raspberry-pi` on QEMU's Pi 4.

`Meta/make-universal-image.sh` makes one disk that starts both AArch64 and x86_64 machines: the firmware of each
finds its own loader on the EFI System Partition, the loader its own kernel (`vulture.<arch>`, with
`vulture.bootarchive.<arch>`), and the kernel its own vultureFS partition, by what's in
`System/Library/CoreServices/Architecture`. `Meta/make-raspberry-pi-image.sh` makes one for a Pi: see
[Raspberry Pi](RaspberryPi.md).

Kernel command line words:

| Word | Effect |
|---|---|
| `-v` | Verbose boot: the kernel log on screen instead of the boot screen |
| `ktest`, `ktest=PREFIX` | Run the in-kernel tests (those whose `suite.name` starts with PREFIX), then power off |
| `ktest-seconds=N` | How long soak tests run (default 2) |
| `utest`, `utest=NAME` | Start `/sbin/vlaunchd --test`, which runs the userland tests (or `/usr/tests/NAME` alone) and powers off |
| `console` | Stay on the text console, with no desktop. Without it the system boots to the desktop (or to the console, if there is no display) |
| `soundtest` | Run the jobs in `/usr/tests/sound` alone (see `test-sound`) |
| `nettest`, `netguitest` | Run the jobs in `/usr/tests/network` or `/usr/tests/network-gui` alone (see `test-network`) |
| `debug` | Log what the system is doing on the console: processes forked, exec'd and ended, launchd's jobs, and the unified log at every level |
| `fault=KIND` | End the boot with a deliberate crash: `data-abort`, `trap`, `verify`, `panic`, `interrupt` (in a timer interrupt) or `processor` (on processor 1); or try something the kernel should stop: `stack-smash`, `text-write`, `rodata-write`, `data-execute`, `pac-return`, `bti-branch`, and on a sanitized kernel `kasan-*` and `ubsan-*` |
| `debugfault=KIND` | Lets root ask the kernel to touch user memory the wrong way, through the `debug.fault` sysctl: `user-read`, `user-write`, `user-execute` (see `Tests/Userland/kernel-fault.cpp`) |
| `nokaslr` | Don't move the kernel (`Meta/run.sh --gdb` adds it, so that symbols stay put) |
| `noseal` | Don't check the system against its seal |
| `cursortopleft` | The display puts a cursor's picture by its corner and takes no notice of its hot spot, as QEMU's window on a Mac does: the driver moves the picture to make up for it. `Meta/run.sh` says this for that window |
| `nocodesign` | Run anything, signed or not | Kernel code that does not touch hardware (such as
`Kernel/BootArguments.cpp`) is compiled into host tests too; see
`Tests/Kernel`.

UEFI uses PE/COFF, so the loader compiles for a Windows target. We use the
GNU flavor (`windows-gnu`) because it keeps the Itanium C++ ABI the rest of
vultureOS uses. With the MSVC flavor, CMake would also drop `--target` and
header dependency tracking.

## System layers

```
 Apps         Files · Terminal · Text Editor · Viewer · Settings · Monitor · Software …
 Desktop      LoginWindow · DesktopShell (menu bar, Search, notifications) · Dock · Overview · Quick Settings
 Frameworks   pranaUI (AppKit-like) · LibAnimation · LibGfx (Quartz-2D-like) · LibText (CoreText-like) · LibImage
 Core libs    LibCore (run loop, JSON plists) · LibIPC (+ipcc IDL) · LibCrypto/TLS/HTTP · LibVT · LibAudio
 Runtime      LibC (math included) · LibCxxRT · LibSystem (syscalls; libSystem.B.dylib) · dyld · LibBase (shared with kernel)
 Services     vlaunchd (PID 1) · logd · notifyd · configd · PranaWindowServer · AudioServer · indexd · clipboardd
 ─────────────────────────────── syscalls (SVC) + Mach-style ports ───────────────────────────────
 Kernel       Hybrid (XNU-style): monolithic core for speed + port IPC for services
              Memory · Tasks/Scheduler · VFS (vultureFS, FAT32, tmpfs, devfs) · Net (TCP/IP) · Security · Devices
 Arch         aarch64 (EL1, GICv2/v3, generic timer, PSCI or spin table) · x86_64 (APIC, ACPI)
 Boot         VultureBoot (our UEFI loader) → BootInfo → kernel
```

## Repository layout

Directories appear as their milestone lands; nothing is created ahead of time
as a stub.

```
Meta/          build system (CMake parts, toolchains), run.sh, CI
Tools/         host C++ tools: mkarchive (M4); mkvfs, fsck.vfs, mkimage (M5); ipcc, imgdiff, mkpiimage, mkfdt; generators
Boot/          VultureBoot UEFI loader: Arch/<arch> (page tables, handoff), UEFI definitions; RaspberryPi (kernel8.img's stub)
Kernel/        API (BootInfo) · Arch (interfaces; aarch64: entry, vectors, page tables, GICv2 and v3, timer, PSCI, spin table, SMP;
               x86_64: entry, IDT, GDT, page tables, APIC, ACPI power, SMP) · Firmware (ACPI)
               Console (framebuffer, font, boot screen) · Debug (log, panic, symbols) · Devices/Serial
               Interrupts · Locking · Memory (buddy, physical memory, heap, memory manager)
               Tasks (Processor, Thread, Scheduler, Process) · Syscalls · Bus/PCI · Devices (VirtIO, Storage)
               FileSystem (VFS, vultureFS, FAT, tmpfs, devfs, pipes, boot archive)
               Time (timer queues) · Tests (ktest) · Library (C++ runtime) · Init.cpp
Libraries/     LibBase · LibArchive · LibC · LibCxxRT · LibSystem (libSystem.B.dylib) · LibDyld (dyld)
               LibCore (event loop, accounts) · LibCrypto · LibIPC · LibJSON · LibServices · LibDeviceTree · LibELF · LibVultureFS · LibTest
               LibGfx (drawing) · LibText (fonts) · LibImage · LibCompress · LibWindow (windows, for programs)
               LibUI (pranaUI) … later
Services/      vlaunchd (process 1, M6); logd · notifyd · configd · clipboardd (M7); WindowServer (M8); Dock …  (M10+)
Applications/  Files · Terminal · Text Editor · Viewer · Settings · Installer · WidgetGallery …   (M9+)
Utilities/     vsh and the command-line tools (bin/), login and accounts                               (M6)
Base/          root filesystem skeleton, themes, fonts, icons, wallpapers, sounds                    (M5+)
Tests/         host unit tests, kernel tests, userland suites (Userland; posix.cpp is the POSIX subset),
               the console (Console/*.chat), golden screenshots (UI; ninja test-ui)
Documentation/ these documents
```

### On-disk hierarchy

macOS-style at the top, real Unix underneath:

- `/Applications/*.app`, `/System/Library/{Kernels,CoreServices,Frameworks,LaunchDaemons,LaunchAgents,Fonts,Themes,Sounds,Desktop Pictures}`, `/Library`, `/Users/<name>/{Desktop,Documents,Downloads,Library}`, `/Volumes`
- `/bin`, `/sbin`, `/usr/{bin,lib,share/man}`, `/private/{etc,var,tmp}` with symlinks at `/etc` and `/var`, and `/dev` (devfs)
- App bundles: `Name.app/Contents/{Info.plist, MacOS/, Resources/}`
- Today there's one volume, with what's sealed checked file by file, and `/tmp` is a directory of its own. Separate System and Data volumes, and `/tmp` where a Mac has it, come with vultureFS 2 (M19).

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
  state and its threads (see below). Its address space is a list of
  regions over anonymous VMObjects, mapped lazily by the page fault handler:
  a first touch gets a zeroed page; after `fork` both processes share every
  page, counted per page, and a write to a shared page copies it. User TLB
  entries are tagged with a 16-bit ASID, so switching processes flushes
  nothing. `/sbin/vlaunchd` is process 1 and reaps orphans. Files can be mapped into
  memory: a mapping's pages are the file's own cached pages (copied on the
  first write in a private mapping).
  A fatal signal prints a crash report: the fault, registers and a backtrace
  symbolized from the program's own symbol table.
- **Threads (M7).** A process's threads share everything but their signal
  masks, the signals sent to them alone (faults, `pthread_kill`), their
  alternate signal stacks, and registers: the thread pointer (TPIDR_EL0) is
  switched with them. A signal for the process goes to the first thread not
  blocking it; a thread that is running in user mode is sent an IPI, as nothing
  else would make it enter the kernel in a tickless system. Stopping stops every
  thread on its way to user mode. `exit` and `exec` end the other threads and
  wait until each is off its processor before the address space changes. The
  last thread to end ends the process. Futexes are private to a process, keyed
  by address space and address; a waiter joins its bucket before reading the
  word, as reading can fault and sleep. `pthread_*` is in the C library, with no
  system call for an uncontended lock; `malloc` and stdio take locks of the same
  kind, and `fork` holds them across the copy so a child never inherits one
  held by a thread it does not have.
- **Thread-local storage (M7).** ELF TLS, AArch64's variant 1: all of it static,
  as every library is loaded before the program starts. dyld gives each module
  an offset from the thread pointer, resolves `TLS_TPREL64` and `TLSDESC`, builds
  the image every thread's block is copied from and sets up the first thread's
  (a statically linked program's C library does it itself). `errno` is one such
  variable.
- **Dynamic linking (M6).** Programs are position independent and name
  `/usr/lib/dyld` as their interpreter; `exec` loads both and starts dyld,
  which maps the libraries the program needs from their files (so all
  processes share their pages through the page cache), relocates
  everything, runs the initializers and jumps to the program. The C
  library, the C++ runtime and LibBase's compiled parts are one library,
  `/usr/lib/libSystem.B.dylib`. Crash reports name library frames with
  their library. See [POSIX](POSIX.md) for what the C library offers.
- **User memory layout (M4, M6).** Programs at 0x400000, dyld at
  0x0000'7f00'0000'0000, `mmap` (libraries included) from
  0x0000'1000'0000'0000 upwards, the stack (8 MiB) below
  0x0000'8000'0000'0000, nothing below 0x10000 (null pointers fault).
- **Unix userland (M6).** Sessions, process groups and job control;
  terminals with a line discipline (the console, pseudo-terminals);
  users, groups and set-ID programs; vlaunchd starts and keeps the jobs of
  `/System/Library/LaunchDaemons` (JSON property lists with launchd's
  keys), getty on the console among them, and shuts down cleanly. vsh is
  the shell (`/bin/sh`), with zsh's prompt and messages for people.
- **Syscalls.** `SVC #0`, negative-errno returns, ELF64 binaries. Defined
  once in `Kernel/API/Syscalls.def`, from which the kernel's dispatcher and
  LibSystem's numbers come; see [System Calls](Syscalls.md).
- **IPC (M7).** Mach-style ports: one receive right, any number of send rights,
  all of them file descriptors, so `poll` and the run loop watch them. Messages
  carry inline bytes (up to 64 KiB), up to 16 descriptors, a reply right and up
  to 8 out-of-line regions (shared copy-on-write), and receivers learn the
  sender's pid, uid and gid from the kernel. A call hands the processor straight
  to the receiver and back. `ipcc` turns an interface description (`*.ipc`) into
  a proxy for clients and a stub for servers; LibIPC has the encoding, large
  byte buffers going out of line by themselves. Each process inherits a bootstrap
  port, which vlaunchd serves: `look_up` finds a service by name, and starts it
  if it is a job with `MachServices` that is not running. Pipes, signals,
  futexes and shared mappings sit alongside. (There are no Unix-domain sockets: on this
  system, local programs talk over ports.)
- **System services (M7).** Each a vlaunchd job reached by name: logd (the
  unified log, `os_log`, `log stream` and `log show` in macOS's format), notifyd
  (`notify(3)`), configd (ComputerName, LocalHostName and HostName in
  `/Library/Preferences/SystemConfiguration/preferences.plist`; it names the
  machine) and clipboardd (each user's pasteboards).
- **Graphics and input (M8).** `/dev/gpu0` (virtio-gpu): a framebuffer in ordinary
  memory to `mmap`, shown rectangle by rectangle when flushed, and a cursor on a plane
  of its own. `/dev/input` (virtio-input): one reader at a time gets every key and
  pointer event; while there is none, keys type at `/dev/console` on a US layout, so
  the text login works from QEMU's window as well as the serial line.
  `shared_memory_create` makes a nameless file to map in several processes; window
  surfaces are these, sent through a port.
- **PranaWindowServer (M8).** A vlaunchd job for the graphical session (the boot flag
  `gui`). Clients draw in two buffers they share with it and present one; `LibWindow`
  is their side. The compositor draws directly in the framebuffer, on a fixed 60 Hz
  beat: the screen is 32-pixel tiles, dirty or not, and each layer (wallpaper, then each
  window's shadow and body) is drawn over the dirty ones, in bands, on up to 8 threads.
  Shadows are nine-sliced blurred masks; corners are continuous, from one mask mirrored.
  A material (vibrancy) is a blur of what is below, at up to 1/8 size, then saturation
  and a tint, so what is below must be finished first: it splits the frame, and
  whatever it reads is redrawn with it. The server draws title bars and traffic
  lights over the client's pixels, moves and resizes windows, and sends pointer and
  keyboard events on. Every look comes from a theme file.
- **Applications (M11).** Ordinary pranaUI programs in `.app` bundles, each with its
  logic in a piece a host test can reach (`Tests/Applications`, `Tests/LibVT`). Terminal is
  LibVT (a VT-style screen model, with no idea what draws it) on a pseudo-terminal. Monitor,
  Disks and Console read `sysctl` and the unified log. Archiver is LibArchive and
  LibCompress, and refuses any member that would land outside the folder it expands into.
  What things are called is in [Naming](Naming.md).
- **Sound (M11).** `/dev/audio0` is virtio-sound: 48 kHz, stereo, 16-bit, four periods of
  4 KiB (about 85 ms), a write waiting while they are full and `poll` saying when one is
  free. AudioServer (`com.vulture.audioserver`) is the only program with it open: it adds
  together what everyone sends, in LibAudio's mixer, one period at a time, and writes
  nothing when nothing plays. The system's sounds are computed, from sine waves and a
  little noise, so there is nothing recorded to carry around or license. Programs use
  `Platform::play_sound` or the `Audio` endpoint; `afplay` plays WAV files. With no device,
  every request is still answered, and nothing is heard. The boot flag `soundtest` runs
  `Tests/Sound` instead of a session.
- **Network (M12).** `Kernel/Net`: IPv4 over Ethernet, and loopback (`en0`, `lo0`). Frames go
  from the driver to a queue for the network thread, which also runs the timers, so nothing
  of it runs in an interrupt; everything that touches protocol state holds one lock, plenty
  for a desktop's few connections. TCP is RFC 9293 with window scaling, RFC 5681's Reno, fast
  retransmit and RFC 6298's timers; what arrives out of order waits for what is missing.
  Not here: IPv6, reassembling fragments, Unix-domain sockets, SACK and TCP timestamps.
  `ipconfigd` gets an address by DHCP and writes `/etc/resolv.conf`; the resolver is in LibC.
  LibCrypto, LibTLS and LibHTTP sit on a `Transport` of the caller's, so most of them run on
  the host for tests. TLS is 1.3 alone, as a client, with no resumption and no client
  certificates. Which authorities it trusts is `/etc/ssl/cert.pem`, which
  `Meta/update-roots.sh` makes from a bundle of their public certificates: data, not code.
- **Packages (M12).** A package is a gzip'd tar of `Manifest.json` and `Payload/`, laid out as
  the system is (`Meta/vpkg.py` makes them). A repository is a folder at a URL with
  `index.json`, listing each package's SHA-256, and `index.json.sig`, an Ed25519 signature
  by the key `/etc/pkg/repositories.json` names for it. An index with a lower serial than
  the last is refused. LibPackage does the work: `pkg` for root at a terminal, and
  `installd` for Software, which runs each job in a process of its own, as everything in a
  download comes from outside. Only members of `admin` may change what is installed.
- **Filesystems (M5).**
  - **VFS.** Paths resolve one component at a time across a mount table.
    Open files are shared by `dup` and `fork`.
  - **Root: vultureFS.** All metadata lives in one copy-on-write B+tree
    with CRC-32C-checksummed nodes. Atomic checkpoints alternate between
    two superblocks, so a crash at any moment leaves the last checkpoint
    whole; see [vultureFS](VultureFS.md).
  - **Page cache.** File data is cached in pages, and `read`, `write` and
    `mmap` share them. A sync thread commits every 5 seconds.
  - **Other filesystems.** FAT12/16/32 (read-only, for the EFI System
    Partition at `/Volumes/EFI`); tmpfs at `/tmp`; devfs at `/dev`; pipes.
    Without a disk, the boot archive is unpacked into a tmpfs root.
  - **Shared with the host.** The same vultureFS code builds for the host
    as `mkvfs`, `fsck.vfs` and `mkimage`.
  - **Later.** Snapshots, clones, xattrs and ACLs.
- **Drivers.**
  - **PCI (M5).** PCIe through ECAM, with INTx routed via the device
    tree's `interrupt-map`.
  - **Disks.** The virtio-pci transport with split virtqueues; virtio-blk
    and NVMe disks. MBR and GPT partitions appear in `/dev` as `disk0`,
    `disk0s1`, … as on macOS.
  - **Later: DeviceKit.** IOKit-like C++ driver classes with a registry
    tree, driver matching, MSI-X through the GICv3 ITS, and DMA buffers.
- **Locks.** `SpinLock` is a fair ticket lock that masks interrupts while
  held, so an interrupt handler can never spin on a lock its own processor
  holds. This removes the class of deadlock the Rust kernel had. Taking a
  lock twice, or waiting on one for 5 s, panics. Threads sleep on `Mutex`
  (FIFO hand-over to the longest waiter), `Semaphore` and `WaitQueue`
  (condition checked under its lock, so no wakeup is lost).
- **Memory protection (M13).** Nothing is writable and executable at once. Programs, their
  libraries, stacks and heaps are at random addresses, and so is the kernel: the loader
  chooses from 16384 places and applies its relocations. The kernel can't reach user memory
  outside of `copy_from_user` and friends (PAN), or run it (PXN). Return addresses are signed,
  with a key for each address space in user mode and one for the kernel (PAC), branches land
  only where they're meant to (BTI, where the processor has it), and stacks have canaries.
- **The seal and code signing (M13).** `vseal` writes a manifest of the system, with every file's
  SHA-256, signed by a key made for the build; the kernel checks the signature when it boots
  and each file when it's used, and refuses to change anything it covers (`EROFS`). A program
  only runs if the seal lists it, or root has vouched for its hash, as the installer does for
  a package whose signature it has checked. Executable memory comes only from such files.
  Limits: the seal goes by whole files, so a block that is read again after leaving the cache
  isn't checked again; and nothing checks the loader or the kernel (no UEFI secure boot).
- **Sandboxes (M13).** A profile in `/System/Library/Sandbox/Profiles` says what a process may
  read, write, run and connect to, and which services it may look up; everything else is
  `EPERM`, and a line in the log. It is checked on canonical paths, so symbolic links and `..`
  don't matter, is inherited, and can only narrow. Calculator, Viewer, Text Editor, Archiver,
  Console, Disks, Software and the services that have no need for more are all sandboxed: see
  `SANDBOX` in `vulture_add_application` and `SandboxProfile` in a job's plist. A sandboxed
  process's children are as sandboxed as it is, so one that needs an application opened asks
  `com.vulture.workspace`, in DesktopShell.
- **Privacy (M13).** A person's Desktop, Documents and Downloads are only available to code that
  came with the system. Anything else, and anything it starts (so `/bin/cat` is no way round
  this), has to be allowed: the kernel asks `privacyd` over `/dev/privacy`, which asks them
  through a panel from DesktopShell, or remembers what they said. Settings can change that,
  for the application's next launch. Nothing installed can answer for itself, as `privacyd`
  asks the kernel who is calling. With nobody to ask, the answer is no. Limits: what a service
  does on an application's behalf isn't covered.
- **Memory compression (M13).** There's no swap. When free memory is low (1/16 of all of it), a thread
  goes round the address spaces. A page it finds mapped, it unmaps, which is the page's second
  chance; if it's still unmapped next time round, nothing has touched it, so it goes into a
  pool of compressed pages (or, if it's all zeros, is let go). A fault brings it back. Pages
  are compressed by `LibCompress/PageCompression`, which is original and made for pages; what
  won't get to half its size stays as it is, and isn't tried again until something has touched
  it, as nothing else can have changed it. `fork` shares compressed pages as it does others.
  The last 1/64 of memory is held back from programs, so that this always has room to work; a
  fault that finds none waits. If nothing is left to compress, the process using the most
  memory is ended, preferring people's own over root's and never init, and whoever else was
  waiting carries on. Statistics are in `vm.compressor.*`, and in Monitor. Limits: a page shared
  with another process isn't compressed, nor are private copies of file pages, and the file
  cache doesn't give up memory.
- **x86_64 (M14).** The same kernel above `Kernel/Arch`. Below it: a trampoline for starting processors from real
  mode, in four low pages the loader sets aside; x2APIC where there is one, the local APIC's one-shot timer calibrated
  against a TSC that's calibrated by HPET or the PIT; MSI vectors for devices (MSI-X entry 0 where there's no MSI);
  SMEP and SMAP where AArch64 has PAN and PXN, with no equivalent of PAC or BTI; `%gs` for the processor area. ACPI
  finds the rest (`Firmware/ACPI`): MADT, MCFG, HPET, FADT. `LibACPI/AML` walks AML without running it, for `_S5_`
  and plain `_PRT`s; a `_PRT` computed by a method isn't understood, so devices without MSI on such a machine get no
  interrupts. Programs: TLS variant 2 by way of `set_thread_pointer`, and an `-ftls-model=initial-exec` build.
- **Real devices (M14).** AHCI (`Devices/Storage/AHCIDevice`; 64-bit controllers only in full); e1000e and its relatives
  (`Devices/Net/E1000`; tested on the 82574L, with others listed by ID and untested, as I217 and I219 have a PHY to wake);
  Intel HD Audio (`Devices/Audio/IntelHDA`: walks the codec to a DAC with an output); xHCI (`Devices/USB/XHCI`) with hubs,
  HID keyboards and pointers, and bulk-only SCSI disks; PS/2 on x86 (`Devices/Input/PS2`); 16550 serial; the firmware's
  framebuffer when there's no GPU to drive (`Devices/Graphics/FirmwareDisplay`: a shadow in memory and a pointer drawn
  in software); and a Raspberry Pi's SD card. Each is tested through QEMU's model of it, which is not the hardware: see
  [Roadmap 1](Roadmap-1.md) for what real machines have and haven't had the chance to show.
- **Installer (M14).** `Applications/Installer` asks; `installd`, which runs as root, checks that the disk isn't in use and
  runs `/usr/libexec/vinstall`, which does as `mkimage` does (the same code, from `Tools/vfs`) on the machine itself: a GPT,
  the EFI System Partition this system started from, and a vultureFS volume from the boot archive that's on it. The seal
  goes with the archive, so what's installed is as verifiable as what it came from.
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
