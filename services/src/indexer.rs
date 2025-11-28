//! # File Indexer (Spotlight equivalent)
//!
//! Background file indexing service for instant search.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;

/// An indexed file entry
#[derive(Debug, Clone)]
pub struct IndexEntry {
    pub path: String,
    pub name: String,
    pub content_hash: u64,
    pub size: u64,
    pub indexed_at: u64,
}

/// The file indexer
pub struct FileIndexer {
    index: BTreeMap<String, IndexEntry>,
    indexed_count: u64,
}

impl FileIndexer {
    pub const fn new() -> Self {
        Self {
            index: BTreeMap::new(),
            indexed_count: 0,
        }
    }

    /// Add a file to the index
    pub fn index_file(&mut self, path: &str, name: &str, size: u64) {
        let entry = IndexEntry {
            path: String::from(path),
            name: String::from(name),
            content_hash: simple_hash(path.as_bytes()),
            size,
            indexed_at: 0,
        };
        self.index.insert(String::from(path), entry);
        self.indexed_count += 1;
    }

    /// Search the index by name (case-insensitive substring match)
    pub fn search(&self, query: &str) -> Vec<&IndexEntry> {
        let query_lower = query.to_ascii_lowercase();
        self.index
            .values()
            .filter(|e| {
                let name_lower: String = e.name.chars().map(|c| c.to_ascii_lowercase()).collect();
                name_lower.contains(&query_lower)
            })
            .collect()
    }

    /// Get index statistics
    pub fn stats(&self) -> (u64, usize) {
        (self.indexed_count, self.index.len())
    }
}

/// Simple hash function for content fingerprinting
fn simple_hash(data: &[u8]) -> u64 {
    let mut hash: u64 = 5381;
    for &byte in data {
        hash = hash.wrapping_mul(33).wrapping_add(byte as u64);
    }
    hash
}

/// Helper trait for char conversion
trait ToAsciiLowercase {
    fn to_ascii_lowercase(self) -> char;
}

impl ToAsciiLowercase for char {
    fn to_ascii_lowercase(self) -> char {
        if self >= 'A' && self <= 'Z' {
            (self as u8 + 32) as char
        } else {
            self
        }
    }
}

/// Helper for string lowercase
trait StrToLower {
    fn to_ascii_lowercase(&self) -> alloc::string::String;
}

impl StrToLower for str {
    fn to_ascii_lowercase(&self) -> alloc::string::String {
        self.chars()
            .map(|c| {
                if c >= 'A' && c <= 'Z' {
                    (c as u8 + 32) as char
                } else {
                    c
                }
            })
            .collect()
    }
}
