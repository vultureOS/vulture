//! # vultureOS Shell
//!
//! Interactive kernel-mode shell with command parsing and execution.
//! Provides basic UNIX-like commands for filesystem and process management.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;
use vulture_fs::VultureFS;
use vulture_security::SecurityContext;

/// Maximum command buffer size
const CMD_BUF_SIZE: usize = 512;

/// Current working directory
static CWD: spin::Mutex<String> = spin::Mutex::new(String::new());

/// Initialize CWD
fn init_cwd() {
    let mut cwd = CWD.lock();
    if cwd.is_empty() {
        *cwd = String::from("/");
    }
}

/// Run the interactive shell
pub fn run(fs: &mut VultureFS, security: &SecurityContext) {
    init_cwd();

    crate::println!("vultureOS Shell v0.1.0");
    crate::println!("Type 'help' for available commands.\n");

    let mut cmd_buf = [0u8; CMD_BUF_SIZE];

    loop {
        // Print prompt
        {
            let cwd = CWD.lock();
            crate::print!("vulture:{} $ ", *cwd);
        }

        // Read input
        let len = crate::drivers::keyboard::read_line(&mut cmd_buf);
        let input = core::str::from_utf8(&cmd_buf[..len]).unwrap_or("");
        let input = input.trim();

        if input.is_empty() {
            continue;
        }

        // Parse command and arguments
        let parts: Vec<&str> = input.split_whitespace().collect();
        let cmd = parts[0];
        let args = &parts[1..];

        // Check security before executing
        if !security.check_access("shell", cmd) {
            crate::println!("Permission denied: {}", cmd);
            continue;
        }

        // Dispatch command
        match cmd {
            "help" => cmd_help(),
            "ls" => cmd_ls(fs, args),
            "cd" => cmd_cd(args),
            "pwd" => cmd_pwd(),
            "cat" => cmd_cat(fs, args),
            "mkdir" => cmd_mkdir(fs, args),
            "touch" => cmd_touch(fs, args),
            "echo" => cmd_echo(fs, args),
            "rm" => cmd_rm(fs, args),
            "cp" => cmd_cp(fs, args),
            "mv" => cmd_mv(fs, args),
            "write" => cmd_write(fs, args),
            "stat" => cmd_stat(fs, args),
            "clear" => cmd_clear(),
            "ps" => cmd_ps(),
            "kill" => cmd_kill(args),
            "uptime" => cmd_uptime(),
            "uname" => cmd_uname(),
            "whoami" => cmd_whoami(),
            "id" => cmd_id(),
            "date" => cmd_date(),
            "shutdown" => cmd_shutdown(),
            "reboot" => cmd_reboot(),
            "exit" => break,
            "sysinfo" => cmd_sysinfo(fs),
            "lspci" => cmd_lspci(),
            _ => crate::println!(
                "Unknown command: '{}'. Type 'help' for available commands.",
                cmd
            ),
        }
    }
}

// ─── Command Implementations ────────────────────────────────────────────────

fn cmd_help() {
    crate::println!("vultureOS Shell — Available Commands:");
    crate::println!("  help              Show this help message");
    crate::println!("  ls [path]         List directory contents");
    crate::println!("  cd <path>         Change directory");
    crate::println!("  pwd               Print working directory");
    crate::println!("  cat <file>        Display file contents");
    crate::println!("  mkdir <path>      Create a directory");
    crate::println!("  touch <file>      Create an empty file");
    crate::println!("  echo <text>       Print text to console");
    crate::println!("  write <file> <d>  Write data to a file");
    crate::println!("  rm <path>         Remove a file");
    crate::println!("  cp <src> <dst>    Copy a file");
    crate::println!("  mv <src> <dst>    Move/rename a file");
    crate::println!("  stat <path>       Show file information");
    crate::println!("  clear             Clear the screen");
    crate::println!("  ps                List running processes");
    crate::println!("  kill <pid>        Kill a process");
    crate::println!("  uptime            Show system uptime");
    crate::println!("  uname             Show system information");
    crate::println!("  whoami            Show current user");
    crate::println!("  id                Show current user ID");
    crate::println!("  date              Show system date and time");
    crate::println!("  sysinfo           Show system status");
    crate::println!("  lspci             List PCI devices");
    crate::println!("  shutdown          Shut down the system");
    crate::println!("  reboot            Reboot the system");
    crate::println!("  exit              Exit shell");
}

fn cmd_ls(fs: &VultureFS, args: &[&str]) {
    let path = if args.is_empty() {
        CWD.lock().clone()
    } else {
        resolve_path(args[0])
    };

    if let Some(inode) = fs.stat(&path) {
        if inode.is_file() {
            let parts: Vec<&str> = path.split('/').collect();
            let name = parts.last().unwrap_or(&"");
            let name = if name.is_empty() { &path } else { *name };
            crate::println!("  {}", name);
            return;
        }
    }

    let entries = fs.list_dir(&path);
    if entries.is_empty() {
        crate::println!("(empty directory)");
    } else {
        for entry in entries {
            // Strip the prefix to show relative names
            let display = if entry.len() > path.len() {
                &entry[path.len()..]
            } else {
                &entry
            };
            let display = display.trim_start_matches('/');
            if !display.is_empty() && !display.contains('/') {
                crate::println!("  {}", display);
            }
        }
    }
}

fn cmd_cd(args: &[&str]) {
    if args.is_empty() {
        *CWD.lock() = String::from("/");
    } else {
        let target = resolve_path(args[0]);
        *CWD.lock() = target;
    }
}

fn cmd_pwd() {
    let cwd = CWD.lock();
    crate::println!("{}", *cwd);
}

fn cmd_cat(fs: &VultureFS, args: &[&str]) {
    if args.is_empty() {
        crate::println!("Usage: cat <file>");
        return;
    }
    let path = resolve_path(args[0]);
    if let Some(inode) = fs.stat(&path) {
        if inode.is_dir() {
            crate::println!("cat: {}: Is a directory", args[0]);
            return;
        }
    }
    match fs.read_file(&path) {
        Some(data) => {
            let text = core::str::from_utf8(data).unwrap_or("<binary data>");
            crate::println!("{}", text);
        }
        None => crate::println!("File not found: {}", path),
    }
}

fn cmd_mkdir(fs: &mut VultureFS, args: &[&str]) {
    if args.is_empty() {
        crate::println!("Usage: mkdir <path>");
        return;
    }
    let path = resolve_path(args[0]);
    if fs.create_dir(&path) {
        crate::println!("Created directory: {}", path);
    } else {
        crate::println!("mkdir: cannot create directory '{}': File exists", args[0]);
    }
}

fn cmd_touch(fs: &mut VultureFS, args: &[&str]) {
    if args.is_empty() {
        crate::println!("Usage: touch <file>");
        return;
    }
    let path = resolve_path(args[0]);
    fs.touch_file(&path);
    crate::println!("Created/Updated: {}", path);
}

fn cmd_echo(fs: &mut VultureFS, args: &[&str]) {
    if args.is_empty() {
        crate::println!();
        return;
    }

    // Check for redirection: echo text > file
    let mut redirect_idx = None;
    for (i, arg) in args.iter().enumerate() {
        if *arg == ">" {
            redirect_idx = Some(i);
            break;
        }
    }

    if let Some(idx) = redirect_idx {
        if idx + 1 < args.len() {
            let text: String = args[..idx].iter().copied().collect::<Vec<&str>>().join(" ");
            let path = resolve_path(args[idx + 1]);
            fs.write_file(&path, text.as_bytes());
            return;
        }
    }

    // Just echo to console
    let text: String = args.iter().copied().collect::<Vec<&str>>().join(" ");
    crate::println!("{}", text);
}

fn cmd_rm(fs: &mut VultureFS, args: &[&str]) {
    if args.is_empty() {
        crate::println!("Usage: rm <path>");
        return;
    }
    let path = resolve_path(args[0]);
    if let Some(inode) = fs.stat(&path) {
        if inode.is_dir() {
            crate::println!("rm: cannot remove '{}': Is a directory", args[0]);
            return;
        }
    }
    if fs.remove_file(&path) {
        crate::println!("Removed: {}", path);
    } else {
        crate::println!("Not found: {}", path);
    }
}

fn cmd_cp(fs: &mut VultureFS, args: &[&str]) {
    if args.len() < 2 {
        crate::println!("Usage: cp <src> <dst>");
        return;
    }
    let src_path = resolve_path(args[0]);
    let mut dst_path = resolve_path(args[1]);

    if let Some(inode) = fs.stat(&src_path) {
        if inode.is_dir() {
            crate::println!("cp: -r not specified; omitting directory '{}'", args[0]);
            return;
        }
    } else {
        crate::println!("cp: cannot stat '{}': No such file or directory", args[0]);
        return;
    }

    if let Some(inode) = fs.stat(&dst_path) {
        if inode.is_dir() {
            let src_parts: Vec<&str> = src_path.split('/').collect();
            let filename = src_parts.last().unwrap_or(&"");
            if dst_path.ends_with('/') {
                dst_path = alloc::format!("{}{}", dst_path, filename);
            } else {
                dst_path = alloc::format!("{}/{}", dst_path, filename);
            }
        }
    }

    let data_opt = fs.read_file(&src_path).cloned();
    match data_opt {
        Some(data) => {
            fs.write_file(&dst_path, &data);
            crate::println!("Copied {} to {}", args[0], args[1]);
        }
        None => crate::println!("Source file not found: {}", args[0]),
    }
}

fn cmd_mv(fs: &mut VultureFS, args: &[&str]) {
    if args.len() < 2 {
        crate::println!("Usage: mv <src> <dst>");
        return;
    }
    let src_path = resolve_path(args[0]);
    let mut dst_path = resolve_path(args[1]);

    if !fs.exists(&src_path) {
        crate::println!("mv: cannot stat '{}': No such file or directory", args[0]);
        return;
    }

    if let Some(inode) = fs.stat(&dst_path) {
        if inode.is_dir() {
            let src_parts: Vec<&str> = src_path.split('/').collect();
            let filename = src_parts.last().unwrap_or(&"");
            if dst_path.ends_with('/') {
                dst_path = alloc::format!("{}{}", dst_path, filename);
            } else {
                dst_path = alloc::format!("{}/{}", dst_path, filename);
            }
        }
    }

    if fs.rename(&src_path, &dst_path) {
        crate::println!("Moved {} to {}", args[0], args[1]);
    } else {
        crate::println!("Failed to move {} to {}", args[0], args[1]);
    }
}

fn cmd_write(fs: &mut VultureFS, args: &[&str]) {
    if args.len() < 2 {
        crate::println!("Usage: write <file> <data>");
        return;
    }
    let path = resolve_path(args[0]);
    let text: String = args[1..].iter().copied().collect::<Vec<&str>>().join(" ");
    fs.write_file(&path, text.as_bytes());
    crate::println!("Written {} bytes to {}", text.len(), path);
}

fn cmd_stat(fs: &VultureFS, args: &[&str]) {
    if args.is_empty() {
        crate::println!("Usage: stat <path>");
        return;
    }
    let path = resolve_path(args[0]);
    match fs.read_file(&path) {
        Some(data) => {
            crate::println!("  Path: {}", path);
            crate::println!("  Size: {} bytes", data.len());
        }
        None => crate::println!("Not found: {}", path),
    }
}

fn cmd_clear() {
    crate::vga::WRITER.lock().clear_screen();
}

fn cmd_ps() {
    crate::println!(
        "{:<6} {:<16} {:<10} {:<10} {:<8}",
        "PID",
        "NAME",
        "STATE",
        "PRIORITY",
        "TICKS"
    );
    crate::println!("{}", "─".repeat(54));
    let processes = crate::scheduler::list_processes();
    for (pid, name, state, priority, ticks) in processes {
        let state_str = match state {
            crate::process::ProcessState::Ready => "Ready",
            crate::process::ProcessState::Running => "Running",
            crate::process::ProcessState::Blocked => "Blocked",
            crate::process::ProcessState::Suspended => "Suspend",
            crate::process::ProcessState::Zombie => "Zombie",
        };
        let prio_str = match priority {
            crate::process::Priority::Idle => "Idle",
            crate::process::Priority::Low => "Low",
            crate::process::Priority::Normal => "Normal",
            crate::process::Priority::High => "High",
            crate::process::Priority::Realtime => "RT",
        };
        crate::println!(
            "{:<6} {:<16} {:<10} {:<10} {:<8}",
            pid,
            name,
            state_str,
            prio_str,
            ticks
        );
    }
}

fn cmd_kill(args: &[&str]) {
    if args.is_empty() {
        crate::println!("Usage: kill <pid>");
        return;
    }
    if let Ok(pid) = args[0].parse::<u64>() {
        if pid == 0 {
            crate::println!("Cannot kill the kernel process");
            return;
        }
        if crate::scheduler::kill(pid, -9) {
            crate::println!("Killed process {}", pid);
        } else {
            crate::println!("Process not found: {}", pid);
        }
    } else {
        crate::println!("Invalid PID: {}", args[0]);
    }
}

fn cmd_uptime() {
    let ticks = crate::interrupts::get_ticks();
    let seconds = ticks / 100; // 100Hz timer
    let minutes = seconds / 60;
    let hours = minutes / 60;
    crate::println!(
        "Uptime: {}h {}m {}s ({} ticks)",
        hours,
        minutes % 60,
        seconds % 60,
        ticks
    );
}

fn cmd_uname() {
    crate::println!("vultureOS 0.1.0 vultureKernel x86_64");
    crate::println!("Copyright (C) 2022-2026 Krisna Pranav");
    crate::println!("License: GPL-3.0-or-later");
}

fn cmd_whoami() {
    crate::println!("root");
}

fn cmd_id() {
    crate::println!("uid=0(root) gid=0(root) groups=0(root)");
}

fn cmd_date() {
    let mut read_rtc = |reg: u8| -> u8 {
        unsafe {
            let mut port70 = x86_64::instructions::port::Port::<u8>::new(0x70);
            let mut port71 = x86_64::instructions::port::Port::<u8>::new(0x71);
            port70.write(reg);
            port71.read()
        }
    };

    let bcd_to_bin = |bcd: u8| -> u8 { (bcd & 0x0F) + ((bcd / 16) * 10) };

    let second = bcd_to_bin(read_rtc(0x00));
    let minute = bcd_to_bin(read_rtc(0x02));
    let hour = bcd_to_bin(read_rtc(0x04));
    let day = bcd_to_bin(read_rtc(0x07));
    let month = bcd_to_bin(read_rtc(0x08));
    let year = bcd_to_bin(read_rtc(0x09));
    let century = bcd_to_bin(read_rtc(0x32)); // ACPI century

    let mut full_year = (century as u16) * 100 + (year as u16);
    if full_year < 2000 {
        full_year = 2000 + (year as u16);
    }

    crate::println!(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC",
        full_year,
        month,
        day,
        hour,
        minute,
        second
    );
}

fn cmd_sysinfo(fs: &VultureFS) {
    crate::println!("╔══════════════════════════════════════╗");
    crate::println!("║      vultureOS System Information     ║");
    crate::println!("╠══════════════════════════════════════╣");
    crate::println!("║ Kernel:    vultureKernel v0.1.0      ║");
    crate::println!("║ Arch:      x86_64                    ║");
    crate::println!("║ FS:        vultureFS (CoW)           ║");

    let ticks = crate::interrupts::get_ticks();
    let secs = ticks / 100;
    crate::println!("║ Uptime:    {} seconds              ║", secs);

    let file_count = fs.list_dir("/").len();
    crate::println!("║ FS Nodes:  {}                      ║", file_count);

    let procs = crate::scheduler::list_processes();
    crate::println!("║ Processes: {}                       ║", procs.len());

    crate::println!("╚══════════════════════════════════════╝");
}

fn cmd_shutdown() {
    crate::println!("Shutting down vultureOS...");
    crate::println!("Flushing filesystem...");
    crate::println!("Stopping services...");
    crate::println!("Goodbye.");

    crate::acpi::shutdown();
}

fn cmd_lspci() {
    crate::println!("╔════════════════════════════════════════════════════════════╗");
    crate::println!("║                      PCI Devices Bus Scan                  ║");
    crate::println!("╠════════════════════════════════════════════════════════════╣");
    crate::println!("║ BUS : DEV  | VEND:DEVC  | CLASS:SUB  | PROG               ║");
    crate::println!("╟────────────────────────────────────────────────────────────╢");

    let devices = crate::drivers::pci::PCI_DEVICES.lock();
    if devices.is_empty() {
        crate::println!("║ No PCI devices discovered.                                 ║");
    } else {
        for dev in devices.iter() {
            crate::println!(
                "║ {:02x}  : {:02x}   | {:04x}:{:04x}  | {:02x}   :{:02x}   | {:02x}                 ║",
                dev.bus, dev.device, dev.vendor_id, dev.device_id, dev.class, dev.subclass, dev.prog_if
            );
        }
    }
    crate::println!("╚════════════════════════════════════════════════════════════╝");
}

fn cmd_reboot() {
    crate::println!("Rebooting vultureOS...");

    // Attempt reset via keyboard controller
    unsafe {
        let mut port = x86_64::instructions::port::Port::<u8>::new(0x64);
        port.write(0xFE);
    }

    // Give it a moment
    for _ in 0..10_000 {
        x86_64::instructions::nop();
    }

    // Fallback: Triple fault
    crate::println!("Keyboard controller reset failed, triggering triple fault...");
    unsafe {
        use x86_64::structures::DescriptorTablePointer;
        use x86_64::VirtAddr;

        let null_idt = DescriptorTablePointer {
            limit: 0,
            base: VirtAddr::new(0),
        };
        x86_64::instructions::tables::lidt(&null_idt);
        core::arch::asm!("int3");
    }

    loop {
        x86_64::instructions::hlt();
    }
}

// ─── Utility ────────────────────────────────────────────────────────────────

/// Resolve a path relative to the current working directory
fn resolve_path(path: &str) -> String {
    if path.starts_with('/') {
        String::from(path)
    } else {
        let cwd = CWD.lock();
        if cwd.ends_with('/') {
            alloc::format!("{}{}", *cwd, path)
        } else {
            alloc::format!("{}/{}", *cwd, path)
        }
    }
}

/// Parse a u64 from a string (no_std compatible)
trait ParseU64 {
    fn parse<T: core::str::FromStr>(&self) -> Result<T, ()>;
}

impl ParseU64 for str {
    fn parse<T: core::str::FromStr>(&self) -> Result<T, ()> {
        core::str::FromStr::from_str(self).map_err(|_| ())
    }
}
