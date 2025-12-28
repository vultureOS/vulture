//! # Preemptive Scheduler
//!
//! Priority-based round-robin scheduler with SMP awareness stubs.
//! Manages the process ready queue and context switching.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::process::{Pid, Priority, Process, ProcessState};
use alloc::collections::VecDeque;
use alloc::vec::Vec;
use spin::Mutex;

/// The global scheduler instance
static SCHEDULER: Mutex<Option<Scheduler>> = Mutex::new(None);

/// Scheduler implementation
pub struct Scheduler {
    /// All processes in the system
    processes: Vec<Process>,
    /// Ready queue (ordered by priority then FIFO)
    ready_queue: VecDeque<Pid>,
    /// Currently running process PID
    current_pid: Option<Pid>,
    /// Time slice (in ticks) for each process
    time_slice: u64,
    /// Current ticks remaining for the running process
    ticks_remaining: u64,
    /// Total context switches performed
    context_switches: u64,
    /// Whether the scheduler is active
    active: bool,
}

impl Scheduler {
    /// Create a new scheduler
    pub fn new() -> Self {
        let mut sched = Self {
            processes: Vec::new(),
            ready_queue: VecDeque::new(),
            current_pid: None,
            time_slice: 10, // 10 ticks = 100ms at 100Hz
            ticks_remaining: 0,
            context_switches: 0,
            active: false,
        };

        // Create the kernel init process (PID 0)
        let init = Process::init_process();
        sched.current_pid = Some(init.pid);
        sched.processes.push(init);

        sched
    }

    /// Add a process to the scheduler
    pub fn add_process(&mut self, process: Process) {
        let pid = process.pid;
        self.processes.push(process);
        self.ready_queue.push_back(pid);
    }

    /// Create and add a new process
    pub fn spawn(&mut self, name: &str, parent: Option<Pid>) -> Pid {
        let process = Process::new(name, parent);
        let pid = process.pid;
        self.add_process(process);
        pid
    }

    /// Get the currently running process
    pub fn current(&self) -> Option<&Process> {
        self.current_pid.and_then(|pid| self.get_process(pid))
    }

    /// Get a process by PID
    pub fn get_process(&self, pid: Pid) -> Option<&Process> {
        self.processes.iter().find(|p| p.pid == pid)
    }

    /// Get a mutable process by PID
    pub fn get_process_mut(&mut self, pid: Pid) -> Option<&mut Process> {
        self.processes.iter_mut().find(|p| p.pid == pid)
    }

    /// Tick the scheduler (called from timer interrupt)
    pub fn tick(&mut self) {
        if !self.active {
            return;
        }

        // Increment CPU time for current process
        if let Some(pid) = self.current_pid {
            if let Some(proc) = self.get_process_mut(pid) {
                proc.cpu_ticks += 1;
            }
        }

        // Check if time slice expired
        if self.ticks_remaining > 0 {
            self.ticks_remaining -= 1;
        }

        if self.ticks_remaining == 0 {
            self.schedule();
        }
    }

    /// Select the next process to run
    fn schedule(&mut self) {
        // Put current process back in ready queue if it's still runnable
        if let Some(current_pid) = self.current_pid {
            let is_running = self
                .processes
                .iter()
                .find(|p| p.pid == current_pid)
                .map_or(false, |p| p.state == ProcessState::Running);
            if is_running {
                if let Some(proc) = self.processes.iter_mut().find(|p| p.pid == current_pid) {
                    proc.set_state(ProcessState::Ready);
                }
                self.ready_queue.push_back(current_pid);
            }
        }

        // Find highest priority ready process
        if let Some(next_pid) = self.pick_next() {
            let priority = {
                if let Some(proc) = self.processes.iter_mut().find(|p| p.pid == next_pid) {
                    proc.set_state(ProcessState::Running);
                    proc.priority
                } else {
                    Priority::Normal
                }
            };
            self.current_pid = Some(next_pid);
            self.ticks_remaining = self.time_slice_for_priority(priority);
            self.context_switches += 1;
        }
    }

    /// Pick the next process from the ready queue (highest priority first)
    fn pick_next(&mut self) -> Option<Pid> {
        if self.ready_queue.is_empty() {
            return self.current_pid;
        }

        // Find highest priority process in the ready queue
        let mut best_idx = 0;
        let mut best_priority = Priority::Idle;

        for (i, &pid) in self.ready_queue.iter().enumerate() {
            if let Some(proc) = self.get_process(pid) {
                if proc.priority >= best_priority && proc.state == ProcessState::Ready {
                    best_priority = proc.priority;
                    best_idx = i;
                }
            }
        }

        self.ready_queue.remove(best_idx)
    }

    /// Get time slice based on priority
    fn time_slice_for_priority(&self, priority: Priority) -> u64 {
        match priority {
            Priority::Idle => 1,
            Priority::Low => 5,
            Priority::Normal => 10,
            Priority::High => 20,
            Priority::Realtime => 50,
        }
    }

    /// Kill a process
    pub fn kill(&mut self, pid: Pid, exit_code: i32) -> bool {
        if let Some(proc) = self.get_process_mut(pid) {
            proc.exit(exit_code);
            // Remove from ready queue
            self.ready_queue.retain(|&p| p != pid);
            true
        } else {
            false
        }
    }

    /// Block the current process
    pub fn block_current(&mut self) {
        if let Some(pid) = self.current_pid {
            if let Some(proc) = self.get_process_mut(pid) {
                proc.set_state(ProcessState::Blocked);
            }
            self.schedule();
        }
    }

    /// Unblock a process
    pub fn unblock(&mut self, pid: Pid) {
        if let Some(proc) = self.get_process_mut(pid) {
            if proc.state == ProcessState::Blocked {
                proc.set_state(ProcessState::Ready);
                self.ready_queue.push_back(pid);
            }
        }
    }

    /// Set the scheduler as active
    pub fn activate(&mut self) {
        self.active = true;
        self.ticks_remaining = self.time_slice;
    }

    /// Get all process info for `ps` command
    pub fn list_processes(&self) -> Vec<(Pid, &str, ProcessState, Priority, u64)> {
        self.processes
            .iter()
            .filter(|p| p.state != ProcessState::Zombie)
            .map(|p| (p.pid, p.name.as_str(), p.state, p.priority, p.cpu_ticks))
            .collect()
    }

    /// Get total context switches
    pub fn context_switch_count(&self) -> u64 {
        self.context_switches
    }

    /// Clean up zombie processes
    pub fn reap_zombies(&mut self) {
        self.processes.retain(|p| p.state != ProcessState::Zombie);
    }
}

// ─── Global Interface ───────────────────────────────────────────────────────

/// Initialize the global scheduler
pub fn init() {
    let sched = Scheduler::new();
    *SCHEDULER.lock() = Some(sched);
}

/// Activate the scheduler
pub fn activate() {
    if let Some(ref mut sched) = *SCHEDULER.lock() {
        sched.activate();
    }
}

/// Spawn a new process
pub fn spawn(name: &str, parent: Option<Pid>) -> Option<Pid> {
    SCHEDULER.lock().as_mut().map(|s| s.spawn(name, parent))
}

/// Timer tick
pub fn tick() {
    if let Some(ref mut sched) = *SCHEDULER.lock() {
        sched.tick();
    }
}

/// Kill a process
pub fn kill(pid: Pid, exit_code: i32) -> bool {
    SCHEDULER
        .lock()
        .as_mut()
        .map_or(false, |s| s.kill(pid, exit_code))
}

/// List all processes
pub fn list_processes() -> Vec<(Pid, alloc::string::String, ProcessState, Priority, u64)> {
    if let Some(ref sched) = *SCHEDULER.lock() {
        sched
            .list_processes()
            .iter()
            .map(|(pid, name, state, priority, ticks)| {
                (
                    *pid,
                    alloc::string::String::from(*name),
                    *state,
                    *priority,
                    *ticks,
                )
            })
            .collect()
    } else {
        Vec::new()
    }
}
