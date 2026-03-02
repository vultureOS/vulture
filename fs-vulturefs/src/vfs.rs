//! # Virtual Filesystem Layer (VFS)
//!
//! Provides a unified filesystem interface with mountpoints and
//! path resolution across multiple filesystem types.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

/// Filesystem type identifiers
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FsType {
    /// vultureFS (native)
    VultureFS,
    /// FAT32 (for compatibility)
    Fat32,
    /// ISO 9660 (CD/DVD)
    Iso9660,
    /// tmpfs (in-memory)
    TmpFs,
    /// devfs (device pseudo-filesystem)
    DevFs,
    /// procfs (process pseudo-filesystem)
    ProcFs,
}

/// A mount point in the VFS
#[derive(Debug, Clone)]
pub struct MountPoint {
    /// Path where this filesystem is mounted
    pub path: String,
    /// Filesystem type
    pub fs_type: FsType,
    /// Whether this mount is read-only
    pub read_only: bool,
    /// Device or source path
    pub source: String,
}

/// VFS layer managing mount points
pub struct Vfs {
    /// Mount table
    mounts: BTreeMap<String, MountPoint>,
}

impl Vfs {
    pub const fn new() -> Self {
        Self {
            mounts: BTreeMap::new(),
        }
    }

    /// Initialize VFS with default mounts
    pub fn init(&mut self) {
        // Root filesystem
        self.mount("/", FsType::VultureFS, false, "rootfs");

        // Pseudo-filesystems
        self.mount("/dev", FsType::DevFs, false, "devfs");
        self.mount("/proc", FsType::ProcFs, true, "procfs");
        self.mount("/tmp", FsType::TmpFs, false, "tmpfs");
    }

    /// Mount a filesystem at a path
    pub fn mount(&mut self, path: &str, fs_type: FsType, read_only: bool, source: &str) {
        let mp = MountPoint {
            path: String::from(path),
            fs_type,
            read_only,
            source: String::from(source),
        };
        self.mounts.insert(String::from(path), mp);
    }

    /// Unmount a filesystem
    pub fn unmount(&mut self, path: &str) -> bool {
        self.mounts.remove(path).is_some()
    }

    /// Resolve a path to its mount point
    pub fn resolve_mount(&self, path: &str) -> Option<&MountPoint> {
        // Find the longest matching prefix
        let mut best_match: Option<&MountPoint> = None;
        let mut best_len = 0;

        for (mount_path, mp) in &self.mounts {
            if path.starts_with(mount_path.as_str()) && mount_path.len() > best_len {
                best_match = Some(mp);
                best_len = mount_path.len();
            }
        }

        best_match
    }

    /// List all mount points
    pub fn list_mounts(&self) -> Vec<&MountPoint> {
        self.mounts.values().collect()
    }
}

/// Normalize a file path (resolve . and ..)
pub fn normalize_path(path: &str) -> String {
    let mut components: Vec<&str> = Vec::new();

    for component in path.split('/') {
        match component {
            "" | "." => {}
            ".." => {
                components.pop();
            }
            c => components.push(c),
        }
    }

    if components.is_empty() {
        String::from("/")
    } else {
        let mut result = String::new();
        for c in components {
            result.push('/');
            result.push_str(c);
        }
        result
    }
}
