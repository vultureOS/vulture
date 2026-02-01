//! # System Call Interface
//!
//! Provides the system call dispatch layer for vultureOS.
//! Implements POSIX-like syscalls: write, read, exit, fork, exec, open, close.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

/// System call numbers
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u64)]
pub enum SyscallNumber {
    /// Write to a file descriptor
    Write = 1,
    /// Read from a file descriptor  
    Read = 2,
    /// Exit the current process
    Exit = 3,
    /// Fork the current process
    Fork = 4,
    /// Execute a program
    Exec = 5,
    /// Open a file
    Open = 6,
    /// Close a file descriptor
    Close = 7,
    /// Get process ID
    GetPid = 8,
    /// Get parent process ID
    GetPpid = 9,
    /// Wait for a child process
    Wait = 10,
    /// Create a directory
    Mkdir = 11,
    /// Remove a file or directory
    Unlink = 12,
    /// Get current working directory
    Getcwd = 13,
    /// Change directory
    Chdir = 14,
    /// Send a signal
    Kill = 15,
    /// Sleep for n milliseconds
    Sleep = 16,
    /// Get system uptime
    Uptime = 17,
    /// IPC send
    IpcSend = 18,
    /// IPC receive
    IpcRecv = 19,
    /// Memory map
    Mmap = 20,
    /// Memory unmap
    Munmap = 21,
}

/// Syscall result type
pub type SyscallResult = i64;

/// Errors returned by syscalls
#[derive(Debug, Clone, Copy)]
pub enum SyscallError {
    InvalidSyscall = -1,
    PermissionDenied = -2,
    NotFound = -3,
    InvalidArgument = -4,
    IoError = -5,
    OutOfMemory = -6,
    ProcessNotFound = -7,
    AlreadyExists = -8,
}

/// Dispatch a system call
pub fn dispatch(
    number: u64,
    arg1: u64,
    arg2: u64,
    arg3: u64,
    _arg4: u64,
    _arg5: u64,
) -> SyscallResult {
    match number {
        1 => sys_write(arg1 as u32, arg2 as *const u8, arg3 as usize),
        2 => sys_read(arg1 as u32, arg2 as *mut u8, arg3 as usize),
        3 => sys_exit(arg1 as i32),
        4 => sys_fork(),
        5 => sys_exec(arg1 as *const u8, arg2 as usize),
        6 => sys_open(arg1 as *const u8, arg2 as usize, arg3 as u32),
        7 => sys_close(arg1 as u32),
        8 => sys_getpid(),
        9 => sys_getppid(),
        10 => sys_wait(arg1 as i32),
        15 => sys_kill(arg1 as u64, arg2 as i32),
        16 => sys_sleep(arg1 as u64),
        17 => sys_uptime(),
        _ => SyscallError::InvalidSyscall as i64,
    }
}

/// sys_write — Write data to a file descriptor
fn sys_write(fd: u32, buf: *const u8, count: usize) -> SyscallResult {
    if buf.is_null() || count == 0 {
        return SyscallError::InvalidArgument as i64;
    }

    // stdout / stderr — write to VGA console
    if fd == 1 || fd == 2 {
        let slice = unsafe { core::slice::from_raw_parts(buf, count) };
        if let Ok(s) = core::str::from_utf8(slice) {
            crate::print!("{}", s);
        }
        return count as i64;
    }

    // TODO: Write to actual file descriptors via VFS
    SyscallError::InvalidArgument as i64
}

/// sys_read — Read data from a file descriptor
fn sys_read(fd: u32, buf: *mut u8, count: usize) -> SyscallResult {
    if buf.is_null() || count == 0 {
        return SyscallError::InvalidArgument as i64;
    }

    // stdin — read from keyboard
    if fd == 0 {
        let slice = unsafe { core::slice::from_raw_parts_mut(buf, count) };
        let len = crate::drivers::keyboard::read_line(slice);
        return len as i64;
    }

    // TODO: Read from actual file descriptors via VFS
    SyscallError::InvalidArgument as i64
}

/// sys_exit — Exit the current process
fn sys_exit(code: i32) -> SyscallResult {
    crate::println!("[syscall] Process exiting with code {}", code);
    // In a real implementation, this would terminate the process and schedule the next one
    code as i64
}

/// sys_fork — Fork the current process
fn sys_fork() -> SyscallResult {
    if let Some(pid) = crate::scheduler::spawn("forked", Some(0)) {
        pid as i64
    } else {
        SyscallError::OutOfMemory as i64
    }
}

/// sys_exec — Execute a program
fn sys_exec(path: *const u8, len: usize) -> SyscallResult {
    if path.is_null() {
        return SyscallError::InvalidArgument as i64;
    }
    let path_slice = unsafe { core::slice::from_raw_parts(path, len) };
    if let Ok(path_str) = core::str::from_utf8(path_slice) {
        crate::println!("[syscall] exec: {}", path_str);
        // TODO: Load and execute binary from filesystem
        0
    } else {
        SyscallError::InvalidArgument as i64
    }
}

/// sys_open — Open a file
fn sys_open(path: *const u8, len: usize, flags: u32) -> SyscallResult {
    if path.is_null() {
        return SyscallError::InvalidArgument as i64;
    }
    let path_slice = unsafe { core::slice::from_raw_parts(path, len) };
    if let Ok(_path_str) = core::str::from_utf8(path_slice) {
        // TODO: Open file via VFS and return fd
        let _ = flags;
        3 // Return fd 3 as placeholder
    } else {
        SyscallError::InvalidArgument as i64
    }
}

/// sys_close — Close a file descriptor
fn sys_close(fd: u32) -> SyscallResult {
    if fd < 3 {
        return SyscallError::InvalidArgument as i64; // Can't close stdin/stdout/stderr
    }
    // TODO: Close file via VFS
    0
}

/// sys_getppid — Get parent process ID
fn sys_getppid() -> SyscallResult {
    0 // Kernel process for now
}

/// sys_wait — Wait for a child process to exit
fn sys_wait(_pid_req: i32) -> SyscallResult {
    -1 // No children for now
}

/// sys_kill — Send a signal to a process
fn sys_kill(pid: u64, sig: i32) -> SyscallResult {
    crate::println!("[syscall] kill: pid {} with signal {}", pid, sig);
    0
}

/// sys_sleep — Sleep for n milliseconds
fn sys_sleep(ms: u64) -> SyscallResult {
    crate::println!("[syscall] sleep: {} ms", ms);
    0
}

/// sys_getpid — Get current process ID
fn sys_getpid() -> SyscallResult {
    0 // Kernel process for now
}

/// sys_uptime — Get system uptime in ticks
fn sys_uptime() -> SyscallResult {
    crate::interrupts::get_ticks() as i64
}
