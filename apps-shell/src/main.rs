//! # vultureOS Shell (Standalone Test Binary)
//!
//! This is the standalone test version of the shell.
//! The real shell runs inside the kernel (kernel/src/shell.rs).
//! This binary allows testing filesystem operations on the host OS.
//!
//! ### Legal Information
//! * **Copyright:** (C) 2022-2026 Krisna Pranav
//! * **License:** GNU General Public License v3.0 (GPL-3.0-or-later)
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use std::io::{self, Write};
use vulture_fs::VultureFS;

fn main() {
    let mut fs = VultureFS::new();
    fs.init_root_fs();

    println!("vultureOS Shell v0.1.0 (standalone test mode)");
    println!("Type 'help' for available commands.\n");

    loop {
        print!("vulture:/ $ ");
        io::stdout().flush().unwrap();

        let mut input = String::new();
        io::stdin().read_line(&mut input).unwrap();
        let input = input.trim();

        if input.is_empty() {
            continue;
        }

        let parts: Vec<&str> = input.split_whitespace().collect();
        let cmd = parts[0];
        let args = &parts[1..];

        match cmd {
            "help" => {
                println!("Available commands:");
                println!("  ls [path]     - List directory");
                println!("  cat <file>    - Show file contents");
                println!("  touch <file>  - Create empty file");
                println!("  mkdir <dir>   - Create directory");
                println!("  write <f> <d> - Write data to file");
                println!("  rm <file>     - Remove file");
                println!("  stat <file>   - File info");
                println!("  uname         - System info");
                println!("  exit          - Quit");
            }
            "ls" => {
                let path = if args.is_empty() { "/" } else { args[0] };
                let entries = fs.list_dir(path);
                for entry in entries {
                    println!("  {}", entry);
                }
            }
            "cat" => {
                if args.is_empty() {
                    println!("Usage: cat <file>");
                } else if let Some(data) = fs.read_file(args[0]) {
                    println!("{}", String::from_utf8_lossy(data));
                } else {
                    println!("File not found: {}", args[0]);
                }
            }
            "touch" => {
                if args.is_empty() {
                    println!("Usage: touch <file>");
                } else {
                    fs.write_file(args[0], &[]);
                    println!("Created: {}", args[0]);
                }
            }
            "mkdir" => {
                if args.is_empty() {
                    println!("Usage: mkdir <dir>");
                } else {
                    fs.write_file(args[0], &[]);
                    println!("Created directory: {}", args[0]);
                }
            }
            "write" => {
                if args.len() < 2 {
                    println!("Usage: write <file> <data>");
                } else {
                    let text = args[1..].join(" ");
                    fs.write_file(args[0], text.as_bytes());
                    println!("Written {} bytes to {}", text.len(), args[0]);
                }
            }
            "rm" => {
                if args.is_empty() {
                    println!("Usage: rm <file>");
                } else if fs.remove_file(args[0]) {
                    println!("Removed: {}", args[0]);
                } else {
                    println!("Not found: {}", args[0]);
                }
            }
            "stat" => {
                if args.is_empty() {
                    println!("Usage: stat <file>");
                } else if let Some(data) = fs.read_file(args[0]) {
                    println!("  Path: {}", args[0]);
                    println!("  Size: {} bytes", data.len());
                } else {
                    println!("Not found: {}", args[0]);
                }
            }
            "uname" => {
                println!("vultureOS 0.1.0 vultureKernel x86_64");
            }
            "exit" => break,
            _ => println!("Unknown command: '{}'. Type 'help'.", cmd),
        }
    }
}
