//! # UDP Implementation
//!
//! User Datagram Protocol handling.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::Ipv4Addr;

/// UDP header
#[derive(Debug, Clone)]
pub struct UdpHeader {
    pub src_port: u16,
    pub dst_port: u16,
    pub length: u16,
    pub checksum: u16,
}

impl UdpHeader {
    pub const SIZE: usize = 8;

    /// Parse a UDP header from raw bytes
    pub fn parse(data: &[u8]) -> Option<(Self, &[u8])> {
        if data.len() < Self::SIZE {
            return None;
        }

        let header = Self {
            src_port: u16::from_be_bytes([data[0], data[1]]),
            dst_port: u16::from_be_bytes([data[2], data[3]]),
            length: u16::from_be_bytes([data[4], data[5]]),
            checksum: u16::from_be_bytes([data[6], data[7]]),
        };

        let payload_len = (header.length as usize).saturating_sub(Self::SIZE);
        if data.len() < Self::SIZE + payload_len {
            return None;
        }

        Some((header, &data[Self::SIZE..Self::SIZE + payload_len]))
    }

    /// Serialize the header into a buffer
    pub fn serialize(&self, buf: &mut [u8]) -> usize {
        if buf.len() < Self::SIZE {
            return 0;
        }
        buf[0..2].copy_from_slice(&self.src_port.to_be_bytes());
        buf[2..4].copy_from_slice(&self.dst_port.to_be_bytes());
        buf[4..6].copy_from_slice(&self.length.to_be_bytes());
        buf[6..8].copy_from_slice(&self.checksum.to_be_bytes());
        Self::SIZE
    }
}

/// A UDP socket
pub struct UdpSocket {
    pub local_addr: Ipv4Addr,
    pub local_port: u16,
    pub bound: bool,
}

impl UdpSocket {
    pub fn new() -> Self {
        Self {
            local_addr: Ipv4Addr::UNSPECIFIED,
            local_port: 0,
            bound: false,
        }
    }

    /// Bind to an address and port
    pub fn bind(&mut self, addr: Ipv4Addr, port: u16) {
        self.local_addr = addr;
        self.local_port = port;
        self.bound = true;
    }

    /// Send a UDP datagram (returns packet bytes in production)
    pub fn send_to(&self, _data: &[u8], _dst_addr: Ipv4Addr, _dst_port: u16) -> usize {
        if !self.bound {
            return 0;
        }
        // In production, construct and transmit the UDP packet
        _data.len()
    }
}
