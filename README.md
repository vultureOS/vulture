# vultureOS

A Unix operating system written in C++ that looks and feels like macOS.
Everything inside it is built from scratch in this repository: bootloader,
kernel, C library, graphics, fonts rasterizer, UI framework, window server,
desktop and apps.

**Status:** all fifteen milestones of the [first roadmap](Documentation/Roadmap-1.md), M0 to M14, are done, in
QEMU. It has never run on a real PC or a real Raspberry Pi, and QEMU's models of devices aren't the devices, so
expect to find problems on real machines. The [Roadmap](Documentation/Roadmap.md) sets out what comes next: from
a Mac that looks right to a Mac you can live on.

- **It starts** on AArch64 and x86_64 through VultureBoot, our own UEFI loader, and on a Raspberry Pi 4 or 5
  from `kernel8.img`. One disk can start all of them.
- **The kernel** runs every processor, schedules threads preemptively by QoS class, keeps files on vultureFS, a
  copy-on-write filesystem with checksummed metadata that has survived 100 power cuts in a row, and compresses
  memory rather than swapping. It has W^X, ASLR and KASLR, PAN, PXN, PAC, BTI, SMEP and SMAP where the processor
  does, a sealed system, signed programs, sandboxes and privacy prompts.
- **Unix:** `vlaunchd` is process 1; dynamically linked programs with threads; `vsh`, with zsh's look and job control;
  about 70 commands where macOS keeps them; Mach-style ports and system services; TCP/IP, TLS 1.3, `curl` and signed packages.
- **The desktop:** PranaWindowServer composes at 60 fps at 2560×1600, with shadows and blurred materials, drawing
  and setting type with our own rasterizer and font engine. On that are pranaUI, the login window, menu bar, Dock, Files,
  Search, Overview, Launcher, notifications, Settings; and Terminal, Text Editor, Viewer, Calculator, Monitor, Console,
  Disks, Archiver, Software and Installer. Log in as `vulture`, password `vulture`.
- **Drivers:** virtio for QEMU; NVMe, AHCI, xHCI with USB HID and disks, e1000e, Intel HD Audio, PS/2, the firmware's
  framebuffer, and on a Pi an SD card. On a Pi there's no USB, network or sound yet: see
  [Raspberry Pi](Documentation/RaspberryPi.md).
- **Tests:** `ninja check` runs the unit tests, the kernel's tests, 45 userland programs (among them our
  [POSIX conformance subset](Documentation/POSIX.md): 208 requirements), logins typed at the console, golden
  screenshots of everything on screen, deliberate attacks on the kernel, a sanitized kernel, and the devices above.
  Every push is built on macOS and Linux, for both architectures, by GitHub Actions.

## Requirements

macOS on Apple Silicon is the main development machine: QEMU then runs
vultureOS with hardware virtualization (HVF).

```sh
brew install llvm@22 lld@22 cmake ninja qemu
```

Linux works too (Ubuntu 24.04 is tested). QEMU runs vultureOS with KVM on
an AArch64 machine and emulates it (TCG, several times slower) elsewhere.
Add LLVM 22 from [apt.llvm.org](https://apt.llvm.org), then:

```sh
sudo apt install clang-22 lld-22 llvm-22 libclang-rt-22-dev cmake ninja-build \
    qemu-system-arm qemu-efi-aarch64
```

`Meta/build.sh` finds LLVM 22 in Homebrew's or apt.llvm.org's place
(`VULTURE_LLVM_ROOT` and `VULTURE_LLD_ROOT` override that), and
`Meta/run.sh` picks the firmware and accelerator for the machine
(`VULTURE_ACCEL=tcg` forces emulation).

Both are built on every push by GitHub Actions
([macOS](.github/workflows/macos.yml), [Linux](.github/workflows/linux.yml));
`ninja check` runs the tests.

## Build and run

```sh
Meta/build.sh             # configure (first time) and build everything
Meta/run.sh               # boot to the desktop, in QEMU's window
Meta/run_debug.sh         # the same, with what the system is doing in this terminal
Meta/run_console.sh       # boot to the text console instead
Meta/build.sh --check     # build, then run every test
```

`Meta/build.sh --help` lists its options (`--clean`, `-j N`, a build
directory, ninja targets). The underlying commands, if you prefer them:

```sh
cmake -G Ninja -S . -B build
ninja -C build            # build everything
ninja -C build run        # boot in QEMU
ninja -C build check      # unit tests + headless boot, crash and kernel tests
```

- `Meta/run.sh` boots from a persistent vultureFS disk,
  `build/qemu/disk.img`. Each new build updates the system files on it
  and leaves your own files. `--fresh-disk` starts it over, and `--nvme`
  attaches it as NVMe.
- `ninja -C build image` makes `build/vultureOS.img`, one bootable disk
  with an EFI System Partition and vultureFS, as a real machine has.
  `Meta/run.sh --image build/vultureOS.img` boots it.
- `ninja -C build test-crash` cuts the power 100 times while files are
  being written. After each cut `fsck.vfs` must pass and nothing committed
  may be lost.
- `Meta/run.sh` boots to the desktop and keeps the terminal quiet: log in as `vulture`,
  password `vulture`. What the system prints goes to `build/qemu/console.log`.
- `Meta/run_debug.sh` boots the same desktop and shows what is going on in the terminal:
  the kernel's log, every process forked, exec'd and ended, every launchd job, and the
  unified log at every level (windows made and destroyed, applications launched,
  sessions begun). The terminal is also the serial console, so you can log in there and
  run `ps` or `log show`. It is all kept in `build/qemu/debug.log`, and what QEMU notices
  the guest doing wrong in `build/qemu/qemu.log`. It takes `run.sh`'s options, such as
  `--gdb`.
- `Meta/run_console.sh` boots to the text console, which you can type at in QEMU's
  window or in the terminal.
- The system is on the network through QEMU's NAT: `curl https://example.com`, `ping`, `pkg`.
  `VULTURE_NET_OPTIONS=hostfwd=tcp::8022-:22` forwards a port to it.
- `Meta/run.sh --boot-args -v` boots verbosely.
- For x86_64, `Meta/build.sh --build-dir build-x86_64 --arch x86_64`, then `Meta/run.sh --build-dir build-x86_64`.
- `Meta/run.sh --real-devices` boots with what real machines have instead of virtio, and `--raspberry-pi` on QEMU's Pi 4.
- [Documentation/Naming.md](Documentation/Naming.md) says what each application and
  service is called, and why those names.
- `Meta/run.sh --help` lists the other options, such as software emulation
  (`--tcg`), waiting for a debugger (`--gdb`) and saving a screenshot.
- For editor support, point clangd at `/opt/homebrew/opt/llvm@22/bin/clangd`.
  The build writes `build/compile_commands.json`.

## Documentation

- [Roadmap](Documentation/Roadmap.md): what comes next, performance budgets, risks
- [Roadmap 1](Documentation/Roadmap-1.md): M0 to M14, and what each of them found
- [Architecture](Documentation/Architecture.md): design, build system, repository layout
- [Boot Protocol](Documentation/BootProtocol.md): how VultureBoot starts the kernel
- [Raspberry Pi](Documentation/RaspberryPi.md): making a disk for one, and what is and isn't known to work
- [System Calls](Documentation/Syscalls.md): the calling convention and the calls
- [vultureFS](Documentation/VultureFS.md): the filesystem, on disk and in the kernel
- [Design System](Documentation/DesignSystem.md): the macOS look, and how it is measured and locked
- [Coding Style](Documentation/CodingStyle.md)

The previous Rust implementation is preserved at tag `v0.1-rust`.

## Author

- [krishpranav](https://github.com/krishpranav)

## License

vultureOS is licensed under the [GPL-3.0 License](LICENSE).
