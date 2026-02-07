//! # Filename: main.rs
//!
//! ### Description
//! boot loader
//!
//! ### Legal Information
//! * **Copyright:** (C) 2022-2026 Krisna Pranav
//! * **License:** GNU General Public License v3.0 (GPL-3.0-or-later)
//!
//! This program is free software: you can redistribute it and/or modify
//! it under the terms of the GNU General Public License as published by
//! the Free Software Foundation, either version 3 of the License, or
//! (at your option) any later version.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use vulture_kernel::Kernel;

fn main() {
    println!("🟢 vultureOS Bootloader (simulated)");
    
    let memory_map = vec![
        ("RAM", 0x0000_0000u64, 0x1_0000_0000u64),
        ("IO",  0xFE00_0000u64, 0x1000_0000u64),
    ];

    println!("Memory Map:");
    for (t, s, e) in &memory_map {
        println!("  {}: {:#x} -> {:#x}", t, s, e);
    }

    println!("➡ Jumping to kernel...\n");

    let mut kernel = Kernel::new();
    kernel.run();
}