//! # Filename: main.rs
//!
//! ### Description
//! memory manager entry point
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

use std::collections::HashMap;

#[derive(Clone)]
pub struct Page {
    pub data: Vec<u8>,
    pub refcount: usize,
}

pub struct MemoryManager {
    pages: HashMap<u64, Page>,
}

impl MemoryManager {
    pub fn new() -> Self {
        Self {
            pages: HashMap::new(),
        }
    }

    pub fn init(&mut self) {
        println!("MemoryManager initialized (virtual paging)");
    }

    pub fn alloc_page(&mut self, addr: u64) {
        self.pages.insert(addr, Page { data: vec![0; 4096], refcount: 1 });
    }

    pub fn copy_on_write(&mut self, addr: u64) {
        if let Some(p) = self.pages.get_mut(&addr) {
            if p.refcount > 1 {
                p.refcount -= 1;
                self.alloc_page(addr + 0x1000);
            }
        }
    }
}