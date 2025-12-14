//! # Ethernet Frame Handling
//!
//! Parses and constructs Ethernet II frames.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

/// EtherType values
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum EtherType {
    Ipv4 = 0x0800,
    Arp = 0x0806,
    Ipv6 = 0x86DD,
    Vlan = 0x8100,
}

/// MAC address (6 bytes)
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MacAddress(pub [u8; 6]);

impl MacAddress {
    pub const BROADCAST: Self = Self([0xFF; 6]);
    pub const ZERO: Self = Self([0; 6]);
}

/// An Ethernet II frame header
#[derive(Debug, Clone)]
pub struct EthernetFrame {
    pub dst: MacAddress,
    pub src: MacAddress,
    pub ethertype: u16,
}

impl EthernetFrame {
    pub const HEADER_SIZE: usize = 14;

    /// Parse an Ethernet frame from raw bytes
    pub fn parse(data: &[u8]) -> Option<(Self, &[u8])> {
        if data.len() < Self::HEADER_SIZE {
            return None;
        }

        let dst = MacAddress([data[0], data[1], data[2], data[3], data[4], data[5]]);
        let src = MacAddress([data[6], data[7], data[8], data[9], data[10], data[11]]);
        let ethertype = u16::from_be_bytes([data[12], data[13]]);

        Some((
            Self {
                dst,
                src,
                ethertype,
            },
            &data[Self::HEADER_SIZE..],
        ))
    }

    /// Serialize the frame header into a buffer
    pub fn serialize(&self, buf: &mut [u8]) -> usize {
        if buf.len() < Self::HEADER_SIZE {
            return 0;
        }
        buf[0..6].copy_from_slice(&self.dst.0);
        buf[6..12].copy_from_slice(&self.src.0);
        buf[12..14].copy_from_slice(&self.ethertype.to_be_bytes());
        Self::HEADER_SIZE
    }

    /// Check if this frame is for us (unicast or broadcast)
    pub fn is_for(&self, our_mac: &MacAddress) -> bool {
        self.dst == *our_mac || self.dst == MacAddress::BROADCAST
    }
}
