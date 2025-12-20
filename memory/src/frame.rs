//! # Physical Frame Allocator
//!
//! Bitmap-based physical frame allocator tracking 4KiB frames.
//! Provides allocation, deallocation, and reference counting.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use spin::Mutex;

/// Page frame size (4 KiB)
pub const FRAME_SIZE: usize = 4096;

/// Maximum number of frames we can track (256 MB worth)
const MAX_FRAMES: usize = 65536;

/// Bitmap tracking allocated frames (1 bit per frame)
static FRAME_BITMAP: Mutex<[u64; MAX_FRAMES / 64]> = Mutex::new([0u64; MAX_FRAMES / 64]);

/// Reference count for each frame (for CoW)
static FRAME_REFCOUNTS: Mutex<[u16; MAX_FRAMES]> = Mutex::new([0u16; MAX_FRAMES]);

/// Total available frames
static TOTAL_FRAMES: Mutex<usize> = Mutex::new(0);

/// Next frame to check (for faster allocation)
static NEXT_FREE: Mutex<usize> = Mutex::new(0);

/// Physical frame address
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct PhysFrame {
    pub number: usize,
}

impl PhysFrame {
    /// Create a frame from a frame number
    pub const fn from_number(number: usize) -> Self {
        Self { number }
    }

    /// Create a frame from a physical address
    pub const fn from_addr(addr: u64) -> Self {
        Self {
            number: (addr / FRAME_SIZE as u64) as usize,
        }
    }

    /// Get the physical address of this frame
    pub const fn addr(&self) -> u64 {
        (self.number * FRAME_SIZE) as u64
    }
}

/// Initialize the frame allocator
pub fn init() {
    let mut total = TOTAL_FRAMES.lock();
    // Assume 256 MB of physical memory for now
    // In a real implementation, this comes from the multiboot memory map
    *total = MAX_FRAMES;

    // Mark first 2 MB as used (kernel, VGA, etc.)
    let reserved_frames = (2 * 1024 * 1024) / FRAME_SIZE; // 512 frames
    let mut bitmap = FRAME_BITMAP.lock();
    for i in 0..reserved_frames {
        let idx = i / 64;
        let bit = i % 64;
        bitmap[idx] |= 1 << bit;
    }

    let mut next = NEXT_FREE.lock();
    *next = reserved_frames;
}

/// Allocate a physical frame
pub fn alloc_frame() -> Option<PhysFrame> {
    let mut bitmap = FRAME_BITMAP.lock();
    let mut next = NEXT_FREE.lock();
    let total = *TOTAL_FRAMES.lock();

    for i in *next..total {
        let idx = i / 64;
        let bit = i % 64;

        if bitmap[idx] & (1 << bit) == 0 {
            // Frame is free — mark as allocated
            bitmap[idx] |= 1 << bit;
            *next = i + 1;

            // Set reference count to 1
            drop(bitmap);
            let mut refs = FRAME_REFCOUNTS.lock();
            refs[i] = 1;

            return Some(PhysFrame::from_number(i));
        }
    }

    // Wrap around and try from the beginning
    for i in 0..*next {
        let idx = i / 64;
        let bit = i % 64;

        if bitmap[idx] & (1 << bit) == 0 {
            bitmap[idx] |= 1 << bit;
            *next = i + 1;

            drop(bitmap);
            let mut refs = FRAME_REFCOUNTS.lock();
            refs[i] = 1;

            return Some(PhysFrame::from_number(i));
        }
    }

    None // Out of memory
}

/// Free a physical frame
pub fn free_frame(frame: PhysFrame) {
    let n = frame.number;
    let mut refs = FRAME_REFCOUNTS.lock();

    if refs[n] > 0 {
        refs[n] -= 1;
    }

    if refs[n] == 0 {
        drop(refs);
        let mut bitmap = FRAME_BITMAP.lock();
        let idx = n / 64;
        let bit = n % 64;
        bitmap[idx] &= !(1 << bit);

        // Update next_free hint
        let mut next = NEXT_FREE.lock();
        if n < *next {
            *next = n;
        }
    }
}

/// Increment the reference count for a frame (for CoW)
pub fn inc_refcount(frame: PhysFrame) {
    let mut refs = FRAME_REFCOUNTS.lock();
    let n = frame.number;
    if n < MAX_FRAMES {
        refs[n] = refs[n].saturating_add(1);
    }
}

/// Get the reference count for a frame
pub fn get_refcount(frame: PhysFrame) -> u16 {
    let refs = FRAME_REFCOUNTS.lock();
    refs[frame.number]
}

/// Get total number of frames
pub fn total_frames() -> usize {
    *TOTAL_FRAMES.lock()
}

/// Get number of used frames
pub fn used_frames() -> usize {
    let bitmap = FRAME_BITMAP.lock();
    let total = *TOTAL_FRAMES.lock();
    let mut count = 0;
    for i in 0..total {
        let idx = i / 64;
        let bit = i % 64;
        if bitmap[idx] & (1 << bit) != 0 {
            count += 1;
        }
    }
    count
}

/// Get number of free frames
pub fn free_frames() -> usize {
    total_frames() - used_frames()
}
