//! # App/Daemon Launcher
//!
//! Launch agent equivalent to macOS launchd.
//! Manages starting, stopping, and monitoring daemons.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;

/// Launch policy
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LaunchPolicy {
    /// Start on boot
    OnBoot,
    /// Start on demand (when IPC port is connected)
    OnDemand,
    /// Start on schedule
    Scheduled,
    /// Manual start only
    Manual,
}

/// A launch agent configuration
#[derive(Debug, Clone)]
pub struct LaunchAgent {
    pub label: String,
    pub program: String,
    pub arguments: Vec<String>,
    pub policy: LaunchPolicy,
    pub keep_alive: bool,
    pub run_at_load: bool,
    pub working_directory: String,
    pub environment: Vec<(String, String)>,
}

impl LaunchAgent {
    pub fn new(label: &str, program: &str) -> Self {
        Self {
            label: String::from(label),
            program: String::from(program),
            arguments: Vec::new(),
            policy: LaunchPolicy::OnDemand,
            keep_alive: false,
            run_at_load: false,
            working_directory: String::from("/"),
            environment: Vec::new(),
        }
    }

    /// Set as boot-time service
    pub fn on_boot(mut self) -> Self {
        self.policy = LaunchPolicy::OnBoot;
        self.run_at_load = true;
        self
    }

    /// Set keep-alive (restart on crash)
    pub fn keep_alive(mut self) -> Self {
        self.keep_alive = true;
        self
    }
}
