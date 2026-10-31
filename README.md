# vultureOS

A Unix operating system written in C++ that looks and feels like macOS.
Everything inside it is built from scratch in this repository: bootloader,
kernel, C library, graphics, fonts rasterizer, UI framework, window server,
desktop and apps.

**Status:** milestone M3 is done.

- VultureBoot, our own UEFI loader, loads the kernel on AArch64 and hands it
  the machine: memory map, framebuffer, device tree and serial port.
- The kernel boots to a macOS-style boot screen, or a verbose log with `-v`.
- It runs on its own page tables with buddy and slab allocators, takes
  interrupts through the GICv3, keeps time without a periodic tick, and
  brings all 8 processors online.
- Threads are scheduled preemptively over all processors by QoS class, with
  work stealing, sleeping locks and a 173 ns context switch.
- Crashes produce a symbolized backtrace; booting with `ktest` runs the
  in-kernel test suite.

Next is M4, userspace. See the [Roadmap](Documentation/Roadmap.md).

## Requirements

macOS on Apple Silicon (QEMU then runs vultureOS with hardware
virtualization), plus:

```sh
brew install llvm@22 lld@22 cmake ninja qemu
```

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

- `Meta/run.sh --boot-args -v` boots verbosely.
- `Meta/run.sh --help` lists the other options, such as software emulation
  (`--tcg`), waiting for a debugger (`--gdb`) and saving a screenshot.
- For editor support, point clangd at `/opt/homebrew/opt/llvm@22/bin/clangd`.
  The build writes `build/compile_commands.json`.

## Documentation

- [Roadmap](Documentation/Roadmap.md): milestones, performance budgets, risks
- [Architecture](Documentation/Architecture.md): design, build system, repository layout
- [Boot Protocol](Documentation/BootProtocol.md): how VultureBoot starts the kernel
- [Design System](Documentation/DesignSystem.md): the macOS look, and how it is measured and locked
- [Coding Style](Documentation/CodingStyle.md)

The previous Rust implementation is preserved at tag `v0.1-rust`.

## Author

- [krishpranav](https://github.com/krishpranav)

## License

vultureOS is licensed under the [GPL-3.0 License](LICENSE).
