//! # Per-Process Address Space
//!
//! Manages virtual address spaces for each process with ASLR support.
//! Each process gets its own page table hierarchy.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::frame::PhysFrame;
use crate::paging::{PageFlags, PageTable, VirtAddr};

/// Memory region types
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RegionType {
    /// Executable code
    Code,
    /// Read-only data
    RoData,
    /// Read-write data
    Data,
    /// Heap
    Heap,
    /// Stack
    Stack,
    /// Memory-mapped file
    Mmap,
    /// Shared memory (IPC)
    Shared,
    /// Kernel space (not user-accessible)
    Kernel,
}

/// A virtual memory region within an address space
#[derive(Debug, Clone)]
pub struct MemoryRegion {
    /// Start virtual address
    pub start: u64,
    /// End virtual address (exclusive)
    pub end: u64,
    /// Region type
    pub region_type: RegionType,
    /// Page flags for this region
    pub flags: PageFlags,
    /// Human-readable name (e.g., "[heap]", "[stack]")
    pub name: &'static str,
}

impl MemoryRegion {
    pub fn new(
        start: u64,
        end: u64,
        region_type: RegionType,
        flags: PageFlags,
        name: &'static str,
    ) -> Self {
        Self {
            start,
            end,
            region_type,
            flags,
            name,
        }
    }

    /// Size of the region in bytes
    pub fn size(&self) -> u64 {
        self.end - self.start
    }

    /// Check if an address falls within this region
    pub fn contains(&self, addr: u64) -> bool {
        addr >= self.start && addr < self.end
    }
}

/// Per-process virtual address space
pub struct AddressSpace {
    /// The root page table (PML4) frame
    pub root_table: PhysFrame,
    /// ASLR random offset
    pub aslr_offset: u64,
    /// Memory regions
    pub regions: [Option<MemoryRegion>; MAX_REGIONS],
    /// Number of active regions
    pub region_count: usize,
}

const MAX_REGIONS: usize = 32;

impl AddressSpace {
    /// Create a new empty address space
    pub fn new(root_frame: PhysFrame) -> Self {
        Self {
            root_table: root_frame,
            aslr_offset: 0, // TODO: randomize with ASLR
            regions: [const { None }; MAX_REGIONS],
            region_count: 0,
        }
    }

    /// Create a kernel address space
    pub fn kernel() -> Self {
        Self {
            root_table: PhysFrame::from_number(0), // Kernel uses identity mapping
            aslr_offset: 0,
            regions: [const { None }; MAX_REGIONS],
            region_count: 0,
        }
    }

    /// Add a memory region
    pub fn add_region(&mut self, region: MemoryRegion) -> bool {
        if self.region_count >= MAX_REGIONS {
            return false;
        }

        // Check for overlaps
        for existing in self.regions.iter().flatten() {
            if region.start < existing.end && region.end > existing.start {
                return false; // Overlap
            }
        }

        self.regions[self.region_count] = Some(region);
        self.region_count += 1;
        true
    }

    /// Find the region containing a virtual address
    pub fn find_region(&self, addr: u64) -> Option<&MemoryRegion> {
        for region in self.regions.iter().flatten() {
            if region.contains(addr) {
                return Some(region);
            }
        }
        None
    }

    /// Remove a region by start address
    pub fn remove_region(&mut self, start: u64) -> bool {
        for i in 0..self.region_count {
            if let Some(ref region) = self.regions[i] {
                if region.start == start {
                    self.regions[i] = None;
                    // Compact the array
                    for j in i..self.region_count - 1 {
                        self.regions[j] = self.regions[j + 1].take();
                    }
                    self.region_count -= 1;
                    return true;
                }
            }
        }
        false
    }

    /// Create a default user address space layout
    pub fn default_user_layout(root_frame: PhysFrame) -> Self {
        let mut space = Self::new(root_frame);

        // Code region: 0x400000 - 0x800000 (4 MB)
        space.add_region(MemoryRegion::new(
            0x0040_0000,
            0x0080_0000,
            RegionType::Code,
            PageFlags::user(),
            "[code]",
        ));

        // Data region: 0x800000 - 0xC00000 (4 MB)
        space.add_region(MemoryRegion::new(
            0x0080_0000,
            0x00C0_0000,
            RegionType::Data,
            PageFlags::user(),
            "[data]",
        ));

        // Heap: grows up from 0x1000_0000
        space.add_region(MemoryRegion::new(
            0x1000_0000,
            0x4000_0000,
            RegionType::Heap,
            PageFlags::user(),
            "[heap]",
        ));

        // Stack: grows down from 0x7FFF_FFFF_0000
        space.add_region(MemoryRegion::new(
            0x7FFF_FFF0_0000,
            0x7FFF_FFFF_0000,
            RegionType::Stack,
            PageFlags::user(),
            "[stack]",
        ));

        space
    }
}
