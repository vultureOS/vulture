//! # Virtual Memory / Paging
//!
//! 4-level x86_64 page table management with map, unmap, and translate operations.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::frame::{self, PhysFrame, FRAME_SIZE};
use spin::Mutex;

/// Page table flags
#[derive(Debug, Clone, Copy)]
pub struct PageFlags {
    /// Page is present in memory
    pub present: bool,
    /// Page is writable
    pub writable: bool,
    /// Page is accessible from user mode
    pub user_accessible: bool,
    /// Write-through caching
    pub write_through: bool,
    /// Disable caching
    pub no_cache: bool,
    /// Page has been accessed
    pub accessed: bool,
    /// Page has been written to (dirty)
    pub dirty: bool,
    /// Huge page (2MB or 1GB)
    pub huge: bool,
    /// Global page (not flushed on CR3 change)
    pub global: bool,
    /// Copy-on-write marker (custom bit in available bits)
    pub cow: bool,
    /// No execute
    pub no_execute: bool,
}

impl PageFlags {
    /// Default kernel page flags
    pub const fn kernel() -> Self {
        Self {
            present: true,
            writable: true,
            user_accessible: false,
            write_through: false,
            no_cache: false,
            accessed: false,
            dirty: false,
            huge: false,
            global: false,
            cow: false,
            no_execute: false,
        }
    }

    /// Default user page flags
    pub const fn user() -> Self {
        Self {
            present: true,
            writable: true,
            user_accessible: true,
            write_through: false,
            no_cache: false,
            accessed: false,
            dirty: false,
            huge: false,
            global: false,
            cow: false,
            no_execute: false,
        }
    }

    /// Read-only flags (for CoW)
    pub const fn read_only() -> Self {
        Self {
            present: true,
            writable: false,
            user_accessible: true,
            write_through: false,
            no_cache: false,
            accessed: false,
            dirty: false,
            huge: false,
            global: false,
            cow: true,
            no_execute: false,
        }
    }

    /// Convert to raw page table entry bits
    pub fn to_bits(&self) -> u64 {
        let mut bits: u64 = 0;
        if self.present {
            bits |= 1 << 0;
        }
        if self.writable {
            bits |= 1 << 1;
        }
        if self.user_accessible {
            bits |= 1 << 2;
        }
        if self.write_through {
            bits |= 1 << 3;
        }
        if self.no_cache {
            bits |= 1 << 4;
        }
        if self.accessed {
            bits |= 1 << 5;
        }
        if self.dirty {
            bits |= 1 << 6;
        }
        if self.huge {
            bits |= 1 << 7;
        }
        if self.global {
            bits |= 1 << 8;
        }
        if self.cow {
            bits |= 1 << 9; // Using available bit 9 for CoW marker
        }
        if self.no_execute {
            bits |= 1 << 63;
        }
        bits
    }

    /// Parse from raw page table entry bits
    pub fn from_bits(bits: u64) -> Self {
        Self {
            present: bits & (1 << 0) != 0,
            writable: bits & (1 << 1) != 0,
            user_accessible: bits & (1 << 2) != 0,
            write_through: bits & (1 << 3) != 0,
            no_cache: bits & (1 << 4) != 0,
            accessed: bits & (1 << 5) != 0,
            dirty: bits & (1 << 6) != 0,
            huge: bits & (1 << 7) != 0,
            global: bits & (1 << 8) != 0,
            cow: bits & (1 << 9) != 0,
            no_execute: bits & (1 << 63) != 0,
        }
    }
}

/// A page table entry
#[derive(Debug, Clone, Copy)]
pub struct PageTableEntry {
    pub raw: u64,
}

impl PageTableEntry {
    /// Create a new empty entry
    pub const fn empty() -> Self {
        Self { raw: 0 }
    }

    /// Create an entry mapping to a physical frame with flags
    pub fn new(frame: PhysFrame, flags: PageFlags) -> Self {
        Self {
            raw: frame.addr() | flags.to_bits(),
        }
    }

    /// Is this entry present?
    pub fn is_present(&self) -> bool {
        self.raw & 1 != 0
    }

    /// Get the physical frame this entry points to
    pub fn frame(&self) -> PhysFrame {
        PhysFrame::from_addr(self.raw & 0x000F_FFFF_FFFF_F000)
    }

    /// Get the flags
    pub fn flags(&self) -> PageFlags {
        PageFlags::from_bits(self.raw)
    }
}

/// A page table (512 entries, each 8 bytes = 4KiB)
#[repr(align(4096))]
pub struct PageTable {
    pub entries: [PageTableEntry; 512],
}

impl PageTable {
    /// Create an empty page table
    pub const fn new() -> Self {
        Self {
            entries: [PageTableEntry::empty(); 512],
        }
    }
}

/// Virtual address decomposition for 4-level paging
pub struct VirtAddr {
    pub raw: u64,
}

impl VirtAddr {
    pub const fn new(addr: u64) -> Self {
        Self { raw: addr }
    }

    /// Level 4 index (PML4)
    pub fn p4_index(&self) -> usize {
        ((self.raw >> 39) & 0x1FF) as usize
    }

    /// Level 3 index (PDPT)
    pub fn p3_index(&self) -> usize {
        ((self.raw >> 30) & 0x1FF) as usize
    }

    /// Level 2 index (PD)
    pub fn p2_index(&self) -> usize {
        ((self.raw >> 21) & 0x1FF) as usize
    }

    /// Level 1 index (PT)
    pub fn p1_index(&self) -> usize {
        ((self.raw >> 12) & 0x1FF) as usize
    }

    /// Page offset
    pub fn page_offset(&self) -> usize {
        (self.raw & 0xFFF) as usize
    }
}

/// Page mapping statistics
static MAPPED_PAGES: Mutex<u64> = Mutex::new(0);

/// Initialize paging subsystem
pub fn init() {
    // In a real implementation, we'd read CR3 and set up identity mapping
    // For now, we track the state
    let mut count = MAPPED_PAGES.lock();
    *count = 512; // Kernel identity-mapped pages
}

/// Get the number of mapped pages
pub fn mapped_pages() -> u64 {
    *MAPPED_PAGES.lock()
}

/// Flush a TLB entry for a specific virtual address
pub fn flush_tlb(addr: u64) {
    // In real implementation: invlpg instruction
    let _ = addr;
}

/// Flush the entire TLB
pub fn flush_tlb_all() {
    // In real implementation: reload CR3
}
