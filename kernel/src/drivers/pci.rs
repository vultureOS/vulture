use alloc::vec::Vec;
use lazy_static::lazy_static;
use spin::Mutex;
use x86_64::instructions::port::Port;

const CONFIG_ADDRESS: u16 = 0xCF8;
const CONFIG_DATA: u16 = 0xCFC;

#[derive(Debug, Clone, Copy)]
pub struct PciDevice {
    pub bus: u8,
    pub device: u8,
    pub function: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub class: u8,
    pub subclass: u8,
    pub prog_if: u8,
}

lazy_static! {
    pub static ref PCI_DEVICES: Mutex<Vec<PciDevice>> = Mutex::new(Vec::new());
}

fn pci_config_read_u32(bus: u8, slot: u8, func: u8, offset: u8) -> u32 {
    let address = ((bus as u32) << 16)
        | ((slot as u32) << 11)
        | ((func as u32) << 8)
        | (offset as u32 & 0xFC)
        | 0x8000_0000;

    unsafe {
        let mut addr_port = Port::<u32>::new(CONFIG_ADDRESS);
        let mut data_port = Port::<u32>::new(CONFIG_DATA);
        addr_port.write(address);
        data_port.read()
    }
}

pub fn init() {
    crate::serial_println!("[pci] Enumerating PCI bus...");

    let mut discovered = 0;

    // Brute force scan Bus 0
    for bus in 0..=255 {
        for device in 0..32 {
            let vendor = pci_config_read_u32(bus, device, 0, 0) & 0xFFFF;
            if vendor != 0xFFFF {
                // Device exists! Look at function 0
                scan_device(bus, device);
                discovered += 1;
            }
        }
    }

    crate::serial_println!("[pci] Enumeration complete. Found {} devices.", discovered);
}

fn scan_device(bus: u8, device: u8) {
    let raw = pci_config_read_u32(bus, device, 0, 0);
    let vendor_id = (raw & 0xFFFF) as u16;
    let device_id = (raw >> 16) as u16;

    let class_raw = pci_config_read_u32(bus, device, 0, 0x08);
    let class = ((class_raw >> 24) & 0xFF) as u8;
    let subclass = ((class_raw >> 16) & 0xFF) as u8;
    let prog_if = ((class_raw >> 8) & 0xFF) as u8;

    let dev = PciDevice {
        bus,
        device,
        function: 0,
        vendor_id,
        device_id,
        class,
        subclass,
        prog_if,
    };

    crate::serial_println!(
        "[pci] Found: Bus {}, Device {} | Vendor: {:#06x}, Device: {:#06x} | Class: {:#04x}:{:#04x}",
        bus, device, vendor_id, device_id, class, subclass
    );

    PCI_DEVICES.lock().push(dev);
}

/// PCI Device identifier
pub struct PciDevice {
    pub bus: u8,
    pub slot: u8,
    pub vendor_id: u16,
    pub device_id: u16,
}
