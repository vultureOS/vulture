//! # VGA Text Mode Buffer
//!
//! Provides colored text output to the VGA text-mode buffer at 0xB8000.
//! Uses the bootloader's physical memory offset for correct virtual address.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use core::fmt;
use spin::Mutex;

/// VGA text buffer dimensions
const BUFFER_HEIGHT: usize = 25;
const BUFFER_WIDTH: usize = 80;
const VGA_PHYS_ADDR: u64 = 0xB8000;

/// Standard VGA colors
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Color {
    Black = 0,
    Blue = 1,
    Green = 2,
    Cyan = 3,
    Red = 4,
    Magenta = 5,
    Brown = 6,
    LightGray = 7,
    DarkGray = 8,
    LightBlue = 9,
    LightGreen = 10,
    LightCyan = 11,
    LightRed = 12,
    Pink = 13,
    Yellow = 14,
    White = 15,
}

/// A color code combining foreground and background
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct ColorCode(u8);

impl ColorCode {
    pub const fn new(foreground: Color, background: Color) -> ColorCode {
        ColorCode((background as u8) << 4 | (foreground as u8))
    }
}

/// A single character cell on screen
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
struct ScreenChar {
    ascii_character: u8,
    color_code: ColorCode,
}

/// The VGA text buffer (memory-mapped)
#[repr(transparent)]
struct Buffer {
    chars: [[ScreenChar; BUFFER_WIDTH]; BUFFER_HEIGHT],
}

/// Writer that outputs to the VGA text buffer
pub struct Writer {
    column_position: usize,
    row_position: usize,
    color_code: ColorCode,
    buffer: *mut Buffer,
}

// Safety: VGA buffer is only accessed through the WRITER mutex
unsafe impl Send for Writer {}

impl Writer {
    /// Write a single byte to the screen
    pub fn write_byte(&mut self, byte: u8) {
        match byte {
            b'\n' => self.new_line(),
            b'\r' => self.column_position = 0,
            0x08 => {
                // Backspace
                if self.column_position > 0 {
                    self.column_position -= 1;
                    self.write_char_at(self.row_position, self.column_position, b' ');
                }
            }
            byte => {
                if self.column_position >= BUFFER_WIDTH {
                    self.new_line();
                }
                let row = self.row_position;
                let col = self.column_position;
                self.write_char_at(row, col, byte);
                self.column_position += 1;
            }
        }
    }

    fn write_char_at(&mut self, row: usize, col: usize, byte: u8) {
        let sc = ScreenChar {
            ascii_character: byte,
            color_code: self.color_code,
        };
        unsafe {
            core::ptr::write_volatile(&mut (*self.buffer).chars[row][col] as *mut ScreenChar, sc);
        }
    }

    fn read_char_at(&self, row: usize, col: usize) -> ScreenChar {
        unsafe { core::ptr::read_volatile(&(*self.buffer).chars[row][col] as *const ScreenChar) }
    }

    /// Write a string to the screen
    pub fn write_string(&mut self, s: &str) {
        for byte in s.bytes() {
            match byte {
                0x20..=0x7e | b'\n' | b'\r' | 0x08 => self.write_byte(byte),
                _ => self.write_byte(0xfe),
            }
        }
    }

    fn new_line(&mut self) {
        if self.row_position < BUFFER_HEIGHT - 1 {
            self.row_position += 1;
            self.column_position = 0;
            return;
        }
        // Scroll up
        for row in 1..BUFFER_HEIGHT {
            for col in 0..BUFFER_WIDTH {
                let character = self.read_char_at(row, col);
                self.write_char_at(row - 1, col, character.ascii_character);
            }
        }
        self.clear_row(BUFFER_HEIGHT - 1);
        self.column_position = 0;
    }

    fn clear_row(&mut self, row: usize) {
        for col in 0..BUFFER_WIDTH {
            self.write_char_at(row, col, b' ');
        }
    }

    pub fn clear_screen(&mut self) {
        for row in 0..BUFFER_HEIGHT {
            self.clear_row(row);
        }
        self.row_position = 0;
        self.column_position = 0;
    }

    pub fn set_color(&mut self, foreground: Color, background: Color) {
        self.color_code = ColorCode::new(foreground, background);
    }
}

impl fmt::Write for Writer {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.write_string(s);
        Ok(())
    }
}

/// Global writer — starts with null buffer, set by init()
pub static WRITER: Mutex<Writer> = Mutex::new(Writer {
    column_position: 0,
    row_position: 0,
    color_code: ColorCode::new(Color::LightGreen, Color::Black),
    buffer: core::ptr::null_mut(),
});

/// Initialize VGA with the bootloader's physical memory offset
pub fn init(phys_mem_offset: u64) {
    let vga_virt_addr = (VGA_PHYS_ADDR + phys_mem_offset) as *mut Buffer;
    let mut writer = WRITER.lock();
    writer.buffer = vga_virt_addr;
    writer.clear_screen();
}

/// Print formatted text to the VGA buffer
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => ($crate::vga::_print(format_args!($($arg)*)));
}

/// Print formatted text with newline to the VGA buffer
#[macro_export]
macro_rules! println {
    () => ($crate::print!("\n"));
    ($($arg:tt)*) => ($crate::print!("{}\n", format_args!($($arg)*)));
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    use core::fmt::Write;
    WRITER.lock().write_fmt(args).unwrap();
}
