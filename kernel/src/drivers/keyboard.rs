//! # PS/2 Keyboard Driver
//!
//! Handles PS/2 keyboard scancodes and converts them to ASCII characters.
//! Maintains a key buffer for consumption by the shell.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use spin::Mutex;

/// Keyboard input buffer (circular)
const KEY_BUFFER_SIZE: usize = 256;

struct KeyBuffer {
    buffer: [u8; KEY_BUFFER_SIZE],
    read_pos: usize,
    write_pos: usize,
    count: usize,
}

impl KeyBuffer {
    const fn new() -> Self {
        Self {
            buffer: [0; KEY_BUFFER_SIZE],
            read_pos: 0,
            write_pos: 0,
            count: 0,
        }
    }

    fn push(&mut self, key: u8) {
        if self.count < KEY_BUFFER_SIZE {
            self.buffer[self.write_pos] = key;
            self.write_pos = (self.write_pos + 1) % KEY_BUFFER_SIZE;
            self.count += 1;
        }
    }

    fn pop(&mut self) -> Option<u8> {
        if self.count == 0 {
            return None;
        }
        let key = self.buffer[self.read_pos];
        self.read_pos = (self.read_pos + 1) % KEY_BUFFER_SIZE;
        self.count -= 1;
        Some(key)
    }
}

static KEY_BUF: Mutex<KeyBuffer> = Mutex::new(KeyBuffer::new());

/// Shift key state tracking
static SHIFT_PRESSED: Mutex<bool> = Mutex::new(false);
static CAPS_LOCK: Mutex<bool> = Mutex::new(false);

/// US keyboard scancode set 1 -> ASCII mapping (lowercase)
static SCANCODE_MAP: [u8; 128] = {
    let mut map = [0u8; 128];
    map[0x02] = b'1';
    map[0x03] = b'2';
    map[0x04] = b'3';
    map[0x05] = b'4';
    map[0x06] = b'5';
    map[0x07] = b'6';
    map[0x08] = b'7';
    map[0x09] = b'8';
    map[0x0A] = b'9';
    map[0x0B] = b'0';
    map[0x0C] = b'-';
    map[0x0D] = b'=';
    map[0x0E] = 0x08; // Backspace
    map[0x0F] = b'\t';
    map[0x10] = b'q';
    map[0x11] = b'w';
    map[0x12] = b'e';
    map[0x13] = b'r';
    map[0x14] = b't';
    map[0x15] = b'y';
    map[0x16] = b'u';
    map[0x17] = b'i';
    map[0x18] = b'o';
    map[0x19] = b'p';
    map[0x1A] = b'[';
    map[0x1B] = b']';
    map[0x1C] = b'\n'; // Enter
    map[0x1E] = b'a';
    map[0x1F] = b's';
    map[0x20] = b'd';
    map[0x21] = b'f';
    map[0x22] = b'g';
    map[0x23] = b'h';
    map[0x24] = b'j';
    map[0x25] = b'k';
    map[0x26] = b'l';
    map[0x27] = b';';
    map[0x28] = b'\'';
    map[0x29] = b'`';
    map[0x2B] = b'\\';
    map[0x2C] = b'z';
    map[0x2D] = b'x';
    map[0x2E] = b'c';
    map[0x2F] = b'v';
    map[0x30] = b'b';
    map[0x31] = b'n';
    map[0x32] = b'm';
    map[0x33] = b',';
    map[0x34] = b'.';
    map[0x35] = b'/';
    map[0x39] = b' '; // Space
    map
};

/// Shifted versions of keys
static SCANCODE_MAP_SHIFT: [u8; 128] = {
    let mut map = [0u8; 128];
    map[0x02] = b'!';
    map[0x03] = b'@';
    map[0x04] = b'#';
    map[0x05] = b'$';
    map[0x06] = b'%';
    map[0x07] = b'^';
    map[0x08] = b'&';
    map[0x09] = b'*';
    map[0x0A] = b'(';
    map[0x0B] = b')';
    map[0x0C] = b'_';
    map[0x0D] = b'+';
    map[0x0E] = 0x08;
    map[0x0F] = b'\t';
    map[0x10] = b'Q';
    map[0x11] = b'W';
    map[0x12] = b'E';
    map[0x13] = b'R';
    map[0x14] = b'T';
    map[0x15] = b'Y';
    map[0x16] = b'U';
    map[0x17] = b'I';
    map[0x18] = b'O';
    map[0x19] = b'P';
    map[0x1A] = b'{';
    map[0x1B] = b'}';
    map[0x1C] = b'\n';
    map[0x1E] = b'A';
    map[0x1F] = b'S';
    map[0x20] = b'D';
    map[0x21] = b'F';
    map[0x22] = b'G';
    map[0x23] = b'H';
    map[0x24] = b'J';
    map[0x25] = b'K';
    map[0x26] = b'L';
    map[0x27] = b':';
    map[0x28] = b'"';
    map[0x29] = b'~';
    map[0x2B] = b'|';
    map[0x2C] = b'Z';
    map[0x2D] = b'X';
    map[0x2E] = b'C';
    map[0x2F] = b'V';
    map[0x30] = b'B';
    map[0x31] = b'N';
    map[0x32] = b'M';
    map[0x33] = b'<';
    map[0x34] = b'>';
    map[0x35] = b'?';
    map[0x39] = b' ';
    map
};

/// Handle a raw scancode from the keyboard interrupt
pub fn handle_scancode(scancode: u8) {
    // Key release (bit 7 set)
    if scancode & 0x80 != 0 {
        let released = scancode & 0x7F;
        // Left/Right shift release
        if released == 0x2A || released == 0x36 {
            *SHIFT_PRESSED.lock() = false;
        }
        return;
    }

    // Caps Lock toggle
    if scancode == 0x3A {
        let mut caps = CAPS_LOCK.lock();
        *caps = !*caps;
        return;
    }

    // Left/Right shift press
    if scancode == 0x2A || scancode == 0x36 {
        *SHIFT_PRESSED.lock() = true;
        return;
    }

    if (scancode as usize) < 128 {
        let shift = *SHIFT_PRESSED.lock();
        let caps = *CAPS_LOCK.lock();

        let mut ascii = if shift {
            SCANCODE_MAP_SHIFT[scancode as usize]
        } else {
            SCANCODE_MAP[scancode as usize]
        };

        // Apply caps lock (only to letters)
        if caps && !shift && ascii >= b'a' && ascii <= b'z' {
            ascii -= 32; // to uppercase
        } else if caps && shift && ascii >= b'A' && ascii <= b'Z' {
            ascii += 32; // back to lowercase when both caps and shift
        }

        if ascii != 0 {
            KEY_BUF.lock().push(ascii);
        }
    }
}

/// Try to read a character from the keyboard buffer (non-blocking)
pub fn try_read_char() -> Option<u8> {
    KEY_BUF.lock().pop()
}

/// Block until a character is available
pub fn read_char() -> u8 {
    loop {
        if let Some(c) = try_read_char() {
            return c;
        }
        x86_64::instructions::hlt();
    }
}

/// Read a line of input (blocking, with echo)
pub fn read_line(buf: &mut [u8]) -> usize {
    let mut pos = 0;
    loop {
        let c = read_char();
        match c {
            b'\n' => {
                crate::print!("\n");
                return pos;
            }
            0x08 => {
                // Backspace
                if pos > 0 {
                    pos -= 1;
                    crate::print!("\x08 \x08");
                }
            }
            _ => {
                if pos < buf.len() - 1 {
                    buf[pos] = c;
                    pos += 1;
                    crate::print!("{}", c as char);
                }
            }
        }
    }
}

/// Scancode to ASCII mapping (partial)
static US_KEYMAP: [u8; 128] = [0; 128];
