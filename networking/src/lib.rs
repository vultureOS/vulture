//! # Network Stack
//!
//! Custom TCP/IP networking stack for vulture.
//! Provides Ethernet, IPv4/IPv6, TCP, UDP, and DNS support.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

pub mod dns;
pub mod ethernet;
pub mod ip;
pub mod tcp;
pub mod udp;

/// The global network stack
pub struct NetworkStack {
    initialized: bool,
    interfaces: [Option<NetworkInterface>; 4],
    interface_count: usize,
}

/// A network interface
#[derive(Debug, Clone)]
pub struct NetworkInterface {
    pub name: &'static str,
    pub mac: [u8; 6],
    pub ipv4: Option<Ipv4Addr>,
    pub ipv6: Option<Ipv6Addr>,
    pub mtu: u16,
    pub up: bool,
}

/// IPv4 address
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Ipv4Addr {
    pub octets: [u8; 4],
}

impl Ipv4Addr {
    pub const fn new(a: u8, b: u8, c: u8, d: u8) -> Self {
        Self {
            octets: [a, b, c, d],
        }
    }

    pub const LOCALHOST: Self = Self::new(127, 0, 0, 1);
    pub const UNSPECIFIED: Self = Self::new(0, 0, 0, 0);
    pub const BROADCAST: Self = Self::new(255, 255, 255, 255);
}

/// IPv6 address
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Ipv6Addr {
    pub octets: [u8; 16],
}

impl Ipv6Addr {
    pub const LOCALHOST: Self = Self {
        octets: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1],
    };
}

impl NetworkStack {
    pub const fn new() -> Self {
        Self {
            initialized: false,
            interfaces: [None, None, None, None],
            interface_count: 0,
        }
    }

    /// Initialize the network stack
    pub fn init(&mut self) {
        // Register the loopback interface
        self.add_interface(NetworkInterface {
            name: "lo0",
            mac: [0; 6],
            ipv4: Some(Ipv4Addr::LOCALHOST),
            ipv6: Some(Ipv6Addr::LOCALHOST),
            mtu: 65535,
            up: true,
        });

        self.initialized = true;
    }

    /// Add a network interface
    pub fn add_interface(&mut self, iface: NetworkInterface) -> bool {
        if self.interface_count >= 4 {
            return false;
        }
        self.interfaces[self.interface_count] = Some(iface);
        self.interface_count += 1;
        true
    }

    /// Get an interface by name
    pub fn get_interface(&self, name: &str) -> Option<&NetworkInterface> {
        self.interfaces.iter().flatten().find(|i| i.name == name)
    }

    /// List all interfaces
    pub fn list_interfaces(&self) -> impl Iterator<Item = &NetworkInterface> {
        self.interfaces.iter().flatten()
    }

    /// Check if the stack is initialized
    pub fn is_ready(&self) -> bool {
        self.initialized
    }
}
