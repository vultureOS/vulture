//! # IP Protocol
//!
//! IPv4 and IPv6 packet handling.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::{Ipv4Addr, Ipv6Addr};

/// IP protocol numbers
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum IpProtocol {
    Icmp = 1,
    Tcp = 6,
    Udp = 17,
    Icmpv6 = 58,
}

/// IPv4 packet header
#[derive(Debug, Clone)]
pub struct Ipv4Header {
    pub version: u8,
    pub ihl: u8,
    pub dscp: u8,
    pub total_length: u16,
    pub identification: u16,
    pub flags: u8,
    pub fragment_offset: u16,
    pub ttl: u8,
    pub protocol: u8,
    pub checksum: u16,
    pub src: Ipv4Addr,
    pub dst: Ipv4Addr,
}

impl Ipv4Header {
    pub const MIN_SIZE: usize = 20;

    /// Parse an IPv4 header from raw bytes
    pub fn parse(data: &[u8]) -> Option<(Self, &[u8])> {
        if data.len() < Self::MIN_SIZE {
            return None;
        }

        let version = data[0] >> 4;
        if version != 4 {
            return None;
        }

        let ihl = data[0] & 0x0F;
        let header_len = (ihl as usize) * 4;
        if data.len() < header_len {
            return None;
        }

        let header = Self {
            version,
            ihl,
            dscp: data[1],
            total_length: u16::from_be_bytes([data[2], data[3]]),
            identification: u16::from_be_bytes([data[4], data[5]]),
            flags: data[6] >> 5,
            fragment_offset: u16::from_be_bytes([data[6] & 0x1F, data[7]]),
            ttl: data[8],
            protocol: data[9],
            checksum: u16::from_be_bytes([data[10], data[11]]),
            src: Ipv4Addr::new(data[12], data[13], data[14], data[15]),
            dst: Ipv4Addr::new(data[16], data[17], data[18], data[19]),
        };

        Some((header, &data[header_len..]))
    }

    /// Compute the header checksum
    pub fn compute_checksum(&self) -> u16 {
        // Simplified checksum computation
        let mut sum: u32 = 0;
        // In production, iterate over all 16-bit words in the header
        sum += (self.version as u32) << 12 | (self.ihl as u32) << 8 | self.dscp as u32;
        sum += self.total_length as u32;
        sum += self.identification as u32;
        sum += self.ttl as u32 | (self.protocol as u32) << 8;

        // Fold 32-bit sum to 16 bits
        while sum >> 16 != 0 {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        !sum as u16
    }
}

/// IPv6 packet header
#[derive(Debug, Clone)]
pub struct Ipv6Header {
    pub version: u8,
    pub traffic_class: u8,
    pub flow_label: u32,
    pub payload_length: u16,
    pub next_header: u8,
    pub hop_limit: u8,
    pub src: Ipv6Addr,
    pub dst: Ipv6Addr,
}

impl Ipv6Header {
    pub const SIZE: usize = 40;

    /// Parse an IPv6 header from raw bytes
    pub fn parse(data: &[u8]) -> Option<(Self, &[u8])> {
        if data.len() < Self::SIZE {
            return None;
        }

        let version = data[0] >> 4;
        if version != 6 {
            return None;
        }

        let mut src = [0u8; 16];
        let mut dst = [0u8; 16];
        src.copy_from_slice(&data[8..24]);
        dst.copy_from_slice(&data[24..40]);

        let header = Self {
            version,
            traffic_class: ((data[0] & 0x0F) << 4) | (data[1] >> 4),
            flow_label: ((data[1] as u32 & 0x0F) << 16) | ((data[2] as u32) << 8) | data[3] as u32,
            payload_length: u16::from_be_bytes([data[4], data[5]]),
            next_header: data[6],
            hop_limit: data[7],
            src: Ipv6Addr { octets: src },
            dst: Ipv6Addr { octets: dst },
        };

        Some((header, &data[Self::SIZE..]))
    }
}
