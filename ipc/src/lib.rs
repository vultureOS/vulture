//! # Filename: main.rs
//!
//! ### Description
//! IPC entry point
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

use std::sync::{Arc, Mutex};
use std::collections::HashMap;

#[derive(Clone)]
pub struct Port {
    name: String,
}

impl Port {
    pub fn new(name: &str) -> Self {
        Self { name: name.to_string() }
    }
}

static QUEUE: once_cell::sync::Lazy<Arc<Mutex<HashMap<String, Vec<Vec<u8>>>>>>
= once_cell::sync::Lazy::new(|| Arc::new(Mutex::new(HashMap::new())));

pub fn send_blocking(port: Port, msg: &[u8]) {
    let mut q = QUEUE.lock().unwrap();
    q.entry(port.name.clone()).or_default().push(msg.to_vec());
    println!("IPC send to {}", port.name);
}