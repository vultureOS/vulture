# vultureOS

A Unix operating system written in C++ that looks and feels like macOS.
Everything inside it is built from scratch in this repository: bootloader,
kernel, C library, graphics, fonts rasterizer, UI framework, window server,
desktop and apps.

**Status:** milestone M4 is done.

- VultureBoot, our own UEFI loader, loads the kernel on AArch64 and hands it
  the machine: memory map, framebuffer, device tree and serial port.
- The kernel boots to a macOS-style boot screen, or a verbose log with `-v`.
- It runs on its own page tables with buddy and slab allocators, takes
  interrupts through the GICv3, keeps time without a periodic tick, and
  brings all 8 processors online.
- Threads are scheduled preemptively over all processors by QoS class, with
  work stealing, sleeping locks and a 173 ns context switch.
- Programs run in user mode with their own address spaces: `/sbin/init`
  starts from the boot archive, and processes fork (copy-on-write), exec,
  wait and handle signals through our own C library. A crashing program is
  killed alone, with a symbolized crash report.
- Crashes produce a symbolized backtrace; booting with `ktest` runs the
  in-kernel test suite.

Next is M5, storage and vultureFS. See the [Roadmap](Documentation/Roadmap.md).

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

Both are built and tested on every push by GitHub Actions
([macOS](.github/workflows/macos.yml), [Linux](.github/workflows/linux.yml)).

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
