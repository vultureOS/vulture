//! # Memory Management Subsystem
//!
//! Provides physical frame allocation, virtual memory paging, kernel heap,
//! copy-on-write, and per-process address space management for vulture.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

pub mod address_space;
pub mod cow;
pub mod frame;
pub mod heap;
pub mod paging;

/// The global memory manager
pub struct MemoryManager {
    total_memory: u64,
    used_memory: u64,
    frame_allocator_initialized: bool,
    heap_initialized: bool,
}

impl MemoryManager {
    /// Create a new memory manager
    pub const fn new() -> Self {
        Self {
            total_memory: 0,
            used_memory: 0,
            frame_allocator_initialized: false,
            heap_initialized: false,
        }
    }

    /// Initialize the full memory subsystem
    pub fn init(&mut self) {
        // Phase 1: Initialize the physical frame allocator
        frame::init();
        self.frame_allocator_initialized = true;

        // Phase 2: Initialize kernel heap
        heap::init();
        self.heap_initialized = true;

        // Phase 3: Initialize paging
        paging::init();

        // Report memory stats
        self.total_memory = frame::total_frames() as u64 * frame::FRAME_SIZE as u64;
        self.used_memory = frame::used_frames() as u64 * frame::FRAME_SIZE as u64;
    }

    /// Get total system memory in bytes
    pub fn total_memory(&self) -> u64 {
        self.total_memory
    }

    /// Get used memory in bytes
    pub fn used_memory(&self) -> u64 {
        self.used_memory
    }

    /// Get free memory in bytes
    pub fn free_memory(&self) -> u64 {
        self.total_memory - self.used_memory
    }

    /// Check if the heap is initialized
    pub fn heap_ready(&self) -> bool {
        self.heap_initialized
    }
}
