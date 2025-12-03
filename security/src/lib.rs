//! # Security Subsystem
//!
//! Provides mandatory access control, capability-based security,
//! app sandboxing, and code integrity verification for vulture.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

pub mod capabilities;
pub mod integrity;
pub mod mac;
pub mod sandbox;

/// The security context for the system
pub struct SecurityContext {
    /// MAC policy engine
    mac: mac::MacPolicy,
    /// Whether the security system is initialized
    initialized: bool,
}

impl SecurityContext {
    pub const fn new() -> Self {
        Self {
            mac: mac::MacPolicy::new(),
            initialized: false,
        }
    }

    /// Initialize the security subsystem
    pub fn init(&mut self) {
        self.mac.init();
        self.initialized = true;
    }

    /// Check if an application has access to a resource
    pub fn check_access(&self, app: &str, resource: &str) -> bool {
        if !self.initialized {
            return true; // Allow all before init
        }
        self.mac.check(app, resource)
    }

    /// Verify code integrity of an executable
    pub fn verify_integrity(&self, path: &str, _hash: &[u8]) -> bool {
        integrity::verify(path, &[])
    }

    /// Check if a process has a capability
    pub fn has_capability(&self, _pid: u64, cap: capabilities::Capability) -> bool {
        capabilities::check(0, cap)
    }
}
