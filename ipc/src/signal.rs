//! # Signal Delivery
//!
//! POSIX-like signal delivery mechanism for inter-process signaling.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use spin::Mutex;

/// Signal numbers (POSIX-compatible subset)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Signal {
    /// Hangup
    SIGHUP = 1,
    /// Interrupt (Ctrl+C)
    SIGINT = 2,
    /// Quit
    SIGQUIT = 3,
    /// Illegal instruction
    SIGILL = 4,
    /// Abort
    SIGABRT = 6,
    /// Floating point exception
    SIGFPE = 8,
    /// Kill (cannot be caught)
    SIGKILL = 9,
    /// Segmentation violation
    SIGSEGV = 11,
    /// Broken pipe
    SIGPIPE = 13,
    /// Alarm clock
    SIGALRM = 14,
    /// Termination
    SIGTERM = 15,
    /// User-defined 1
    SIGUSR1 = 16,
    /// User-defined 2
    SIGUSR2 = 17,
    /// Child process status change
    SIGCHLD = 18,
    /// Continue
    SIGCONT = 19,
    /// Stop (cannot be caught)
    SIGSTOP = 20,
}

/// Signal disposition
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SignalDisposition {
    /// Default action for this signal
    Default,
    /// Ignore the signal
    Ignore,
    /// Call a handler (handler address stored separately)
    Handle,
    /// Terminate the process
    Terminate,
    /// Stop the process
    Stop,
    /// Continue a stopped process
    Continue,
}

/// Signal handler table for a process
pub struct SignalTable {
    dispositions: [SignalDisposition; 32],
    pending: u32, // Bitmask of pending signals
    blocked: u32, // Bitmask of blocked signals
}

impl SignalTable {
    pub const fn new() -> Self {
        Self {
            dispositions: [SignalDisposition::Default; 32],
            pending: 0,
            blocked: 0,
        }
    }

    /// Set the disposition for a signal
    pub fn set_disposition(&mut self, signal: Signal, disposition: SignalDisposition) -> bool {
        let num = signal as u32;
        // SIGKILL and SIGSTOP cannot be caught or ignored
        if num == Signal::SIGKILL as u32 || num == Signal::SIGSTOP as u32 {
            if disposition != SignalDisposition::Default {
                return false;
            }
        }
        if (num as usize) < 32 {
            self.dispositions[num as usize] = disposition;
            true
        } else {
            false
        }
    }

    /// Send a signal to this process
    pub fn send_signal(&mut self, signal: Signal) {
        let num = signal as u32;
        if num < 32 {
            self.pending |= 1 << num;
        }
    }

    /// Check for pending unblocked signals
    pub fn has_pending(&self) -> bool {
        (self.pending & !self.blocked) != 0
    }

    /// Get the next pending signal to deliver
    pub fn next_pending(&mut self) -> Option<Signal> {
        let deliverable = self.pending & !self.blocked;
        if deliverable == 0 {
            return None;
        }

        // Find the lowest-numbered pending signal
        let bit = deliverable.trailing_zeros();
        self.pending &= !(1 << bit);

        signal_from_u32(bit)
    }

    /// Block a signal
    pub fn block(&mut self, signal: Signal) {
        let num = signal as u32;
        if num < 32 {
            self.blocked |= 1 << num;
        }
    }

    /// Unblock a signal
    pub fn unblock(&mut self, signal: Signal) {
        let num = signal as u32;
        if num < 32 {
            self.blocked &= !(1 << num);
        }
    }

    /// Get the default action for a signal
    pub fn default_action(signal: Signal) -> SignalDisposition {
        match signal {
            Signal::SIGKILL
            | Signal::SIGTERM
            | Signal::SIGINT
            | Signal::SIGQUIT
            | Signal::SIGABRT
            | Signal::SIGFPE
            | Signal::SIGILL
            | Signal::SIGSEGV
            | Signal::SIGPIPE
            | Signal::SIGHUP => SignalDisposition::Terminate,
            Signal::SIGSTOP => SignalDisposition::Stop,
            Signal::SIGCONT => SignalDisposition::Continue,
            Signal::SIGCHLD | Signal::SIGALRM => SignalDisposition::Ignore,
            Signal::SIGUSR1 | Signal::SIGUSR2 => SignalDisposition::Terminate,
        }
    }
}

/// Convert a u32 to a Signal
fn signal_from_u32(num: u32) -> Option<Signal> {
    match num {
        1 => Some(Signal::SIGHUP),
        2 => Some(Signal::SIGINT),
        3 => Some(Signal::SIGQUIT),
        4 => Some(Signal::SIGILL),
        6 => Some(Signal::SIGABRT),
        8 => Some(Signal::SIGFPE),
        9 => Some(Signal::SIGKILL),
        11 => Some(Signal::SIGSEGV),
        13 => Some(Signal::SIGPIPE),
        14 => Some(Signal::SIGALRM),
        15 => Some(Signal::SIGTERM),
        16 => Some(Signal::SIGUSR1),
        17 => Some(Signal::SIGUSR2),
        18 => Some(Signal::SIGCHLD),
        19 => Some(Signal::SIGCONT),
        20 => Some(Signal::SIGSTOP),
        _ => None,
    }
}
