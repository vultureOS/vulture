use acpi::{AcpiHandler, AcpiTables, PhysicalMapping};
use core::ptr::NonNull;

#[derive(Clone)]
pub struct VultureAcpiHandler {
    phys_offset: u64,
}

impl AcpiHandler for VultureAcpiHandler {
    unsafe fn map_physical_region<T>(
        &self,
        physical_address: usize,
        size: usize,
    ) -> PhysicalMapping<Self, T> {
        let virtual_address = physical_address as u64 + self.phys_offset;
        PhysicalMapping::new(
            physical_address,
            NonNull::new(virtual_address as *mut T).unwrap(),
            size,
            size,
            self.clone(),
        )
    }

    fn unmap_physical_region<T>(_region: &PhysicalMapping<Self, T>) {
        // We use offset mapping, no actual unmap needed
    }
}

pub fn init(phys_mem_offset: u64) {
    crate::serial_println!("[acpi] Initializing ACPI subsystem...");

    let handler = VultureAcpiHandler {
        phys_offset: phys_mem_offset,
    };

    // Search for RSDP in BIOS area
    match unsafe { acpi::AcpiTables::search_for_rsdp_bios(handler) } {
        Ok(tables) => {
            crate::serial_println!("[acpi] Found ACPI tables");
            if let Ok(fadt) = tables.find_table::<acpi::fadt::Fadt>() {
                crate::serial_println!("[acpi] Found FADT mapping");
                if let Ok(pm1a) = fadt.pm1a_control_block() {
                    crate::serial_println!("[acpi] PM1a Control Block: {:#x}", pm1a.address);
                }
            }
        }
        Err(e) => {
            crate::serial_println!("[acpi] Error finding RSDP: {:?}", e);
        }
    }
}

pub fn shutdown() {
    // For ACPI shutdown, we need PM1a_CNT_BLK port from FADT and we need the \_S5 object from AML.
    // QEMU/Bochs standard ACPI shutdown is usually at port 0x604 or 0xB004, but standard PC uses PM ports.
    // We will inject the standard QEMU ACPI poweroff signal as a stopgap while parsing tables.

    crate::println!("Shutting down via ACPI...");
    unsafe {
        // Standard ISA debug exit
        x86_64::instructions::port::Port::<u16>::new(0x604).write(0x2000); // QEMU/Bochs
        x86_64::instructions::port::Port::<u16>::new(0xB004).write(0x2000); // QEMU standard
    }

    crate::println!("It is now safe to turn off your computer.");
    crate::serial_println!("[acpi] System halted.");
    loop {
        x86_64::instructions::hlt();
    }
}
