//! # PIT Timer Driver
//!
//! Programmable Interval Timer (8253/8254) configured at 100Hz
//! for preemptive scheduling ticks.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use x86_64::instructions::port::Port;

/// PIT base frequency (1.193182 MHz)
const PIT_FREQUENCY: u32 = 1_193_182;

/// Desired timer frequency in Hz
const TIMER_HZ: u32 = 100;

/// PIT I/O ports
const PIT_CHANNEL_0: u16 = 0x40;
const PIT_COMMAND: u16 = 0x43;

/// Initialize the PIT at the desired frequency
pub fn init() {
    let divisor = PIT_FREQUENCY / TIMER_HZ;

    unsafe {
        // Channel 0, lobyte/hibyte, rate generator
        let mut cmd_port = Port::<u8>::new(PIT_COMMAND);
        cmd_port.write(0x36);

        let mut data_port = Port::<u8>::new(PIT_CHANNEL_0);
        data_port.write((divisor & 0xFF) as u8); // Low byte
        data_port.write(((divisor >> 8) & 0xFF) as u8); // High byte
    }
}

/// Get the configured timer frequency
pub const fn frequency() -> u32 {
    TIMER_HZ
}
