//! # App Sandboxing
//!
//! Application sandboxing with configurable permission profiles.
//! Restricts app access to filesystem, network, devices, and IPC.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::capabilities::{Capability, CapabilitySet};
use alloc::string::String;
use alloc::vec::Vec;

/// Sandbox profile defining what an app can do
#[derive(Debug, Clone)]
pub struct SandboxProfile {
    /// Profile name
    pub name: String,
    /// Allowed filesystem paths (read)
    pub read_paths: Vec<String>,
    /// Allowed filesystem paths (write)
    pub write_paths: Vec<String>,
    /// Capability set
    pub capabilities: CapabilitySet,
    /// Allow network access
    pub allow_network: bool,
    /// Allow IPC
    pub allow_ipc: bool,
    /// Allow hardware device access
    pub allow_devices: bool,
    /// Maximum memory (bytes)
    pub max_memory: u64,
    /// Maximum open file descriptors
    pub max_fds: u32,
    /// Maximum processes/threads
    pub max_processes: u32,
}

impl SandboxProfile {
    /// Create a restrictive sandbox profile
    pub fn restrictive(name: &str) -> Self {
        Self {
            name: String::from(name),
            read_paths: Vec::new(),
            write_paths: Vec::new(),
            capabilities: CapabilitySet::empty(),
            allow_network: false,
            allow_ipc: false,
            allow_devices: false,
            max_memory: 64 * 1024 * 1024, // 64 MB
            max_fds: 32,
            max_processes: 4,
        }
    }

    /// Create a standard app sandbox profile
    pub fn standard(name: &str) -> Self {
        let mut profile = Self::restrictive(name);
        profile.capabilities = CapabilitySet::default_user();
        profile.allow_network = true;
        profile.allow_ipc = true;
        profile.max_memory = 256 * 1024 * 1024; // 256 MB
        profile.max_fds = 256;
        profile.max_processes = 16;

        // Allow access to user's home directory
        profile.read_paths.push(String::from("/Users/"));
        profile.write_paths.push(String::from("/Users/"));
        profile.read_paths.push(String::from("/tmp/"));
        profile.write_paths.push(String::from("/tmp/"));

        profile
    }

    /// Create an unrestricted profile (for system processes)
    pub fn unrestricted(name: &str) -> Self {
        let mut read_paths = Vec::new();
        read_paths.push(String::from("/"));
        let mut write_paths = Vec::new();
        write_paths.push(String::from("/"));
        Self {
            name: String::from(name),
            read_paths,
            write_paths,
            capabilities: CapabilitySet::full(),
            allow_network: true,
            allow_ipc: true,
            allow_devices: true,
            max_memory: u64::MAX,
            max_fds: u32::MAX,
            max_processes: u32::MAX,
        }
    }

    /// Check if a path is readable under this profile
    pub fn can_read(&self, path: &str) -> bool {
        self.read_paths.iter().any(|p| path.starts_with(p.as_str()))
    }

    /// Check if a path is writable under this profile
    pub fn can_write(&self, path: &str) -> bool {
        self.write_paths
            .iter()
            .any(|p| path.starts_with(p.as_str()))
    }
}

/// Permission prompt types (macOS-style permission dialogs)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PermissionPrompt {
    Camera,
    Microphone,
    Location,
    Files,
    ScreenRecording,
    Contacts,
    Calendar,
    Network,
}

/// Permission state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PermissionState {
    NotDetermined,
    Granted,
    Denied,
    Restricted,
}
