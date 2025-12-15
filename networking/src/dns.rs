//! # DNS Resolver
//!
//! Simple DNS client for resolving domain names to IP addresses.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use crate::Ipv4Addr;
use alloc::string::String;
use alloc::vec::Vec;

/// DNS record types
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum DnsRecordType {
    A = 1,     // IPv4 address
    AAAA = 28, // IPv6 address
    CNAME = 5, // Canonical name
    MX = 15,   // Mail exchange
    NS = 2,    // Name server
    TXT = 16,  // Text record
    PTR = 12,  // Pointer
    SOA = 6,   // Start of authority
}

/// A DNS record
#[derive(Debug, Clone)]
pub struct DnsRecord {
    pub name: String,
    pub record_type: DnsRecordType,
    pub ttl: u32,
    pub data: DnsRecordData,
}

/// DNS record data
#[derive(Debug, Clone)]
pub enum DnsRecordData {
    A(Ipv4Addr),
    AAAA([u8; 16]),
    CNAME(String),
    MX { priority: u16, exchange: String },
    TXT(String),
    Unknown(Vec<u8>),
}

/// DNS resolver configuration
pub struct DnsResolver {
    /// DNS server addresses
    nameservers: Vec<Ipv4Addr>,
    /// Cache of resolved records
    cache: Vec<DnsRecord>,
    /// Maximum cache size
    max_cache: usize,
}

impl DnsResolver {
    pub fn new() -> Self {
        let mut resolver = Self {
            nameservers: Vec::new(),
            cache: Vec::new(),
            max_cache: 256,
        };

        // Default DNS servers
        resolver.nameservers.push(Ipv4Addr::new(8, 8, 8, 8)); // Google
        resolver.nameservers.push(Ipv4Addr::new(1, 1, 1, 1)); // Cloudflare

        // Pre-populate cache with localhost
        resolver.cache.push(DnsRecord {
            name: String::from("localhost"),
            record_type: DnsRecordType::A,
            ttl: u32::MAX,
            data: DnsRecordData::A(Ipv4Addr::LOCALHOST),
        });

        resolver
    }

    /// Resolve a domain name to an IPv4 address
    pub fn resolve(&self, name: &str) -> Option<Ipv4Addr> {
        // Check cache first
        for record in &self.cache {
            if record.name == name {
                if let DnsRecordData::A(addr) = record.data {
                    return Some(addr);
                }
            }
        }

        // In production, send DNS query packet via UDP port 53
        None
    }

    /// Add a nameserver
    pub fn add_nameserver(&mut self, addr: Ipv4Addr) {
        self.nameservers.push(addr);
    }

    /// Manually add a cache entry
    pub fn add_cache_entry(&mut self, record: DnsRecord) {
        if self.cache.len() >= self.max_cache {
            self.cache.remove(0); // FIFO eviction
        }
        self.cache.push(record);
    }

    /// Clear the DNS cache
    pub fn flush_cache(&mut self) {
        self.cache.clear();
    }
}
