//! # Serial Port (COM1)
//!
//! Provides serial port output for QEMU debugging via COM1 (port 0x3F8).
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use core::fmt;
use spin::Mutex;
use x86_64::instructions::port::Port;

const COM1_BASE: u16 = 0x3F8;

/// COM1 serial port
pub struct SerialPort {
    base: u16,
}

impl SerialPort {
    pub const fn new(base: u16) -> Self {
        Self { base }
    }

    /// Initialize the serial port
    pub fn init(&self) {
        unsafe {
            Port::<u8>::new(self.base + 1).write(0x00); // Disable interrupts
            Port::<u8>::new(self.base + 3).write(0x80); // Enable DLAB
            Port::<u8>::new(self.base + 0).write(0x03); // Baud divisor lo (38400)
            Port::<u8>::new(self.base + 1).write(0x00); // Baud divisor hi
            Port::<u8>::new(self.base + 3).write(0x03); // 8N1
            Port::<u8>::new(self.base + 2).write(0xC7); // Enable FIFO
            Port::<u8>::new(self.base + 4).write(0x0B); // IRQs enabled, RTS/DSR
        }
    }

    fn is_transmit_empty(&self) -> bool {
        unsafe { Port::<u8>::new(self.base + 5).read() & 0x20 != 0 }
    }

    pub fn send(&self, byte: u8) {
        while !self.is_transmit_empty() {
            core::hint::spin_loop();
        }
        unsafe {
            Port::<u8>::new(self.base).write(byte);
        }
    }
}

impl fmt::Write for SerialPort {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for byte in s.bytes() {
            self.send(byte);
        }
        Ok(())
    }
}

/// Global COM1 serial port
pub static SERIAL1: Mutex<SerialPort> = Mutex::new(SerialPort::new(COM1_BASE));

/// Initialize serial port
pub fn init() {
    SERIAL1.lock().init();
}

/// Print to serial port (for QEMU debugging)
#[macro_export]
macro_rules! serial_print {
    ($($arg:tt)*) => {
        $crate::serial::_serial_print(format_args!($($arg)*));
    };
}

#[macro_export]
macro_rules! serial_println {
    () => ($crate::serial_print!("\n"));
    ($($arg:tt)*) => ($crate::serial_print!("{}\n", format_args!($($arg)*)));
}

#[doc(hidden)]
pub fn _serial_print(args: fmt::Arguments) {
    use core::fmt::Write;
    SERIAL1.lock().write_fmt(args).unwrap();
}
