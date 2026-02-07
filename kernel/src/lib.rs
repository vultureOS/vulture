//! # Filename: main.rs
//!
//! ### Description
//! kernel entry point
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

use vulture_memory::MemoryManager;
use vulture_fs::VultureFS;
use vulture_security::SecurityContext;

pub struct Kernel {
    memory: MemoryManager,
    fs: VultureFS,
    security: SecurityContext,
}

impl Kernel {
    pub fn new() -> Self {
        Self {
            memory: MemoryManager::new(),
            fs: VultureFS::new(),
            security: SecurityContext::new(),
        }
    }

    pub fn run(&mut self) -> ! {
        println!("vultureKernel starting...");

        self.memory.init();
        self.fs.init_root_fs();
        self.security.init();

        println!("Kernel fully initialized.");

        loop {
            core::hint::spin_loop();
        }
    }
}