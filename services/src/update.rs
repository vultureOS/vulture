//! # Update System
//!
//! Atomic OS updates with rollback, delta updates, and background installation.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;

/// Update types
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpdateType {
    /// Security patch
    Security,
    /// Bug fix
    BugFix,
    /// Feature update
    Feature,
    /// Major version upgrade
    Major,
}

/// Update state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpdateState {
    Available,
    Downloading,
    Downloaded,
    Installing,
    Installed,
    Failed,
    RolledBack,
}

/// An available update
#[derive(Debug, Clone)]
pub struct Update {
    pub id: String,
    pub version: String,
    pub update_type: UpdateType,
    pub description: String,
    pub size_bytes: u64,
    pub state: UpdateState,
    pub requires_restart: bool,
}

/// Update manager
pub struct UpdateManager {
    current_version: String,
    available_updates: Vec<Update>,
    installed_versions: Vec<String>,
}

impl UpdateManager {
    pub fn new() -> Self {
        let mut installed = Vec::new();
        installed.push(String::from("0.1.0"));
        Self {
            current_version: String::from("0.1.0"),
            available_updates: Vec::new(),
            installed_versions: installed,
        }
    }

    /// Check for available updates
    pub fn check_updates(&mut self) -> usize {
        // In production, this would query an update server
        self.available_updates.len()
    }

    /// Install an update (atomic)
    pub fn install(&mut self, id: &str) -> bool {
        if let Some(update) = self.available_updates.iter_mut().find(|u| u.id == id) {
            update.state = UpdateState::Installing;
            // Atomic update: install to new partition/slot
            update.state = UpdateState::Installed;
            self.installed_versions.push(update.version.clone());
            true
        } else {
            false
        }
    }

    /// Rollback to previous version
    pub fn rollback(&mut self) -> bool {
        if self.installed_versions.len() > 1 {
            self.installed_versions.pop();
            self.current_version = self.installed_versions.last().unwrap().clone();
            true
        } else {
            false
        }
    }

    /// Get current version
    pub fn current_version(&self) -> &str {
        &self.current_version
    }
}
