//! # Copy-on-Write (CoW) Page Management
//!
//! Implements copy-on-write semantics for shared memory pages.
//! When a process forks, pages are shared read-only. On write,
//! the page is duplicated and made writable for the writing process.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::frame::{self, PhysFrame};
use crate::paging::PageFlags;

/// CoW page state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CowState {
    /// Page is exclusively owned (refcount == 1)
    Exclusive,
    /// Page is shared (refcount > 1)
    Shared,
}

/// Check the CoW state of a frame
pub fn cow_state(frame: PhysFrame) -> CowState {
    let refcount = frame::get_refcount(frame);
    if refcount <= 1 {
        CowState::Exclusive
    } else {
        CowState::Shared
    }
}

/// Share a page for CoW (called during fork)
///
/// Increments the reference count and returns read-only flags
pub fn share_page(frame: PhysFrame) -> PageFlags {
    frame::inc_refcount(frame);
    PageFlags::read_only()
}

/// Handle a CoW fault (called from page fault handler)
///
/// If the page is shared, allocate a new frame, copy data, and return it.
/// If the page is exclusively owned, just make it writable.
pub fn handle_cow_fault(old_frame: PhysFrame) -> Option<(PhysFrame, PageFlags)> {
    match cow_state(old_frame) {
        CowState::Exclusive => {
            // Just make it writable again
            Some((old_frame, PageFlags::user()))
        }
        CowState::Shared => {
            // Allocate a new frame
            let new_frame = frame::alloc_frame()?;

            // Perform the actual copy
            copy_frame(old_frame, new_frame);

            // Decrement reference on old frame
            frame::free_frame(old_frame);

            Some((new_frame, PageFlags::user()))
        }
    }
}

/// Copy the contents of one physical frame to another
fn copy_frame(src: PhysFrame, dst: PhysFrame) {
    let src_addr = src.as_u64();
    let dst_addr = dst.as_u64();
    
    // Safety: Kernel is mapped and has access to all physical memory
    unsafe {
        core::ptr::copy_nonoverlapping(
            src_addr as *const u8,
            dst_addr as *mut u8,
            4096, // 4KiB frame size
        );
    }
}

/// Unshare a page (called during process exit)
///
/// Decrements the reference count. If it reaches 0, the frame is freed.
pub fn unshare_page(frame: PhysFrame) {
    frame::free_frame(frame);
}
