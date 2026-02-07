//! # Filename: main.rs
//!
//! ### Description
//! file system
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

pub struct VultureFS {
    files: HashMap<String, Vec<u8>>,
}

impl VultureFS {
    pub fn new() -> Self {
        Self { files: HashMap::new() }
    }

    pub fn init_root_fs(&mut self) {
        let dirs = vec![
            "/System",
            "/Applications",
            "/Users",
            "/Users/username",
            "/Library",
            "/Volumes",
            "/Developer",
        ];

        for d in dirs {
            self.files.insert(d.to_string(), vec![]);
        }

        println!("vultureFS mounted with required hierarchy");
    }

    pub fn write_file(&mut self, path: &str, data: &[u8]) {
        self.files.insert(path.to_string(), data.to_vec());
    }

    pub fn read_file(&self, path: &str) -> Option<&Vec<u8>> {
        self.files.get(path)
    }

    pub fn list_dir(&self, prefix: &str) -> Vec<String> {
        self.files
            .keys()
            .filter(|k| k.starts_with(prefix))
            .cloned()
            .collect()
    }
}