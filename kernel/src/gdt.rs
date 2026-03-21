//! # Global Descriptor Table (GDT)
//!
//! Sets up the GDT with kernel/user code and data segments plus TSS
//! for x86_64 long mode operation.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use spin::Mutex;
use x86_64::structures::gdt::{Descriptor, GlobalDescriptorTable, SegmentSelector};
use x86_64::structures::tss::TaskStateSegment;
use x86_64::VirtAddr;

/// Double fault IST index
pub const DOUBLE_FAULT_IST_INDEX: u16 = 0;

/// Stack size for the double fault handler
const STACK_SIZE: usize = 4096 * 5;

/// Double fault handler stack
static mut DOUBLE_FAULT_STACK: [u8; STACK_SIZE] = [0; STACK_SIZE];

/// Task State Segment
static TSS: spin::Lazy<TaskStateSegment> = spin::Lazy::new(|| {
    let mut tss = TaskStateSegment::new();
    tss.interrupt_stack_table[DOUBLE_FAULT_IST_INDEX as usize] = {
        let stack_start = VirtAddr::from_ptr(unsafe { &DOUBLE_FAULT_STACK });
        stack_start + STACK_SIZE as u64
    };
    tss
});

/// GDT with selectors
static GDT: spin::Lazy<(GlobalDescriptorTable, Selectors)> = spin::Lazy::new(|| {
    let mut gdt = GlobalDescriptorTable::new();
    let kernel_code = gdt.append(Descriptor::kernel_code_segment());
    let kernel_data = gdt.append(Descriptor::kernel_data_segment());
    let tss_selector = gdt.append(Descriptor::tss_segment(&TSS));
    let user_data = gdt.append(Descriptor::user_data_segment());
    let user_code = gdt.append(Descriptor::user_code_segment());
    (
        gdt,
        Selectors {
            kernel_code,
            kernel_data,
            tss_selector,
            user_data,
            user_code,
        },
    )
});

/// Segment selectors
pub struct Selectors {
    pub kernel_code: SegmentSelector,
    pub kernel_data: SegmentSelector,
    pub tss_selector: SegmentSelector,
    pub user_data: SegmentSelector,
    pub user_code: SegmentSelector,
}

/// Initialize the GDT
pub fn init() {
    use x86_64::instructions::segmentation::{Segment, CS, DS, ES, SS};
    use x86_64::instructions::tables::load_tss;

    GDT.0.load();
    unsafe {
        CS::set_reg(GDT.1.kernel_code);
        DS::set_reg(GDT.1.kernel_data);
        ES::set_reg(GDT.1.kernel_data);
        SS::set_reg(GDT.1.kernel_data);
        load_tss(GDT.1.tss_selector);
    }
}

/// Retrieve the system global selectors
pub fn get_selectors() -> Selectors {
    Selectors {
        kernel_code: GDT.1.kernel_code,
        kernel_data: GDT.1.kernel_data,
        tss_selector: GDT.1.tss_selector,
        user_data: GDT.1.user_data,
        user_code: GDT.1.user_code,
    }
}
