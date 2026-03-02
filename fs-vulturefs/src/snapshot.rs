//! # Filesystem Snapshots
//!
//! APFS-like snapshot management for vultureFS.
//! Captures the state of the filesystem at a point in time.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::inode::Inode;
use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

/// A filesystem snapshot
#[derive(Debug, Clone)]
pub struct Snapshot {
    /// Snapshot name
    pub name: String,
    /// Snapshot ID
    pub id: u64,
    /// Timestamp (kernel ticks)
    pub created_at: u64,
    /// Number of inodes captured
    pub inode_count: usize,
    /// Total data size
    pub total_size: u64,
    /// Captured inode state (path -> cloned inode)
    pub inodes: BTreeMap<String, Inode>,
}

/// Manages filesystem snapshots
pub struct SnapshotManager {
    snapshots: Vec<Snapshot>,
    next_id: u64,
}

impl SnapshotManager {
    pub const fn new() -> Self {
        Self {
            snapshots: Vec::new(),
            next_id: 1,
        }
    }

    /// Create a new snapshot from the current filesystem state
    pub fn create(&mut self, name: &str, current_inodes: &BTreeMap<String, Inode>) {
        let mut total_size = 0u64;
        let mut cloned_inodes = BTreeMap::new();

        for (path, inode) in current_inodes {
            total_size += inode.size;
            cloned_inodes.insert(path.clone(), inode.clone());
        }

        let snapshot = Snapshot {
            name: String::from(name),
            id: self.next_id,
            created_at: 0, // TODO: get from kernel ticks
            inode_count: cloned_inodes.len(),
            total_size,
            inodes: cloned_inodes,
        };

        self.next_id += 1;
        self.snapshots.push(snapshot);
    }

    /// List all snapshots
    pub fn list(&self) -> &[Snapshot] {
        &self.snapshots
    }

    /// Get a specific snapshot by name
    pub fn get(&self, name: &str) -> Option<&Snapshot> {
        self.snapshots.iter().find(|s| s.name == name)
    }

    /// Delete a snapshot
    pub fn delete(&mut self, name: &str) -> bool {
        if let Some(pos) = self.snapshots.iter().position(|s| s.name == name) {
            self.snapshots.remove(pos);
            true
        } else {
            false
        }
    }

    /// Get the number of snapshots
    pub fn count(&self) -> usize {
        self.snapshots.len()
    }

    /// Restore filesystem from a snapshot
    pub fn restore(&self, name: &str) -> Option<BTreeMap<String, Inode>> {
        self.get(name).map(|s| s.inodes.clone())
    }
}
