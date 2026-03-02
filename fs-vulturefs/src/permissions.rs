//! # File Permissions
//!
//! POSIX file permissions and Access Control Lists (ACLs).
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::vec::Vec;

/// Unix-style file permissions
#[derive(Debug, Clone, Copy)]
pub struct FilePermissions {
    /// Raw permission bits (e.g., 0o755)
    pub mode: u32,
}

impl FilePermissions {
    /// Create new permissions from an octal mode
    pub const fn new(mode: u32) -> Self {
        Self { mode }
    }

    /// Owner read permission
    pub fn owner_read(&self) -> bool {
        self.mode & 0o400 != 0
    }

    /// Owner write permission
    pub fn owner_write(&self) -> bool {
        self.mode & 0o200 != 0
    }

    /// Owner execute permission
    pub fn owner_exec(&self) -> bool {
        self.mode & 0o100 != 0
    }

    /// Group read permission
    pub fn group_read(&self) -> bool {
        self.mode & 0o040 != 0
    }

    /// Group write permission
    pub fn group_write(&self) -> bool {
        self.mode & 0o020 != 0
    }

    /// Group execute permission
    pub fn group_exec(&self) -> bool {
        self.mode & 0o010 != 0
    }

    /// Other read permission
    pub fn other_read(&self) -> bool {
        self.mode & 0o004 != 0
    }

    /// Other write permission
    pub fn other_write(&self) -> bool {
        self.mode & 0o002 != 0
    }

    /// Other execute permission
    pub fn other_exec(&self) -> bool {
        self.mode & 0o001 != 0
    }

    /// Set-UID bit
    pub fn setuid(&self) -> bool {
        self.mode & 0o4000 != 0
    }

    /// Set-GID bit
    pub fn setgid(&self) -> bool {
        self.mode & 0o2000 != 0
    }

    /// Sticky bit
    pub fn sticky(&self) -> bool {
        self.mode & 0o1000 != 0
    }

    /// Check if a user has read access
    pub fn can_read(&self, uid: u32, gid: u32, file_uid: u32, file_gid: u32) -> bool {
        if uid == 0 {
            return true; // Root can read everything
        }
        if uid == file_uid {
            return self.owner_read();
        }
        if gid == file_gid {
            return self.group_read();
        }
        self.other_read()
    }

    /// Check if a user has write access
    pub fn can_write(&self, uid: u32, gid: u32, file_uid: u32, file_gid: u32) -> bool {
        if uid == 0 {
            return true;
        }
        if uid == file_uid {
            return self.owner_write();
        }
        if gid == file_gid {
            return self.group_write();
        }
        self.other_write()
    }

    /// Check if a user has execute access
    pub fn can_execute(&self, uid: u32, gid: u32, file_uid: u32, file_gid: u32) -> bool {
        if uid == 0 {
            return true;
        }
        if uid == file_uid {
            return self.owner_exec();
        }
        if gid == file_gid {
            return self.group_exec();
        }
        self.other_exec()
    }

    /// Format as a Unix permission string (e.g., "rwxr-xr--")
    pub fn to_string_repr(&self) -> [u8; 9] {
        let mut s = [b'-'; 9];
        if self.owner_read() {
            s[0] = b'r';
        }
        if self.owner_write() {
            s[1] = b'w';
        }
        if self.owner_exec() {
            s[2] = b'x';
        }
        if self.group_read() {
            s[3] = b'r';
        }
        if self.group_write() {
            s[4] = b'w';
        }
        if self.group_exec() {
            s[5] = b'x';
        }
        if self.other_read() {
            s[6] = b'r';
        }
        if self.other_write() {
            s[7] = b'w';
        }
        if self.other_exec() {
            s[8] = b'x';
        }
        s
    }
}

/// ACL entry type
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AclEntryType {
    /// Allow the specified permissions
    Allow,
    /// Deny the specified permissions
    Deny,
}

/// An Access Control List entry
#[derive(Debug, Clone)]
pub struct AclEntry {
    /// Entry type (allow/deny)
    pub entry_type: AclEntryType,
    /// Target user ID (None = applies to all)
    pub uid: Option<u32>,
    /// Target group ID (None = applies to all)
    pub gid: Option<u32>,
    /// Permission bits
    pub permissions: u32,
}

/// Access Control List
#[derive(Debug, Clone)]
pub struct Acl {
    pub entries: Vec<AclEntry>,
}

impl Acl {
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
        }
    }

    /// Add an ACL entry
    pub fn add_entry(&mut self, entry: AclEntry) {
        self.entries.push(entry);
    }

    /// Check if the ACL explicitly allows access
    pub fn check_access(&self, uid: u32, gid: u32, required: u32) -> Option<bool> {
        for entry in &self.entries {
            let matches = match (entry.uid, entry.gid) {
                (Some(u), _) if u == uid => true,
                (_, Some(g)) if g == gid => true,
                (None, None) => true,
                _ => false,
            };

            if matches {
                let has_perms = (entry.permissions & required) == required;
                match entry.entry_type {
                    AclEntryType::Allow if has_perms => return Some(true),
                    AclEntryType::Deny if has_perms => return Some(false),
                    _ => {}
                }
            }
        }
        None // No matching ACL entry found — fall through to POSIX perms
    }
}
