//! # Device Drivers
//!
//! Driver trait and registry for kernel device drivers.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

pub mod ata;
pub mod keyboard;
pub mod pci;
pub mod timer;

/// Trait that all device drivers must implement
pub trait Driver {
    /// Human-readable name of the driver
    fn name(&self) -> &'static str;

    /// Initialize the driver
    fn init(&mut self);

    /// Check if the driver is active
    fn is_active(&self) -> bool;
}

/// Initialize all device drivers
pub fn init_all() {
    pci::init();
    ata::init();
    timer::init();
    crate::println!("[drivers] Timer initialized (100 Hz)");
    crate::println!("[drivers] Keyboard driver active");
}
