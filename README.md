# vultureOS

A Unix operating system written in C++ that looks and feels like macOS.
Everything inside it is built from scratch in this repository: bootloader,
kernel, C library, graphics, fonts rasterizer, UI framework, window server,
desktop and apps.

**Status:** milestone M0 is done. VultureBoot, our own UEFI loader, boots on
AArch64 in QEMU, and the shared base library is in place with its unit tests.
The kernel arrives in M1. See the [Roadmap](Documentation/Roadmap.md).

## Requirements

macOS on Apple Silicon (QEMU then runs vultureOS with hardware
virtualization), plus:

```sh
brew install llvm@22 lld@22 cmake ninja qemu
```

## Build and run

```sh
cmake -G Ninja -S . -B build
ninja -C build            # build everything
ninja -C build run        # boot in QEMU
ninja -C build check      # unit tests + headless boot test
```

`Meta/run.sh --help` lists more options, such as software emulation (`--tcg`)
and waiting for a debugger (`--gdb`).

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
