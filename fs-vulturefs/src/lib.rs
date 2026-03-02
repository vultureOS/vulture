//! # vultureFS — File System
//!
//! An APFS-inspired filesystem for vultureOS featuring copy-on-write,
//! snapshots, journaling, and POSIX-compatible permissions + ACLs.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

pub mod inode;
pub mod journal;
pub mod permissions;
pub mod snapshot;
pub mod vfs;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;
use inode::{Inode, InodeType};
use permissions::FilePermissions;
use spin::Mutex;

/// The vultureFS filesystem
pub struct VultureFS {
    /// All inodes by path
    inodes: BTreeMap<String, Inode>,
    /// Next inode number
    next_ino: u64,
    /// Journal for crash recovery
    journal: journal::Journal,
    /// Snapshots
    snapshots: snapshot::SnapshotManager,
    /// Mounted flag
    mounted: bool,
}

impl VultureFS {
    /// Create a new filesystem instance
    pub fn new() -> Self {
        Self {
            inodes: BTreeMap::new(),
            next_ino: 1,
            journal: journal::Journal::new(),
            snapshots: snapshot::SnapshotManager::new(),
            mounted: false,
        }
    }

    /// Initialize the root filesystem with the vultureOS hierarchy
    pub fn init_root_fs(&mut self) {
        // Create root directory
        self.create_inode("/", InodeType::Directory);

        // System directories (macOS-like hierarchy)
        let system_dirs = [
            "/System",
            "/System/Kernel",
            "/System/Library",
            "/System/Drivers",
            "/Applications",
            "/Users",
            "/Users/root",
            "/Users/root/Desktop",
            "/Users/root/Documents",
            "/Users/root/Downloads",
            "/Library",
            "/Library/Preferences",
            "/Library/Caches",
            "/Library/Logs",
            "/Volumes",
            "/Developer",
            "/Developer/SDK",
            "/Developer/Tools",
            "/tmp",
            "/var",
            "/var/log",
            "/var/run",
            "/etc",
        ];

        for dir in &system_dirs {
            self.create_inode(dir, InodeType::Directory);
        }

        // Create some default files
        self.write_file("/etc/hostname", b"vultureOS");
        self.write_file("/etc/version", b"0.1.0");
        self.write_file(
            "/etc/motd",
            b"Welcome to vultureOS!\nType 'help' for available commands.\n",
        );
        self.write_file(
            "/System/Kernel/version",
            b"vultureKernel 0.1.0 (x86_64)\nBuilt with Rust\nLicense: GPL-3.0-or-later\n",
        );

        // Begin journaling
        self.journal.begin_transaction("init_root_fs");
        self.journal.commit();

        self.mounted = true;
    }

    /// Create a new inode
    fn create_inode(&mut self, path: &str, inode_type: InodeType) -> u64 {
        let ino = self.next_ino;
        self.next_ino += 1;

        let inode = Inode::new(ino, inode_type, path);
        self.inodes.insert(String::from(path), inode);
        ino
    }

    /// Create a directory
    pub fn create_dir(&mut self, path: &str) -> bool {
        self.journal.begin_transaction("create_dir");
        if self.inodes.contains_key(path) {
            self.journal.commit();
            return false;
        }
        let ino = self.next_ino;
        self.next_ino += 1;
        let inode = Inode::new(ino, InodeType::Directory, path);
        self.inodes.insert(String::from(path), inode);
        self.journal.commit();
        true
    }

    /// Update timestamps or create an empty file
    pub fn touch_file(&mut self, path: &str) {
        self.journal.begin_transaction("touch_file");
        if let Some(inode) = self.inodes.get_mut(path) {
            inode.modified_at += 1;
            inode.accessed_at += 1;
            inode.dirty = true;
        } else {
            let ino = self.next_ino;
            self.next_ino += 1;
            let mut inode = Inode::new(ino, InodeType::RegularFile, path);
            inode.modified_at += 1;
            self.inodes.insert(String::from(path), inode);
        }
        self.journal.commit();
    }

    /// Write data to a file (create if doesn't exist)
    pub fn write_file(&mut self, path: &str, data: &[u8]) {
        self.journal.begin_transaction("write_file");

        if let Some(inode) = self.inodes.get_mut(path) {
            // Update existing file
            inode.data = Vec::from(data);
            inode.size = data.len() as u64;
            inode.modified_at += 1;
        } else {
            // Create new file
            let ino = self.next_ino;
            self.next_ino += 1;
            let mut inode = Inode::new(ino, InodeType::RegularFile, path);
            inode.data = Vec::from(data);
            inode.size = data.len() as u64;
            self.inodes.insert(String::from(path), inode);
        }

        self.journal.commit();
    }

    /// Read a file's contents
    pub fn read_file(&self, path: &str) -> Option<&Vec<u8>> {
        self.inodes.get(path).map(|inode| &inode.data)
    }

    /// Remove a file
    pub fn remove_file(&mut self, path: &str) -> bool {
        self.journal.begin_transaction("remove_file");
        let result = self.inodes.remove(path).is_some();
        self.journal.commit();
        result
    }

    /// List entries in a directory
    pub fn list_dir(&self, prefix: &str) -> Vec<String> {
        let prefix_normalized = if prefix.ends_with('/') {
            String::from(prefix)
        } else {
            alloc::format!("{}/", prefix)
        };

        self.inodes
            .keys()
            .filter(|k| {
                if prefix == "/" {
                    // For root, list immediate children
                    let stripped = k.trim_start_matches('/');
                    !stripped.is_empty() && !stripped.contains('/')
                } else {
                    k.starts_with(&prefix_normalized) && k.len() > prefix_normalized.len()
                }
            })
            .cloned()
            .collect()
    }

    /// Rename/move a file or directory
    pub fn rename(&mut self, src: &str, dst: &str) -> bool {
        self.journal.begin_transaction("rename");

        // Ensure src exists, dst does not
        if !self.inodes.contains_key(src) || self.inodes.contains_key(dst) {
            self.journal.commit();
            return false;
        }

        let mut to_insert = Vec::new();
        let mut to_remove = Vec::new();

        let src_prefix = if src.ends_with('/') {
            String::from(src)
        } else {
            alloc::format!("{}/", src)
        };
        let dst_prefix = if dst.ends_with('/') {
            String::from(dst)
        } else {
            alloc::format!("{}/", dst)
        };

        for (path, inode) in self.inodes.iter() {
            if path == src {
                let mut new_inode = inode.clone();
                new_inode.name = String::from(dst);
                to_insert.push((String::from(dst), new_inode));
                to_remove.push(path.clone());
            } else if path.starts_with(&src_prefix) && path.len() > src_prefix.len() {
                let suffix = &path[src_prefix.len()..];
                let new_path = alloc::format!("{}{}", dst_prefix, suffix);

                let mut new_inode = inode.clone();
                new_inode.name = new_path.clone();
                to_insert.push((new_path, new_inode));
                to_remove.push(path.clone());
            }
        }

        for p in to_remove {
            self.inodes.remove(&p);
        }
        for (p, inode) in to_insert {
            self.inodes.insert(p, inode);
        }

        self.journal.commit();
        true
    }

    /// Check if a path exists
    pub fn exists(&self, path: &str) -> bool {
        self.inodes.contains_key(path)
    }

    /// Get inode metadata for a path
    pub fn stat(&self, path: &str) -> Option<&Inode> {
        self.inodes.get(path)
    }

    /// Create a snapshot
    pub fn create_snapshot(&mut self, name: &str) {
        self.snapshots.create(name, &self.inodes);
    }

    /// Get filesystem statistics
    pub fn stats(&self) -> FsStats {
        let mut total_size = 0u64;
        let mut file_count = 0u64;
        let mut dir_count = 0u64;

        for inode in self.inodes.values() {
            match inode.inode_type {
                InodeType::RegularFile => {
                    file_count += 1;
                    total_size += inode.size;
                }
                InodeType::Directory => dir_count += 1,
                _ => {}
            }
        }

        FsStats {
            total_inodes: self.inodes.len() as u64,
            file_count,
            dir_count,
            total_size,
            journal_transactions: self.journal.transaction_count(),
            snapshot_count: self.snapshots.count(),
        }
    }
}

/// Filesystem statistics
pub struct FsStats {
    pub total_inodes: u64,
    pub file_count: u64,
    pub dir_count: u64,
    pub total_size: u64,
    pub journal_transactions: u64,
    pub snapshot_count: usize,
}
