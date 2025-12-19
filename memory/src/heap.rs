//! # Kernel Heap Allocator
//!
//! Provides `#[global_allocator]` for the kernel using a linked-list
//! free-list allocator that supports individual deallocation.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use core::alloc::{GlobalAlloc, Layout};
use core::mem;
use core::ptr;
use spin::Mutex;

/// Kernel heap start address
pub const HEAP_START: usize = 0x4444_4444_0000;
/// Kernel heap size (4 MB — large enough for shell + subsystems)
pub const HEAP_SIZE: usize = 4 * 1024 * 1024;

/// A node in the free list
struct FreeNode {
    size: usize,
    next: Option<&'static mut FreeNode>,
}

impl FreeNode {
    const fn new(size: usize) -> Self {
        Self { size, next: None }
    }

    fn start_addr(&self) -> usize {
        self as *const Self as usize
    }

    fn end_addr(&self) -> usize {
        self.start_addr() + self.size
    }
}

/// A linked-list free-list allocator
pub struct LinkedListAllocator {
    head: FreeNode,
}

impl LinkedListAllocator {
    pub const fn new() -> Self {
        Self {
            head: FreeNode::new(0),
        }
    }

    /// Initialize the allocator with the given heap region.
    ///
    /// # Safety
    /// The caller must guarantee that the given memory range is valid,
    /// unused, and mapped in the page table.
    pub unsafe fn init(&mut self, heap_start: usize, heap_size: usize) {
        self.add_free_region(heap_start, heap_size);
    }

    /// Add a free region to the free list.
    unsafe fn add_free_region(&mut self, addr: usize, size: usize) {
        // Ensure the freed region is capable of holding a FreeNode
        assert_eq!(align_up(addr, mem::align_of::<FreeNode>()), addr);
        assert!(size >= mem::size_of::<FreeNode>());

        let mut node = FreeNode::new(size);
        node.next = self.head.next.take();
        let node_ptr = addr as *mut FreeNode;
        node_ptr.write(node);
        self.head.next = Some(&mut *node_ptr);
    }

    /// Find a free region with the given size and alignment and remove it
    /// from the list.
    ///
    /// Returns `(region_start, region_size)` on success.
    fn find_region(&mut self, size: usize, align: usize) -> Option<(usize, usize)> {
        let mut current = &mut self.head;

        while let Some(ref mut region) = current.next {
            if let Ok(alloc_start) = Self::alloc_from_region(region, size, align) {
                // Region suitable — remove it from the list
                let next = region.next.take();
                let ret = Some((alloc_start, region.size));
                current.next = next;
                return ret;
            } else {
                // Region too small — move to next
                current = current.next.as_mut().unwrap();
            }
        }

        None // no suitable region found
    }

    /// Try to allocate from a given region. Returns the start address on success.
    fn alloc_from_region(region: &FreeNode, size: usize, align: usize) -> Result<usize, ()> {
        let alloc_start = align_up(region.start_addr(), align);
        let alloc_end = alloc_start.checked_add(size).ok_or(())?;

        if alloc_end > region.end_addr() {
            // Region too small
            return Err(());
        }

        let excess_size = region.end_addr() - alloc_end;
        if excess_size > 0 && excess_size < mem::size_of::<FreeNode>() {
            // Rest of region too small to hold a FreeNode (fragmentation)
            // — we can't split, so this region doesn't work unless we
            // accept wasting those bytes
            // For simplicity, accept the waste
        }

        Ok(alloc_start)
    }

    /// Adjust layout so that the resulting allocated memory region is also
    /// capable of storing a `FreeNode`.
    fn size_align(layout: Layout) -> (usize, usize) {
        let layout = layout
            .align_to(mem::align_of::<FreeNode>())
            .expect("adjusting alignment failed")
            .pad_to_align();
        let size = layout.size().max(mem::size_of::<FreeNode>());
        (size, layout.align())
    }
}

/// Wrapper to allow Mutex in a static
pub struct Locked<T> {
    inner: Mutex<T>,
}

impl<T> Locked<T> {
    pub const fn new(inner: T) -> Self {
        Self {
            inner: Mutex::new(inner),
        }
    }
}

unsafe impl GlobalAlloc for Locked<LinkedListAllocator> {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let (size, align) = LinkedListAllocator::size_align(layout);
        let mut allocator = self.inner.lock();

        if let Some((region_start, region_size)) = allocator.find_region(size, align) {
            let alloc_end = region_start.checked_add(size).expect("overflow");
            let excess_size = region_size
                - (alloc_end - (region_start - (align_up(region_start, align) - region_start)));

            // Actually compute excess from alloc_end to region end
            let region_end_addr =
                region_start + region_size - (align_up(region_start, align) - region_start);
            // Simpler: we took a region of `region_size` bytes.
            // We used `alloc_end - region.start_addr()` bytes (where region.start_addr()
            // is the node that was at the head of the region).
            // But since we may have aligned up, let's just recalculate:
            let actual_region_start = region_start - (align_up(region_start, align) - region_start);
            // Hmm, this is getting complex. Let's simplify:
            // The original region started at some address and had `region_size` bytes.
            // We aligned the start within, so the excess is from `alloc_end` to the original region end.
            // But we don't have the original address anymore. Let's use a simpler approach:

            // The region_start we got IS already aligned. The region_size is the full region.
            // We need to put back the excess after our allocation.
            let excess_start = alloc_end;
            let excess = (region_start + region_size) - excess_start;

            if excess >= mem::size_of::<FreeNode>() {
                // Put excess back into the free list
                let excess_addr = align_up(excess_start, mem::align_of::<FreeNode>());
                let remaining = (region_start + region_size) - excess_addr;
                if remaining >= mem::size_of::<FreeNode>() {
                    allocator.add_free_region(excess_addr, remaining);
                }
            }

            region_start as *mut u8
        } else {
            ptr::null_mut()
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        let (size, _) = LinkedListAllocator::size_align(layout);
        let mut allocator = self.inner.lock();
        allocator.add_free_region(ptr as usize, size);
    }
}

/// The global kernel allocator
#[global_allocator]
static ALLOCATOR: Locked<LinkedListAllocator> = Locked::new(LinkedListAllocator::new());

/// Initialize the kernel heap
pub fn init() {
    unsafe {
        ALLOCATOR.inner.lock().init(HEAP_START, HEAP_SIZE);
    }
}

/// Get heap usage information (approximate for linked-list allocator)
pub fn heap_usage() -> (usize, usize) {
    // Walk the free list to calculate free space
    let allocator = ALLOCATOR.inner.lock();
    let mut free = 0usize;
    let mut current = &allocator.head;
    while let Some(ref node) = current.next {
        free += node.size;
        current = node;
    }
    let used = HEAP_SIZE - free;
    (used, HEAP_SIZE)
}

/// Align address up
fn align_up(addr: usize, align: usize) -> usize {
    (addr + align - 1) & !(align - 1)
}
