//! # Inode
//!
//! Filesystem inode structure with metadata, extended attributes,
//! and data block management.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::permissions::FilePermissions;
use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

/// Inode types
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InodeType {
    RegularFile,
    Directory,
    Symlink,
    CharDevice,
    BlockDevice,
    Pipe,
    Socket,
}

/// An inode representing a filesystem object
#[derive(Debug, Clone)]
pub struct Inode {
    /// Inode number
    pub ino: u64,
    /// Type of filesystem object
    pub inode_type: InodeType,
    /// File name/path
    pub name: String,
    /// File size in bytes
    pub size: u64,
    /// File data (inline for small files)
    pub data: Vec<u8>,
    /// Hard link count
    pub link_count: u32,
    /// Owner user ID
    pub uid: u32,
    /// Owner group ID
    pub gid: u32,
    /// File permissions
    pub permissions: FilePermissions,
    /// Creation time (kernel ticks)
    pub created_at: u64,
    /// Last modified time (kernel ticks)
    pub modified_at: u64,
    /// Last accessed time (kernel ticks)
    pub accessed_at: u64,
    /// Extended attributes
    pub xattrs: BTreeMap<String, Vec<u8>>,
    /// Copy-on-write clone source (inode number)
    pub cow_source: Option<u64>,
    /// Whether this inode has been modified since last snapshot
    pub dirty: bool,
}

impl Inode {
    /// Create a new inode
    pub fn new(ino: u64, inode_type: InodeType, name: &str) -> Self {
        let default_perms = match inode_type {
            InodeType::Directory => FilePermissions::new(0o755),
            InodeType::RegularFile => FilePermissions::new(0o644),
            _ => FilePermissions::new(0o644),
        };

        Self {
            ino,
            inode_type,
            name: String::from(name),
            size: 0,
            data: Vec::new(),
            link_count: 1,
            uid: 0,
            gid: 0,
            permissions: default_perms,
            created_at: 0,
            modified_at: 0,
            accessed_at: 0,
            xattrs: BTreeMap::new(),
            cow_source: None,
            dirty: false,
        }
    }

    /// Set an extended attribute
    pub fn set_xattr(&mut self, key: &str, value: &[u8]) {
        self.xattrs.insert(String::from(key), Vec::from(value));
        self.dirty = true;
    }

    /// Get an extended attribute
    pub fn get_xattr(&self, key: &str) -> Option<&Vec<u8>> {
        self.xattrs.get(key)
    }

    /// Remove an extended attribute
    pub fn remove_xattr(&mut self, key: &str) -> bool {
        self.xattrs.remove(key).is_some()
    }

    /// Create a CoW clone of this inode
    pub fn clone_cow(&self, new_ino: u64) -> Self {
        let mut clone = self.clone();
        clone.ino = new_ino;
        clone.cow_source = Some(self.ino);
        clone.link_count = 1;
        clone
    }

    /// Check if this is a directory
    pub fn is_dir(&self) -> bool {
        self.inode_type == InodeType::Directory
    }

    /// Check if this is a regular file
    pub fn is_file(&self) -> bool {
        self.inode_type == InodeType::RegularFile
    }
}
