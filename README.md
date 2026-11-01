# vultureOS

A Unix operating system written in C++ that looks and feels like macOS.
Everything inside it is built from scratch in this repository: bootloader,
kernel, C library, graphics, fonts rasterizer, UI framework, window server,
desktop and apps.

**Status:** milestone M6 is done: vultureOS is a working Unix system, with
a text login on the console.

- VultureBoot, our own UEFI loader, loads the kernel on AArch64 and hands it
  the machine: memory map, framebuffer, device tree and serial port.
- The kernel boots to a macOS-style boot screen, or a verbose log with `-v`.
  It runs all 8 processors, schedules threads preemptively by QoS class
  (173 ns context switch), and keeps time without a periodic tick.
- Programs run in their own address spaces, dynamically linked against
  `/usr/lib/libSystem.B.dylib` by our dynamic linker, `/usr/lib/dyld`. A
  crashing program is killed alone, with a symbolized crash report.
- Files live on vultureFS, our copy-on-write filesystem with checksummed
  metadata, on virtio-blk or NVMe disks; 100 power cuts in a row while
  writing leave it consistent.
- `vlaunchd` is process 1, as launchd is on macOS: it keeps the jobs of
  `/System/Library/LaunchDaemons` running, getty on the console among them.
- Log in as `vulture` (password `vulture`) to `vsh`, a shell with zsh's
  look and job control, and about 70 commands in the places macOS keeps
  them (`ls`, `grep`, `sed`, `find`, `ps`, `sudo`, ...). Root has no
  password, as on macOS: use `sudo`.
- Every push is built on macOS and Linux by GitHub Actions; `ninja check`
  runs the unit tests, the kernel tests, 30 userland suites (among them our
  [POSIX conformance subset](Documentation/POSIX.md): 208 requirements), a
  console login typed over the serial line, and the storage tests.

Next is M7, IPC and services. See the [Roadmap](Documentation/Roadmap.md).

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
Meta/run.sh               # boot in QEMU
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
- `Meta/run.sh --boot-args -v` boots verbosely.
- `Meta/run.sh --help` lists the other options, such as software emulation
  (`--tcg`), waiting for a debugger (`--gdb`) and saving a screenshot.
- For editor support, point clangd at `/opt/homebrew/opt/llvm@22/bin/clangd`.
  The build writes `build/compile_commands.json`.

## Documentation

- [Roadmap](Documentation/Roadmap.md): milestones, performance budgets, risks
- [Architecture](Documentation/Architecture.md): design, build system, repository layout
- [Boot Protocol](Documentation/BootProtocol.md): how VultureBoot starts the kernel
- [System Calls](Documentation/Syscalls.md): the calling convention and the calls
- [vultureFS](Documentation/VultureFS.md): the filesystem, on disk and in the kernel
- [Design System](Documentation/DesignSystem.md): the macOS look, and how it is measured and locked
- [Coding Style](Documentation/CodingStyle.md)

The previous Rust implementation is preserved at tag `v0.1-rust`.

## Author

- [krishpranav](https://github.com/krishpranav)

## License

vultureOS is licensed under the [GPL-3.0 License](LICENSE).
