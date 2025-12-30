//! # vultureKernel
//!
//! The core kernel for vultureOS — a hybrid microkernel written in Rust.
//!
//! ### Legal Information
//! * **Copyright:** (C) 2022-2026 Krisna Pranav
//! * **License:** GNU General Public License v3.0 (GPL-3.0-or-later)
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]
#![feature(abi_x86_interrupt)]

extern crate alloc;

pub mod acpi;
pub mod drivers;
pub mod gdt;
pub mod interrupts;
pub mod process;
pub mod scheduler;
pub mod serial;
pub mod shell;
pub mod syscall;
pub mod vga;

use bootloader::bootinfo::{MemoryMap, MemoryRegionType};
use x86_64::structures::paging::{
    FrameAllocator, Mapper, OffsetPageTable, Page, PageTable, PageTableFlags, PhysFrame, Size4KiB,
};
use x86_64::{PhysAddr, VirtAddr};

// ─── Heap Page Mapping ──────────────────────────────────────────────────────

/// Create an OffsetPageTable from the bootloader's physical memory offset.
///
/// # Safety
/// The caller must guarantee that the complete physical memory is mapped at
/// `physical_memory_offset` in the virtual address space. Also, this function
/// must only be called once to avoid aliasing `&mut` references.
unsafe fn init_offset_page_table(physical_memory_offset: VirtAddr) -> OffsetPageTable<'static> {
    let level_4_table = active_level_4_table(physical_memory_offset);
    OffsetPageTable::new(level_4_table, physical_memory_offset)
}

/// Returns a mutable reference to the active level 4 page table.
///
/// # Safety
/// The caller must guarantee that the complete physical memory is mapped at
/// `physical_memory_offset`.
unsafe fn active_level_4_table(physical_memory_offset: VirtAddr) -> &'static mut PageTable {
    use x86_64::registers::control::Cr3;

    let (level_4_table_frame, _) = Cr3::read();
    let phys = level_4_table_frame.start_address();
    let virt = physical_memory_offset + phys.as_u64();
    let page_table_ptr: *mut PageTable = virt.as_mut_ptr();

    &mut *page_table_ptr
}

/// A frame allocator that uses the bootloader's memory map to find free frames.
struct BootInfoFrameAllocator {
    memory_map: &'static MemoryMap,
    next: usize,
}

impl BootInfoFrameAllocator {
    /// Create a new frame allocator from the bootloader's memory map.
    ///
    /// # Safety
    /// The caller must guarantee that the passed memory map is valid and that
    /// all frames marked as `USABLE` are really unused.
    unsafe fn new(memory_map: &'static MemoryMap) -> Self {
        Self {
            memory_map,
            next: 0,
        }
    }

    /// Returns an iterator over usable physical frames from the memory map.
    fn usable_frames(&self) -> impl Iterator<Item = PhysFrame> + '_ {
        self.memory_map
            .iter()
            .filter(|r| r.region_type == MemoryRegionType::Usable)
            .map(|r| r.range.start_addr()..r.range.end_addr())
            .flat_map(|r| r.step_by(4096))
            .map(|addr| PhysFrame::containing_address(PhysAddr::new(addr)))
    }
}

unsafe impl FrameAllocator<Size4KiB> for BootInfoFrameAllocator {
    fn allocate_frame(&mut self) -> Option<PhysFrame<Size4KiB>> {
        let frame = self.usable_frames().nth(self.next);
        self.next += 1;
        frame
    }
}

/// Map the kernel heap pages in the active page table.
///
/// This must be called **before** the heap allocator is initialized,
/// otherwise any heap allocation will page-fault.
fn init_heap_pages(physical_memory_offset: VirtAddr, memory_map: &'static MemoryMap) {
    let mut mapper = unsafe { init_offset_page_table(physical_memory_offset) };
    let mut frame_allocator = unsafe { BootInfoFrameAllocator::new(memory_map) };

    let heap_start = VirtAddr::new(vulture_memory::heap::HEAP_START as u64);
    let heap_end = heap_start + vulture_memory::heap::HEAP_SIZE as u64 - 1u64;
    let heap_start_page: Page<Size4KiB> = Page::containing_address(heap_start);
    let heap_end_page: Page<Size4KiB> = Page::containing_address(heap_end);

    for page in Page::range_inclusive(heap_start_page, heap_end_page) {
        let frame = frame_allocator
            .allocate_frame()
            .expect("heap mapping: out of physical frames");
        let flags = PageTableFlags::PRESENT | PageTableFlags::WRITABLE;
        unsafe {
            mapper
                .map_to(page, frame, flags, &mut frame_allocator)
                .expect("heap mapping: map_to failed")
                .flush();
        }
    }
}

// ─── Kernel ─────────────────────────────────────────────────────────────────

/// The vultureOS Kernel
pub struct Kernel;

impl Kernel {
    pub fn new() -> Self {
        Self
    }

    /// Main kernel boot sequence — receives full boot info from the bootloader.
    pub fn run_with_boot_info(&mut self, boot_info: &'static bootloader::BootInfo) -> ! {
        let phys_mem_offset = boot_info.physical_memory_offset;

        // ─── Phase 0: Serial (I/O ports, works immediately) ──────────
        serial::init();
        serial_println!("[boot] Serial initialized");

        // ─── Phase 1: VGA Console ────────────────────────────────────
        serial_println!(
            "[boot] Initializing VGA (phys_offset: {:#x})...",
            phys_mem_offset
        );
        vga::init(phys_mem_offset);

        println!("========================================");
        println!("  vultureOS v0.1.0 — vultureKernel");
        println!("========================================");
        println!();

        // ─── Phase 2: GDT (needed for interrupts) ────────────────────
        println!("[boot] Initializing GDT...");
        serial_println!("[boot] Initializing GDT...");
        gdt::init();
        println!("[boot] GDT initialized");

        // ─── Phase 3: IDT + interrupts ───────────────────────────────
        println!("[boot] Initializing IDT + PIC...");
        serial_println!("[boot] Initializing IDT + PIC...");
        interrupts::init();
        println!("[boot] Interrupts enabled");

        // ─── Phase 4: Map heap pages, then init memory manager ───────
        println!("[boot] Mapping heap pages...");
        serial_println!("[boot] Mapping heap pages...");
        init_heap_pages(VirtAddr::new(phys_mem_offset), &boot_info.memory_map);
        println!("[boot] Heap pages mapped");

        println!("[boot] Initializing memory manager...");
        serial_println!("[boot] Initializing memory manager...");
        let mut memory = vulture_memory::MemoryManager::new();
        memory.init();
        println!("[boot] Memory manager ready");

        // ─── Phase 5: Subsystems (need heap) ─────────────────────────
        serial_println!("[boot] Heap ready, creating subsystems...");

        println!("[boot] Mounting vultureFS...");
        let mut fs = vulture_fs::VultureFS::new();
        fs.init_root_fs();
        println!("[boot] Filesystem mounted");

        println!("[boot] Initializing security...");
        let security = vulture_security::SecurityContext::new();
        println!("[boot] Security ready");

        println!("[boot] Initializing IPC...");
        let mut ipc = vulture_ipc::IpcSubsystem::new();
        ipc.init();
        println!("[boot] IPC ready");

        println!("[boot] Initializing network stack...");
        let _network = vulture_net::NetworkStack::new();
        println!("[boot] Network stack ready");

        println!("[boot] Starting system services...");
        let _services = vulture_services::ServiceManager::new();
        println!("[boot] System services ready");

        // ─── Phase 6: Device drivers ─────────────────────────────────
        println!("[boot] Initializing device drivers...");
        drivers::init_all();
        println!("[boot] Drivers ready");

        // ─── Phase 6.5: ACPI ───────────────────────────────────────
        println!("[boot] Initializing ACPI...");
        acpi::init(phys_mem_offset);
        println!("[boot] ACPI ready");

        // ─── Phase 7: Scheduler ──────────────────────────────────────
        println!("[boot] Initializing scheduler...");
        scheduler::init();
        scheduler::activate();
        println!("[boot] Scheduler active");

        println!();
        println!("========================================");
        println!("  vultureOS kernel fully initialized!");
        println!("  All subsystems operational.");
        println!("========================================");
        println!();

        serial_println!("[boot] Entering shell...");

        // Enter the interactive shell
        shell::run(&mut fs, &security);

        println!("Shell exited. System halted.");
        loop {
            x86_64::instructions::hlt();
        }
    }
}
