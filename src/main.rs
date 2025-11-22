//! # vultureOS Entry Point
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]
#![no_main]

extern crate alloc;

use bootloader::{entry_point, BootInfo};
use core::panic::PanicInfo;

entry_point!(kernel_main);

fn kernel_main(boot_info: &'static BootInfo) -> ! {
    // Immediate raw serial write — no init, no deps, just verify we get here
    unsafe {
        // Write "OK\n" to COM1 (0x3F8) — this proves we reached kernel_main
        x86_64::instructions::port::Port::<u8>::new(0x3F8).write(b'O');
        x86_64::instructions::port::Port::<u8>::new(0x3F8).write(b'K');
        x86_64::instructions::port::Port::<u8>::new(0x3F8).write(b'\n');
    }

    // Now run the full kernel with boot info (needed for heap page mapping)
    let mut kernel = vulture_kernel::Kernel::new();
    kernel.run_with_boot_info(boot_info);
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    // Try serial output for panic
    unsafe {
        let msg = b"\n[PANIC] ";
        for &byte in msg {
            x86_64::instructions::port::Port::<u8>::new(0x3F8).write(byte);
        }
    }
    vulture_kernel::serial_println!("{}", info);
    loop {
        x86_64::instructions::hlt();
    }
}
