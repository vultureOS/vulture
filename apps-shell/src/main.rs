//! # Filename: main.rs
//!
//! ### Description
//! Entry point of apps shell
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

use vulture_fs::VultureFS;
use std::io::{self, Write};

fn main() {
    let mut fs = VultureFS::new();
    fs.init_root_fs();

    println!("pranaOS Shell");
    println!("Commands: ls, cat, mkdir, touch, echo, exit");

    loop {
        print!("$ ");
        io::stdout().flush().unwrap();

        let mut input = String::new();
        io::stdin().read_line(&mut input).unwrap();
        let cmd = input.trim();

        match cmd {
            "ls" => {
                for f in fs.list_dir("/") {
                    println!("{}", f);
                }
            }
            "touch file.txt" => {
                fs.write_file("/file.txt", b"");
                println!("created file.txt");
            }
            "echo hello > file.txt" => {
                fs.write_file("/file.txt", b"hello");
                println!("written");
            }
            "cat file.txt" => {
                if let Some(data) = fs.read_file("/file.txt") {
                    println!("{}", String::from_utf8_lossy(data));
                }
            }
            "exit" => break,
            _ => println!("Unknown command"),
        }
    }
}