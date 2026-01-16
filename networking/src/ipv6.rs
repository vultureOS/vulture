
/// IPv6 header structure
#[repr(C, packed)]
pub struct Ipv6Header {
    version_class_label: u32,
    payload_length: u16,
    next_header: u8,
    hop_limit: u8,
    src: [u8; 16],
    dst: [u8; 16],
}
