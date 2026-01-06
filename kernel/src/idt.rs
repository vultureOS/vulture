
/// Interrupt Descriptor Table (IDT) structure
#[repr(C, packed)]
pub struct IdtEntry {
    low: u16,
    selector: u16,
    flags: u16,
    mid: u16,
    high: u32,
    reserved: u32,
}
