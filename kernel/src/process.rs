//! # Process Model
//!
//! Implements the process/thread model with Process Control Blocks (PCBs),
//! PID allocation, and process state management.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;

/// Process ID type
pub type Pid = u64;

/// Process states
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessState {
    /// Process is ready to be scheduled
    Ready,
    /// Process is currently running
    Running,
    /// Process is blocked waiting for an event
    Blocked,
    /// Process has been suspended
    Suspended,
    /// Process has exited and is awaiting cleanup
    Zombie,
}

/// Process priority levels
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Priority {
    Idle = 0,
    Low = 1,
    Normal = 2,
    High = 3,
    Realtime = 4,
}

/// Process Control Block
#[derive(Debug)]
pub struct Process {
    /// Process ID
    pub pid: Pid,
    /// Parent process ID
    pub parent_pid: Option<Pid>,
    /// Process name
    pub name: String,
    /// Current state
    pub state: ProcessState,
    /// Priority level
    pub priority: Priority,
    /// Exit code (set when process exits)
    pub exit_code: Option<i32>,
    /// CPU time used (in ticks)
    pub cpu_ticks: u64,
    /// Creation timestamp (in kernel ticks)
    pub created_at: u64,
    /// User ID
    pub uid: u32,
    /// Group ID
    pub gid: u32,
    /// Working directory
    pub cwd: String,
    /// Open file descriptors
    pub open_fds: Vec<FileDescriptor>,
    /// Capability flags for security
    pub capabilities: u64,
}

/// File descriptor entry
#[derive(Debug, Clone)]
pub struct FileDescriptor {
    pub fd: u32,
    pub path: String,
    pub flags: u32,
    pub offset: u64,
}

/// Next PID counter
static NEXT_PID: Mutex<Pid> = Mutex::new(1);

impl Process {
    /// Create a new process
    pub fn new(name: &str, parent: Option<Pid>) -> Self {
        let pid = {
            let mut next = NEXT_PID.lock();
            let p = *next;
            *next += 1;
            p
        };

        Self {
            pid,
            parent_pid: parent,
            name: String::from(name),
            state: ProcessState::Ready,
            priority: Priority::Normal,
            exit_code: None,
            cpu_ticks: 0,
            created_at: crate::interrupts::get_ticks(),
            uid: 0,
            gid: 0,
            cwd: String::from("/"),
            open_fds: Vec::new(),
            capabilities: 0xFFFFFFFF, // All capabilities for now
        }
    }

    /// Create the init process (PID 1)
    pub fn init_process() -> Self {
        Self {
            pid: 0,
            parent_pid: None,
            name: String::from("kernel"),
            state: ProcessState::Running,
            priority: Priority::Realtime,
            exit_code: None,
            cpu_ticks: 0,
            created_at: 0,
            uid: 0,
            gid: 0,
            cwd: String::from("/"),
            open_fds: Vec::new(),
            capabilities: 0xFFFFFFFFFFFFFFFF,
        }
    }

    /// Transition to a new state
    pub fn set_state(&mut self, new_state: ProcessState) {
        self.state = new_state;
    }

    /// Mark the process as exited
    pub fn exit(&mut self, code: i32) {
        self.state = ProcessState::Zombie;
        self.exit_code = Some(code);
    }

    /// Add a file descriptor
    pub fn open_file(&mut self, path: &str, flags: u32) -> u32 {
        let fd = self.open_fds.len() as u32 + 3; // 0,1,2 are stdin/stdout/stderr
        self.open_fds.push(FileDescriptor {
            fd,
            path: String::from(path),
            flags,
            offset: 0,
        });
        fd
    }

    /// Close a file descriptor
    pub fn close_file(&mut self, fd: u32) -> bool {
        if let Some(pos) = self.open_fds.iter().position(|f| f.fd == fd) {
            self.open_fds.remove(pos);
            true
        } else {
            false
        }
    }
}

/// Execute a function in Ring 3 (User Mode)
///
/// This constructs an `iretq` stack frame to safely drop privileges
/// from Kernel Mode (Ring 0) to User Mode (Ring 3).
///
/// # Safety
/// The instruction pointer and stack pointer must be valid mapped memory.
pub unsafe fn execute_ring3(instruction_ptr: u64, stack_ptr: u64) -> ! {
    let selectors = crate::gdt::get_selectors();

    // x86_64 User Data Segment requires RPL=3
    let user_data = selectors.user_data.0 | 3;
    let user_code = selectors.user_code.0 | 3;

    core::arch::asm!(
        "mov ds, ax",
        "mov es, ax",
        "mov fs, ax",
        "mov gs, ax",

        // Push arguments for IRETQ
        "push rax",      // SS (Stack Segment)
        "push rsi",      // RSP (Stack Pointer)
        "push 0x202",    // RFLAGS (Interrupts enabled)
        "push rcx",      // CS (Code Segment)
        "push rdi",      // RIP (Instruction Pointer)

        // Clear general purpose registers to prevent data leakage
        "xor rax, rax",
        "xor rbx, rbx",
        "xor rcx, rcx",
        "xor rdx, rdx",
        "xor rsi, rsi",
        "xor rdi, rdi",
        "xor rbp, rbp",
        "xor r8, r8",
        "xor r9, r9",
        "xor r10, r10",
        "xor r11, r11",
        "xor r12, r12",
        "xor r13, r13",
        "xor r14, r14",
        "xor r15, r15",

        "iretq",

        in("ax") user_data,
        in("cx") user_code,
        in("rdi") instruction_ptr,
        in("rsi") stack_ptr,
        options(noreturn)
    );
}
