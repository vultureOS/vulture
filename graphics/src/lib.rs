//! # PranaWindowServer — Graphics & Window System
//!
//! Compositor, framebuffer abstraction, GPU acceleration stubs,
//! and multi-display support for vultureOS.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

/// Color in RGBA format
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Color {
    pub const fn rgb(r: u8, g: u8, b: u8) -> Self {
        Self { r, g, b, a: 255 }
    }

    pub const fn rgba(r: u8, g: u8, b: u8, a: u8) -> Self {
        Self { r, g, b, a }
    }

    pub const BLACK: Self = Self::rgb(0, 0, 0);
    pub const WHITE: Self = Self::rgb(255, 255, 255);
    pub const TRANSPARENT: Self = Self::rgba(0, 0, 0, 0);
}

/// Rectangle
#[derive(Debug, Clone, Copy)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

/// Display information
#[derive(Debug, Clone)]
pub struct Display {
    pub id: u32,
    pub width: u32,
    pub height: u32,
    pub refresh_rate: u32,
    pub scale_factor: f32,
    pub primary: bool,
    pub name: String,
}

/// A window in the compositor
#[derive(Debug, Clone)]
pub struct Window {
    pub id: u64,
    pub title: String,
    pub frame: Rect,
    pub visible: bool,
    pub focused: bool,
    pub owner_pid: u64,
    pub z_order: i32,
}

/// Compositor state
pub struct Compositor {
    pub displays: Vec<Display>,
    pub windows: Vec<Window>,
    pub vsync_enabled: bool,
    pub hidpi_enabled: bool,
    next_window_id: u64,
}

impl Compositor {
    pub fn new() -> Self {
        Self {
            displays: Vec::new(),
            windows: Vec::new(),
            vsync_enabled: true,
            hidpi_enabled: true,
            next_window_id: 1,
        }
    }

    /// Initialize with detected displays
    pub fn init(&mut self) {
        // Default display (will be replaced by actual GPU detection)
        self.displays.push(Display {
            id: 0,
            width: 1920,
            height: 1080,
            refresh_rate: 60,
            scale_factor: 1.0,
            primary: true,
            name: String::from("Default Display"),
        });
    }

    /// Create a new window
    pub fn create_window(&mut self, title: &str, frame: Rect, owner_pid: u64) -> u64 {
        let id = self.next_window_id;
        self.next_window_id += 1;

        self.windows.push(Window {
            id,
            title: String::from(title),
            frame,
            visible: true,
            focused: true,
            owner_pid,
            z_order: self.windows.len() as i32,
        });

        id
    }

    /// Destroy a window
    pub fn destroy_window(&mut self, id: u64) -> bool {
        if let Some(pos) = self.windows.iter().position(|w| w.id == id) {
            self.windows.remove(pos);
            true
        } else {
            false
        }
    }

    /// Composite all visible windows (frame render)
    pub fn composite(&self) {
        // In production, render all windows back-to-front into the framebuffer
        // using GPU acceleration
    }
}

/// Framebuffer abstraction
pub struct Framebuffer {
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub bpp: u8,
    pub buffer: Vec<u8>,
}

impl Framebuffer {
    pub fn new(width: u32, height: u32) -> Self {
        let bpp = 4; // 32-bit RGBA
        let pitch = width * bpp as u32;
        let size = (pitch * height) as usize;
        Self {
            width,
            height,
            pitch,
            bpp,
            buffer: alloc::vec![0; size],
        }
    }

    /// Set a pixel
    pub fn set_pixel(&mut self, x: u32, y: u32, color: Color) {
        if x < self.width && y < self.height {
            let offset = ((y * self.pitch) + (x * self.bpp as u32)) as usize;
            if offset + 3 < self.buffer.len() {
                self.buffer[offset] = color.b;
                self.buffer[offset + 1] = color.g;
                self.buffer[offset + 2] = color.r;
                self.buffer[offset + 3] = color.a;
            }
        }
    }

    /// Fill a rectangle
    pub fn fill_rect(&mut self, rect: Rect, color: Color) {
        for y in rect.y..(rect.y + rect.height as i32) {
            for x in rect.x..(rect.x + rect.width as i32) {
                if x >= 0 && y >= 0 {
                    self.set_pixel(x as u32, y as u32, color);
                }
            }
        }
    }

    /// Clear with a color
    pub fn clear(&mut self, color: Color) {
        self.fill_rect(
            Rect {
                x: 0,
                y: 0,
                width: self.width,
                height: self.height,
            },
            color,
        );
    }
}
