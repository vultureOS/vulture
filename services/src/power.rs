//! # Power Management
//!
//! System power management including sleep, wake, and shutdown.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

/// Power states
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PowerState {
    /// Normal operation
    Active,
    /// Screen off, system running
    DisplaySleep,
    /// Low-power sleep (RAM preserved)
    Sleep,
    /// Hibernate (state saved to disk)
    Hibernate,
    /// Shutting down
    ShuttingDown,
    /// Rebooting
    Rebooting,
}

/// Battery state
#[derive(Debug, Clone, Copy)]
pub struct BatteryInfo {
    pub present: bool,
    pub charging: bool,
    pub percentage: u8,
    pub time_remaining_minutes: u32,
    pub cycle_count: u32,
    pub health: u8, // percentage
}

impl BatteryInfo {
    /// Create a default (no battery / desktop) info
    pub const fn desktop() -> Self {
        Self {
            present: false,
            charging: false,
            percentage: 100,
            time_remaining_minutes: 0,
            cycle_count: 0,
            health: 100,
        }
    }
}

/// Power management interface
pub struct PowerManager {
    pub state: PowerState,
    pub battery: BatteryInfo,
}

impl PowerManager {
    pub const fn new() -> Self {
        Self {
            state: PowerState::Active,
            battery: BatteryInfo::desktop(),
        }
    }

    /// Request sleep
    pub fn sleep(&mut self) {
        self.state = PowerState::Sleep;
    }

    /// Wake from sleep
    pub fn wake(&mut self) {
        self.state = PowerState::Active;
    }

    /// Request shutdown
    pub fn shutdown(&mut self) {
        self.state = PowerState::ShuttingDown;
    }

    /// Request reboot
    pub fn reboot(&mut self) {
        self.state = PowerState::Rebooting;
    }
}
