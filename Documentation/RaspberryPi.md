# Raspberry Pi

vultureOS starts on a Raspberry Pi 4 or 5 from one file, `kernel8.img`, which `ninja` puts in
`build/raspberry-pi/`.

**What has and hasn't been tested.** All of this has only ever run on QEMU's emulated Pi 4
(`ninja test-raspberry-pi`). It has never run on a real Raspberry Pi, and the Pi 5 has never been
run at all, as QEMU doesn't have one. Expect to find problems on real hardware. The table below
says what stands in for what.

## Making a disk

The Pi's firmware isn't part of vultureOS: it's the Raspberry Pi Foundation's, under its own license,
and isn't in this repository. Put a copy of these, from the `boot/` directory of
[raspberrypi/firmware](https://github.com/raspberrypi/firmware), in a directory of your own:

| File | For |
|---|---|
| `start4.elf`, `fixup4.dat` | Pi 4 |
| `bcm2711-rpi-4-b.dtb` | Pi 4 |
| `bcm2712-rpi-5-b.dtb` | Pi 5 (its other firmware is in its EEPROM) |
| `overlays/disable-bt.dtbo` | Optional: a Pi 4's serial console, with the line in `config.txt` uncommented |

Then:

```
Meta/make-raspberry-pi-image.sh --firmware DIR -o pi.img
```

and write `pi.img` to an SD card. `Meta/make-universal-image.sh --raspberry-pi DIR` makes a disk
that starts PCs and AArch64 UEFI machines as well. Words in `cmdline.txt` on the first partition
are the kernel's command line; the firmware adds its own in front of them.

## How it starts

The firmware loads `kernel8.img` and jumps to it, at EL2 (`Boot/RaspberryPi`). The file is a stub followed
by the kernel and the boot archive, put together by `Tools/mkpiimage`. The stub comes down to EL1, turns
on the MMU, and from the device tree and the firmware's mailbox makes the same `BootInfo` and page
tables that the UEFI loader makes: see [Boot Protocol](BootProtocol.md). So the kernel is the same kernel, and these are
the only things it does differently here:

| | Elsewhere | On a Pi |
|---|---|---|
| Interrupt controller | GICv3 | GICv2 (`Arch/aarch64/GICv2.cpp`) |
| Starting other processors | PSCI | The spin table, on a Pi 4 (a Pi 5 has PSCI) |
| Powering off and restarting | PSCI | The watchdog, on a Pi 4. There's no real off: the firmware stops, and waits |
| Display | virtio-gpu | The framebuffer it asks the firmware for, through `FirmwareDisplay` |
| Disk | virtio-blk, NVMe, AHCI | The SD card, on an SD Host Controller (`Devices/Storage/SDCard.cpp`) |
| Clock | A real-time clock | None: it starts from when the kernel was built (a Pi 5 has one, unused) |
| Protections | PAN, PAC, BTI | None of these on a Pi 4's Cortex-A72; PAN only on a Pi 5's A76 |

## What QEMU's Pi 4 can and can't show

`Meta/raspberry-pi-test.sh` boots `kernel8.img` as the firmware would, and runs the kernel's own tests on all
four processors, every userland test, a login through the serial port, and, on the SD card, writes that are still
there after a restart. It compares the login window with a golden screenshot.

| Real Pi | In the test | So what's unknown |
|---|---|---|
| The firmware | QEMU loads the file, with its own parking code | Whether the firmware accepts this file, and where it loads it (the stub copes with anywhere higher than it's linked) |
| The firmware's device tree | `Tests/Hardware/raspi4b.dts`, written by hand to the same pattern | Anything the real one does differently |
| EMMC2 controller | QEMU's older SDHCI controller, in its place | The real controller's quirks. It's kept simple on purpose: 25 MHz, no DMA, no interrupts |
| GIC-400, with the firmware having set it up | QEMU's GICv2 with no security extensions | The non-secure view, which the firmware should have prepared |
| Caches | QEMU has none | Every cache flush in the stub, spin table and mailbox is from the architecture's rules, and untested |
| Pixel order | QEMU's | If colours are swapped on a real screen, this is why. The stub reads back what the firmware says it did |

## What's missing

- **USB, so no keyboard or mouse.** A Pi 4's USB ports are behind PCIe, on a VL805 chip; a Pi 5's are behind its RP1.
  The xHCI driver would do for the chip, but the PCIe controller needs a driver of its own, and neither is in QEMU. That
  driver has not been written blind: mistakes in it can stop a Pi from booting at all, where now it reaches the login
  window. It's the next thing to do with a Pi to test on. Until then, the serial console is the only way in.
- **Network.** A Pi 4's Ethernet is a GENET controller, a Pi 5's is on the RP1. Neither has a driver. Nor has Wi-Fi.
- **Sound.**
- **Faster SD.** It's 12.5 MB/s at most, with no higher bus speeds and no DMA.
