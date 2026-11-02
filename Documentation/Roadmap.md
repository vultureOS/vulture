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
| **M2 Core kernel** | ✅ 2026-10-31 | GICv3, generic timer (tickless), PSCI SMP bring-up and power-off, buddy allocator with per-CPU page caches, slab `kmalloc` with per-CPU magazines, kernel-owned page tables, direct map, MMIO window and guard-paged kernel stacks, IRQ-safe ticket `SpinLock`, per-CPU data (TPIDR_EL1), `ktest` | All 8 vCPUs online; allocator/timer/lock stress `ktest` passes; QEMU exits with a pass marker. Verified by `ninja check` (`test-boot`, `test-kernel`). |
| **M3 Threads + scheduler** | ✅ 2026-10-31 | Kernel threads with guard-paged stacks, context switch with FP/SIMD, per-CPU run queues with QoS classes, idle-first placement and work stealing, preemption by time slice and by more urgent wakeups, `Mutex`, `Semaphore`, `WaitQueue`, sleep, join; `RefPtr`/`OwnPtr` | 1000 threads ping-pong across 8 CPUs for 60 s with no hang (`ninja test-soak`: 228 million round trips); context-switch benchmark recorded (`ninja bench`: 173 ns) |
| **M4 Userspace** | ✅ 2026-10-31 | Address spaces (TTBR0 + 16-bit ASIDs), regions and anonymous VMObjects, demand paging, copy-on-write fork, ELF loader, SVC entry, syscall table from one `.def` file, fault-safe user copies (exception table), fork/exec/exit/waitpid, signals with handlers and crash reports, the boot archive (ustar, packed by our `mkarchive`) as the read-only root; LibC v0, LibCxxRT, crt0, `/sbin/init` | `/sbin/init` runs at EL0, forks/execs tests; a user segfault kills only that process and prints a crash report. Verified by `ninja check` (`test-userland`: 14 programs). |
| **M5 Storage + vultureFS** | ✅ 2026-10-31 | VFS (mount table, path walk across mounts and symbolic links), shared open files, pipes, devfs, tmpfs; PCIe ECAM with INTx routing; virtio-pci transport, virtio-blk and NVMe; MBR and GPT; FAT12/16/32 (read-only, the ESP at `/Volumes/EFI`); a page cache shared by `read`, `write` and `mmap` (private and shared file mappings); **vultureFS v1** (copy-on-write B+tree, CRC-32C metadata, alternating superblocks, crash-safe checkpoints, orphans) as the root; `Tools/mkvfs`, `fsck.vfs`, `mkimage` (GPT + FAT32 ESP + vultureFS) | Root on vultureFS persists across reboots; 100 forced kills mid-write always remount clean and pass fsck. Verified by `ninja check` (`test-persistence`: three boots, each finding what the last committed) and `ninja test-crash` (100 QEMU power cuts at random moments while writing and fsyncing: fsck clean after every cut, every committed file intact) |
| **M6 Unix userland** | ✅ 2026-11-01 | Process groups, sessions and job control; TTY layer (line discipline, termios, foreground groups), the console's input, pseudo-terminals; ANSI text console; users, groups and credentials (set-ID programs), permissions; `poll`, `sysctl`, `uname`, `getrandom`, `pread`/`pwrite` and more; **vlaunchd** as process 1 (JSON plists with launchd's keys: RunAtLoad, KeepAlive, ThrottleInterval, StartInterval, UserName, ...; restart policy; orphan reaping; clean shutdown); **vsh** (the POSIX shell language, pipes, redirects, globs, here-documents, functions, job control, history with ^R, completion, scripts, zsh's prompt and messages); about 70 command-line tools with macOS's options and output; `login`, `su`, `passwd`, `sudo`, `getty` (PBKDF2-SHA512 hashes in `/etc/master.passwd`); **dyld** + `libSystem.B.dylib` (every program position independent and dynamically linked, the library's pages shared through the page cache; a prelinked shared cache is left for later); LibCore (event loop, accounts), LibJSON, LibCrypto (SHA-2, HMAC, PBKDF2); CI on macOS and Linux | Text login, `ls -l /System/Library \| grep Kernels \| wc -l`, Ctrl-Z/`fg`; our POSIX conformance subset passes. Verified by `ninja check`: `test-login` (a console login typed over the serial line: the pipeline, a job suspended and continued, logging out and in again, `sudo poweroff`) and `test-userland` (30 suites, among them `posix`: 208 of 208 requirements, see [POSIX](POSIX.md)) |
| **M7 IPC + services** | ✅ 2026-11-01 | Kernel ports with direct handoff; LibIPC + `ipcc` IDL compiler; logd + `log`, notifyd, configd, pasteboard; name lookup via vlaunchd; threads (futexes, per-thread signals, ELF TLS, pthreads) | IPC round trip within budget; `log stream` shows multiple services |
| **M8 Graphics + PranaWindowServer** | ✅ 2026-11-01 | virtio-gpu 2D + virtio-input, which also types at the text console; LibGfx (AA path rasterizer, strokes, rounded rects, gradients, blur, transforms, SIMD, tiles); LibText (TrueType/OpenType, glyph atlas, subpixel positioning); LibImage (inflate, PNG, JPEG, `.vicns`); WindowServer (shared-memory surfaces, damage compositing, shadows, vibrancy, input routing, window management, 2x) | 10 client windows with shadows and blur at 60 fps at 2560×1600 while dragging; golden screenshot passes |
| **M9 pranaUI** | ✅ 2026-11-01 | App/Window/View, responder chain, layout, LibAnimation, appearance (light/dark, accent, materials), the full control set, scrolling, drag & drop, accessibility tree, declarative builder layer | Widget Gallery within ±1 px of the measured metrics; light/dark × 1x/2x golden tests pass |
| **M10 Desktop** | ✅ 2026-11-01 | LoginWindow, menu bar, Dock, Files, Search + Indexer, Overview/Spaces, Launcher, Notification + Quick Settings, wallpapers, Settings, boot progress screen | Scripted UI walkthrough passes with golden screenshots |
| **M11 Apps + audio** | ✅ 2026-11-02 | Terminal (LibVT), Text Editor, Viewer, Calculator, Monitor, Console, Disks, Archiver (tar and gzip), Software; virtio-sound, LibAudio (mixer, sounds made from sine waves, WAV), AudioServer, `afplay`; alert, notification, trash, login and volume sounds, a Sound pane and a Quick Settings slider | Each app's UI test passes; sound plays through QEMU. Verified by `ninja check`: `test-ui` (a walkthrough of every application at 2x, 189 steps, real keyboard and pointer, golden screenshots) and `test-sound` (a 440 Hz tone sent through the server is found, 1.2 s of it, in what QEMU recorded the device playing) |
| **M12 Network** | ✅ 2026-11-02 | virtio-net; ARP, IPv4, ICMP, UDP and TCP (window scaling, Reno, fast retransmit, RFC 6298 timers); BSD sockets with macOS's structures; `ipconfigd` (DHCP); a resolver (`getaddrinfo`, `/etc/hosts`, CNAMEs); LibCrypto (AES-GCM, in hardware where it can be, ChaCha20-Poly1305, HKDF, X25519, Ed25519, RSA and ECDSA checks on P-256 and P-384); LibTLS (TLS 1.3 client, X.509 chains); LibHTTP; `curl`, `ping`, `host`, `ifconfig`, `netstat`, `nc`, `shasum`; signed packages (`pkg`, `installd`, Software's Get and Remove), Settings' Network pane | `curl https://example.com` works; crypto test vectors pass; a package installs from a test repo. Verified by `ninja check` (`test-host`: RFC vectors and OpenSSL's answers; `test-userland`: `sockets`, with 4 MiB through one packet in 23 lost, and `crypto`; `test-network`: 48 checks against this machine's servers, then Software's Get, run and Remove with the real pointer) and `ninja test-internet` (`curl https://example.com`, over the real internet, certificate checked) |
| **M13 Hardening** | ✅ 2026-11-02 | W^X, ASLR + KASLR, PAN/PXN/UXN, PAC/BTI, stack protector, kernel UBSan + KASAN-style shadow, sandbox profiles, privacy prompts, code signing at exec, sealed System volume, memory compressor | Mitigation suite passes. Verified by `ninja check` (`test-userland`: `hardening`, `sandbox`, `privacy`, `compressor`; `test-hardening`: each deliberate attack on the kernel ends in the panic that catches it, each change to a sealed file is caught, BTI under emulation as this host has none; `test-sanitized-kernel`: what is planted is found, and nothing else in the kernel's tests or userland's; `test-compressor`: half as much memory again as a 1 GiB and a 512 MiB machine have, every byte checked; `test-network`: a package's first try at Documents asks, and the answer is kept) |
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

| Metric | Target | Measured |
|---|---|---|
| Boot, firmware handoff to login window | ≤ 2 s | |
| Null syscall | < 150 ns | |
| Context switch | < 1 µs | 173 ns (M3, yield between two threads on one processor) |
| IPC round trip | < 3 µs | 0.84 µs (M7, call and answer between two processes; 4.6 µs through a generated proxy to a vlaunchd-started service) |
| Window drag at 2560×1600 | 60 fps | 59.2–59.7 fps (M8, and again at M9; 10 windows, 5 with blur; 16.7 ms beat, 8–11 ms of work per frame; beats with no pointer movement draw nothing) |
| Input to photon | ≤ 1 frame | |
| Text Editor cold launch | < 300 ms | |

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
- **M2:**
  - The sleeping `Mutex` moved to M3: sleeping needs threads and wait
    queues, which arrive there. M2 has the IRQ-safe `SpinLock`.
  - From handoff to "boot complete", with all 8 processors online, takes
    about 25 ms; starting the 7 secondary processors takes under 1 ms.
  - Waking an idle processor (in WFI) with an IPI costs 20-35 µs under HVF:
    the host has to wake the vCPU thread. One `run_on_all` round to 7 idle
    processors takes 130-260 µs. M3's scheduler should spin briefly before
    WFI and avoid IPIs on hot paths.
  - The buddy allocator keeps its free lists inside free memory. Under a
    hypervisor, the first write to a guest page makes the host commit it, so
    allocating all of memory once (the `exhaust_and_recover` test) commits
    all of the guest's RAM on the host until QEMU exits, and takes ~1.7 s.
  - The GIC's distributor needs affinity routing enabled before its groups;
    with QEMU's GICv3 (single security state) `GICD_CTLR.DS` reads as 1.
  - A tickless kernel cannot wait with a bare WFI: with no periodic tick,
    nothing may ever wake it. Every timed wait arms its own timer.
- **M3:**
  - Measured (`ninja bench`): context switch 173 ns; semaphore round trip
    (two wakeups, two switches) 404 ns on one processor, 24 µs across two
    (each wakeup wakes an idle vCPU through the host); creating, starting
    and joining a thread 6.4 µs. A user-interactive thread preempts a
    background one 8 µs after it is started.
  - 1000 threads ping-ponging through semaphores manage 3.8 million round
    trips per second on 8 processors, steadily for 60 s.
  - Under HVF, a timer on a busy processor fires within ~15 µs, but a
    processor idling in WFI wakes up to 2-4 ms late: QEMU sleeps the vCPU
    thread with a host timed wait, which macOS coalesces. Real hardware does
    not have this. If frame pacing (M8) needs it, idle processors can wake
    early and spin out the rest.
  - Rule that keeps switching deadlock-free: a thread is resumed only after
    its last processor finished saving it. Wakers wait for that (briefly);
    work stealing skips such threads; no processor ever waits for another in
    the middle of a switch.
  - Drains of per-processor caches for exact memory accounting run in
    interrupt context (`Processor::call_on_all`): doing them in threads
    would itself allocate and free stacks, changing what is measured.
- **M4:**
  - Programs are static `ET_EXEC` executables at 0x400000, not static PIE
    as planned: position independence matters once there is ASLR (M13), and
    the loader is ready to grow it.
  - The boot archive is a ustar file, so macOS's own `tar` can list it; the
    kernel only reads it (the writer is in our host tool).
  - Timers became cancellable from any processor (waiting out a running
    callback), so a signal can cut short a sleep on another processor
    without the timer touching a thread that has gone.
  - A signal that would be ignored is discarded when sent; otherwise an
    ignored `SIGCHLD` from an adopted orphan interrupted init's `waitpid`.
  - `sigreturn` finds the signal frame through `x19` (callee-saved, so it
    survives the handler) rather than the stack pointer.
- **M5:**
  - vultureFS v1 has no journal. Being copy-on-write is enough: nothing
    the last checkpoint uses is overwritten before the next one commits,
    which the allocator enforces with two bitmaps (in use now, in use by
    the last checkpoint).
  - File data goes to new blocks at write-back. Metadata nodes are given
    disk locations only when a checkpoint writes them, so a node changed
    many times between checkpoints is written once.
  - The crash harness kills QEMU (SIGKILL) mid-write. A simulated disk in
    the host tests goes further: it loses, reorders and tears unflushed
    writes. Both must leave a volume that `fsck.vfs` passes and that holds
    exactly its last committed checkpoint.
  - Measured under QEMU+HVF with `cache=writeback`, the crash writer
    commits about 300 iterations a second. Each iteration is two file
    writes, three fsyncs (each a full checkpoint) and a rename.
  - Mappings are torn down by the dying process's own thread: ending a
    file mapping can sleep, and the scheduler's reaper runs with
    interrupts masked.
  - Version 1 limits volumes to 28 GiB (224 bitmap blocks named in the
    superblock). Snapshots, clones, xattrs, ACLs and the sealed System
    volume come later (M13 seals System).
- **M8:**
  - Coverage is each pixel's exact area in the shape, from signed areas summed
    along the row, not samples. The accumulating sum saturates where shapes overlap,
    so a stroke's outline goes through the exact inner corner, rather than a pivot
    point whose small overlaps brightened edges by a few levels.
  - Curves are flattened to a fortieth of a pixel: at a tenth, the polygons sat
    inside a circle by enough to show as 20 levels of 255 at its edge.
  - A material's blur reads what is below it, but the framebuffer holds that only
    where it is being redrawn. Anything within blur reach of a material therefore
    redoes all of it, and redraws all it reads. Redrawing less left the picture
    depending on the order changes came in, which the golden test showed at once
    (the same scene, 13 levels apart between boots).
  - The drag benchmark's first run gave 54 fps at 16 ms of work a frame. Most of the
    work went to per-pixel code for materials, and to waking 7 threads for each layer
    (an interprocessor interrupt each, and those trap under HVF): workers now spin
    for 200 us between jobs. That made it 8 ms and still 54 fps, as frames were timed
    from the last one and timers fire 2-4 ms late under HVF. On a fixed beat, as a
    display's refresh is, a late frame takes that time from the wait for the next.
  - Injected pointer events passed every test while the real pointer did nothing:
    the input device told pollers nothing had changed but never woke them. The
    golden test now drags a window with QEMU's tablet device, through /dev/input.
  - QEMU's virtio-gpu writes no response for commands on the cursor queue.
  - The look is not yet calibrated: the theme's values are starting estimates from
    what macOS is known to use, not measurements of it (see
    [Design System](DesignSystem.md)). Goldens catch regressions, not distance from
    macOS.
- **M9:** The look can be measured without anyone's screenshots: AppKit picks its
  look by the SDK a program links against, so a program linked against SDK 15.2 draws
  Sequoia's window corners, title bars and control sizes on macOS 26, off screen (see
  `Meta/Calibration` and `DesignSystem.md`, which says what this leaves out). A face is
  a stack of rounded rectangles of alpha, a shape's edge at 2x spreads a pixel with
  about 12% of its coverage, and 1x is the box filter of 2x; rings outside must not
  run under the face, or a translucent one shows them. Golden screenshots found a real
  race: material jobs each blurred the screen while siblings wrote to it, so a band of
  a sidebar depended on which thread got there first. They now compute everything and
  then place it. pranaUI runs on the host with a platform of the test's own, so its 58
  tests run under ASan and UBSan; a test on the real server caught what the host's
  could not: a press that becomes a drag, move or menu never hears its release.
- **M10:** A desktop is mostly clients like any other: the server adds only what they
  cannot do themselves (who is an application, whose menus show, hotkeys, minimizing,
  spaces, Overview), and tells a shell client the rest as a JSON state on any
  change. Damage for materials must grow to a fixed point: a lower window's blur
  reach dirtied part of the menu bar after the bar had been considered, and the bar
  then blurred from finished pixels beside it. An active window's shadow must be
  invalidated before it shrinks. A menu published as JSON is rebuilt only when it
  changes, since any window, its own included, changes the state. The walkthrough
  (`Tests/UI/desktop.chat`, real keyboard and pointer) found what unit tests had not:
  a reference to an appearance that a switch to dark had replaced, a path held across
  the reload that freed it, and Return going on from a field to what contains it.
- **M11:** Running the system's real programs found kernel bugs no unit test had:
  exec and exit freed an address space `describe()` was still reading (a panic in the
  slab allocator, minutes later, and only under Monitor), and a pseudo-terminal's slave
  belonged to root, so a person's shell could not open it, and never told the master it
  had gone. A number from Monitor found another: AudioServer at 40 s of CPU, a server
  spinning on a device with room, because `unwatch` takes the watch's id and it was given
  the descriptor; `test-sound` now also fails if the server uses the processor while
  there is nothing to play. Screenshots found what no assertion did: table dividers
  drawn 16 points from where they could be dragged, and a row's text outside its
  selection. Checking sound means looking at it: QEMU's `wav` backend records what the
  device played, and a Goertzel filter over each 20 ms finds the tone in it.
- **M13:** A test of the next feature is what found a mistake in the last one. Asking before
  an installed application reads Documents didn't: the seal said it was a system program, as a
  test's package was in the seal. CMake reads a `[` in a name as grouping list items, so with
  `/bin/[` in the list, the filter to leave packages out saw everything after it as one item,
  and matched nothing. The same test showed that opening an application from Software, once
  Software was sandboxed, put it inside Software's sandbox; so what's sandboxed now asks the
  desktop to do it. The sanitized kernel's only real find was a misaligned read in the sound
  driver, which no ordinary test could see: this hardware doesn't mind. PAC broke backtraces
  first, by being on: a signed return address printed as nonsense until the signature was
  stripped. And the tamper tests passed for a while by testing nothing, as `Meta/run.sh`
  helpfully put back the file they had changed before booting. So a test of what is caught
  now also says what it changed, and looks for the specific message. The compressor's second
  chance needed no support from the page tables' access bits at all: a page that's unmapped
  and not asked for again hasn't been touched, and asking for it again is what page faults
  already do. On the 1 GiB machine, 1,531 MiB of three processes' memory, in order and then
  not, moved 5.9 GB through the pool in 14 seconds, every byte correct. Its one real problem
  showed up as a test that only sometimes took too long, under the sanitizers, where everything
  is twenty times slower: a busy program's pages going through their second chance looked like
  progress, so running out of memory took as long as luck had it, and pages that wouldn't
  compress were tried again every time round. Raising the timeout would have passed.
- **M12:** Loopback found what a clean network never would: after a timeout, going back to
  resend moves `snd_nxt`, and the cumulative ACK that then arrives is for more than
  `snd_nxt`. Taken as acknowledging what was never sent, it was answered with an ACK, which
  was answered with an ACK, for ever. A connection now keeps the furthest it ever sent too.
  The test that showed it loses one packet in 23 on purpose, a `sysctl` for root.
  QEMU's user-mode network answers ping from the host, DNS through the host's resolver,
  and everything else with a real connection: `ping example.com` worked on the first boot.
  Public-key code is checked against published vectors and, where those can't be trusted
  from memory, against OpenSSL's and Node's answers: nothing here was tested only against
  itself. AES on a processor without it costs a read of the whole S-box per byte, so a client
  there offers ChaCha20 first. A signature on the index vouches for each package through
  its hash, so that one key and one file cover a repository; a package's own manifest must
  still agree with the index, and its files may go only under `/Applications` and `/usr/local`.

