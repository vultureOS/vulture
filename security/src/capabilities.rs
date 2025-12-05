//! # Capability-Based Security
//!
//! Fine-grained capability tokens controlling what operations
//! each process is allowed to perform.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

/// System capabilities
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u64)]
pub enum Capability {
    /// Read files
    FileRead = 1 << 0,
    /// Write files
    FileWrite = 1 << 1,
    /// Execute programs
    FileExec = 1 << 2,
    /// Create/delete files
    FileCreate = 1 << 3,
    /// Network access
    NetConnect = 1 << 4,
    /// Bind to ports
    NetBind = 1 << 5,
    /// Raw socket access
    NetRaw = 1 << 6,
    /// Send signals to other processes
    ProcSignal = 1 << 7,
    /// Fork/spawn processes
    ProcCreate = 1 << 8,
    /// Modify process priority
    ProcPriority = 1 << 9,
    /// Access hardware devices
    DeviceAccess = 1 << 10,
    /// Mount/unmount filesystems
    FsMount = 1 << 11,
    /// Change file ownership
    FsChown = 1 << 12,
    /// Use IPC
    IpcAccess = 1 << 13,
    /// Access camera
    Camera = 1 << 14,
    /// Access microphone
    Microphone = 1 << 15,
    /// Access screen recording
    ScreenRecord = 1 << 16,
    /// Access location
    Location = 1 << 17,
    /// System administration
    SysAdmin = 1 << 18,
    /// Reboot/shutdown
    SysPower = 1 << 19,
    /// Load kernel modules
    SysModule = 1 << 20,
    /// All capabilities
    All = 0xFFFFFFFFFFFFFFFF,
}

/// Capability set for a process
#[derive(Debug, Clone, Copy)]
pub struct CapabilitySet {
    /// Bitmask of granted capabilities
    pub bits: u64,
}

impl CapabilitySet {
    /// Create a new capability set with no capabilities
    pub const fn empty() -> Self {
        Self { bits: 0 }
    }

    /// Create a full capability set (root)
    pub const fn full() -> Self {
        Self { bits: u64::MAX }
    }

    /// Create a default user capability set
    pub const fn default_user() -> Self {
        Self {
            bits: Capability::FileRead as u64
                | Capability::FileWrite as u64
                | Capability::FileExec as u64
                | Capability::NetConnect as u64
                | Capability::ProcCreate as u64
                | Capability::IpcAccess as u64,
        }
    }

    /// Grant a capability
    pub fn grant(&mut self, cap: Capability) {
        self.bits |= cap as u64;
    }

    /// Revoke a capability
    pub fn revoke(&mut self, cap: Capability) {
        self.bits &= !(cap as u64);
    }

    /// Check if a capability is granted
    pub fn has(&self, cap: Capability) -> bool {
        self.bits & (cap as u64) != 0
    }

    /// Check if all specified capabilities are granted
    pub fn has_all(&self, caps: &[Capability]) -> bool {
        caps.iter().all(|c| self.has(*c))
    }
}

/// Check if a process (by PID) has a capability
/// In Phase 1, all processes have all capabilities
pub fn check(_pid: u64, _cap: Capability) -> bool {
    true // Permissive in Phase 1
}
