//! # Interrupt Descriptor Table (IDT)
//!
//! Sets up CPU exception handlers and hardware interrupt handlers
//! including timer (IRQ0) and keyboard (IRQ1) via PIC 8259.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::gdt;
use pic8259::ChainedPics;
use spin::Mutex;
use x86_64::structures::idt::{InterruptDescriptorTable, InterruptStackFrame, PageFaultErrorCode};

/// PIC offsets — remap IRQs to avoid conflict with CPU exceptions
pub const PIC_1_OFFSET: u8 = 32;
pub const PIC_2_OFFSET: u8 = PIC_1_OFFSET + 8;

/// Hardware interrupt indices
#[derive(Debug, Clone, Copy)]
#[repr(u8)]
pub enum InterruptIndex {
    Timer = PIC_1_OFFSET,
    Keyboard = PIC_1_OFFSET + 1,
}

/// Chained PIC 8259 controller
pub static PICS: Mutex<ChainedPics> =
    Mutex::new(unsafe { ChainedPics::new(PIC_1_OFFSET, PIC_2_OFFSET) });

/// Global tick counter for scheduling
static TICKS: spin::Mutex<u64> = spin::Mutex::new(0);

/// Get the current tick count
pub fn get_ticks() -> u64 {
    *TICKS.lock()
}

/// The IDT
static IDT: spin::Lazy<InterruptDescriptorTable> = spin::Lazy::new(|| {
    let mut idt = InterruptDescriptorTable::new();

    // CPU exception handlers
    idt.breakpoint.set_handler_fn(breakpoint_handler);
    unsafe {
        idt.double_fault
            .set_handler_fn(double_fault_handler)
            .set_stack_index(gdt::DOUBLE_FAULT_IST_INDEX);
    }
    idt.page_fault.set_handler_fn(page_fault_handler);
    idt.general_protection_fault
        .set_handler_fn(general_protection_handler);
    idt.invalid_opcode.set_handler_fn(invalid_opcode_handler);
    idt.segment_not_present
        .set_handler_fn(segment_not_present_handler);
    idt.stack_segment_fault
        .set_handler_fn(stack_segment_handler);

    // Hardware interrupt handlers
    idt[InterruptIndex::Timer as u8].set_handler_fn(timer_interrupt_handler);
    idt[InterruptIndex::Keyboard as u8].set_handler_fn(keyboard_interrupt_handler);

    idt
});

/// Initialize IDT and PICs
pub fn init() {
    IDT.load();
    unsafe {
        PICS.lock().initialize();
    }
    x86_64::instructions::interrupts::enable();
}

// ─── CPU Exception Handlers ────────────────────────────────────────────────

extern "x86-interrupt" fn breakpoint_handler(stack_frame: InterruptStackFrame) {
    crate::println!("[EXCEPTION] BREAKPOINT\n{:#?}", stack_frame);
}

extern "x86-interrupt" fn double_fault_handler(
    stack_frame: InterruptStackFrame,
    _error_code: u64,
) -> ! {
    crate::println!("[FATAL] DOUBLE FAULT\n{:#?}", stack_frame);
    loop {
        x86_64::instructions::hlt();
    }
}

extern "x86-interrupt" fn page_fault_handler(
    stack_frame: InterruptStackFrame,
    error_code: PageFaultErrorCode,
) {
    use x86_64::registers::control::Cr2;
    crate::println!(
        "[EXCEPTION] PAGE FAULT\n  Accessed Address: {:?}\n  Error Code: {:?}\n{:#?}",
        Cr2::read(),
        error_code,
        stack_frame
    );
    loop {
        x86_64::instructions::hlt();
    }
}

extern "x86-interrupt" fn general_protection_handler(
    stack_frame: InterruptStackFrame,
    error_code: u64,
) {
    crate::println!(
        "[EXCEPTION] GENERAL PROTECTION FAULT (error: {})\n{:#?}",
        error_code,
        stack_frame
    );
    loop {
        x86_64::instructions::hlt();
    }
}

extern "x86-interrupt" fn invalid_opcode_handler(stack_frame: InterruptStackFrame) {
    crate::println!("[EXCEPTION] INVALID OPCODE\n{:#?}", stack_frame);
    loop {
        x86_64::instructions::hlt();
    }
}

extern "x86-interrupt" fn segment_not_present_handler(
    stack_frame: InterruptStackFrame,
    error_code: u64,
) {
    crate::println!(
        "[EXCEPTION] SEGMENT NOT PRESENT (error: {})\n{:#?}",
        error_code,
        stack_frame
    );
    loop {
        x86_64::instructions::hlt();
    }
}

extern "x86-interrupt" fn stack_segment_handler(stack_frame: InterruptStackFrame, error_code: u64) {
    crate::println!(
        "[EXCEPTION] STACK SEGMENT FAULT (error: {})\n{:#?}",
        error_code,
        stack_frame
    );
    loop {
        x86_64::instructions::hlt();
    }
}

// ─── Hardware Interrupt Handlers ────────────────────────────────────────────

extern "x86-interrupt" fn timer_interrupt_handler(_stack_frame: InterruptStackFrame) {
    {
        let mut ticks = TICKS.lock();
        *ticks += 1;
    }

    crate::scheduler::tick();

    unsafe {
        PICS.lock()
            .notify_end_of_interrupt(InterruptIndex::Timer as u8);
    }
}

extern "x86-interrupt" fn keyboard_interrupt_handler(_stack_frame: InterruptStackFrame) {
    use x86_64::instructions::port::Port;

    let mut port = Port::new(0x60);
    let scancode: u8 = unsafe { port.read() };

    crate::drivers::keyboard::handle_scancode(scancode);

    unsafe {
        PICS.lock()
            .notify_end_of_interrupt(InterruptIndex::Keyboard as u8);
    }
}
