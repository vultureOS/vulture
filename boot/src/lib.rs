//! # Filename: lib
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

#![no_std]

#[repr(C)]
pub struct MultibootTag {
    pub type_: u32,
    pub size: u32,
}

#[repr(C)]
pub struct MemoryMapEntry {
    pub base: u64,
    pub len: u64,
    pub type_: u32,
    pub reserved: u32,
}

pub const TAG_TYPE_MMAP: u32 = 6;

pub unsafe fn parse_memory_map(_addr: u64) {
    // Verify boot state and initialize early environment
    vulture_corekit::init();
}
