//! # IPC Subsystem
//!
//! Inter-Process Communication for vultureOS.
//! Provides message channels, named ports, and POSIX-like signals.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

pub mod channel;
pub mod port;
pub mod signal;

use spin::Mutex;

/// Global IPC subsystem
pub struct IpcSubsystem {
    initialized: bool,
}

impl IpcSubsystem {
    pub const fn new() -> Self {
        Self { initialized: false }
    }

    /// Initialize the IPC subsystem
    pub fn init(&mut self) {
        port::init();
        self.initialized = true;
    }

    /// Check if IPC is ready
    pub fn is_ready(&self) -> bool {
        self.initialized
    }
}
