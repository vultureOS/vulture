//! # Named Ports (Mach-like)
//!
//! Named port registry for service discovery and IPC endpoint management.
//! Similar to Mach ports / macOS XPC.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;

/// Maximum number of registered ports
const MAX_PORTS: usize = 256;

/// A named IPC port
#[derive(Debug, Clone)]
pub struct Port {
    /// Port name (e.g., "com.vulture.clipboard")
    pub name: String,
    /// Owner process PID
    pub owner_pid: u64,
    /// Port ID
    pub port_id: u64,
    /// Whether the port is active
    pub active: bool,
    /// Access permissions
    pub permissions: PortPermissions,
}

/// Port access permissions
#[derive(Debug, Clone, Copy)]
pub struct PortPermissions {
    /// Allow any process to send
    pub public_send: bool,
    /// Allow any process to receive
    pub public_receive: bool,
    /// Required capability to access
    pub required_capability: u64,
}

impl PortPermissions {
    pub const fn public() -> Self {
        Self {
            public_send: true,
            public_receive: false,
            required_capability: 0,
        }
    }

    pub const fn private() -> Self {
        Self {
            public_send: false,
            public_receive: false,
            required_capability: 0,
        }
    }
}

/// Global port registry
static PORT_REGISTRY: Mutex<PortRegistry> = Mutex::new(PortRegistry::new());

struct PortRegistry {
    ports: [Option<Port>; MAX_PORTS],
    count: usize,
    next_id: u64,
}

impl PortRegistry {
    const fn new() -> Self {
        Self {
            ports: [const { None }; MAX_PORTS],
            count: 0,
            next_id: 1,
        }
    }
}

/// Initialize the port subsystem
pub fn init() {
    let mut reg = PORT_REGISTRY.lock();
    // Register system ports
    register_system_port(&mut reg, "com.vultureos.kernel", 0);
    register_system_port(&mut reg, "com.vultureos.clipboard", 0);
    register_system_port(&mut reg, "com.vultureos.notification", 0);
    register_system_port(&mut reg, "com.vultureos.power", 0);
    register_system_port(&mut reg, "com.vultureos.network", 0);
    register_system_port(&mut reg, "com.vultureos.fs", 0);
}

fn register_system_port(reg: &mut PortRegistry, name: &str, owner: u64) {
    if reg.count >= MAX_PORTS {
        return;
    }

    let port_id = reg.next_id;
    let idx = reg.count;

    let port = Port {
        name: String::from(name),
        owner_pid: owner,
        port_id,
        active: true,
        permissions: PortPermissions::public(),
    };

    reg.ports[idx] = Some(port);
    reg.count += 1;
    reg.next_id += 1;
}

/// Register a new named port
pub fn register(name: &str, owner_pid: u64, permissions: PortPermissions) -> Option<u64> {
    let mut reg = PORT_REGISTRY.lock();
    if reg.count >= MAX_PORTS {
        return None;
    }

    // Check for duplicate names
    for port in reg.ports.iter().flatten() {
        if port.name == name {
            return None; // Already registered
        }
    }

    let port_id = reg.next_id;
    reg.next_id += 1;

    let port = Port {
        name: String::from(name),
        owner_pid,
        port_id,
        active: true,
        permissions,
    };

    let idx = reg.count;
    reg.ports[idx] = Some(port);
    reg.count += 1;

    Some(port_id)
}

/// Look up a port by name
pub fn lookup(name: &str) -> Option<Port> {
    let reg = PORT_REGISTRY.lock();
    for port in reg.ports.iter().flatten() {
        if port.name == name && port.active {
            return Some(port.clone());
        }
    }
    None
}

/// Unregister a port
pub fn unregister(name: &str, owner_pid: u64) -> bool {
    let mut reg = PORT_REGISTRY.lock();
    for port in reg.ports.iter_mut() {
        if let Some(ref mut p) = port {
            if p.name == name && p.owner_pid == owner_pid {
                p.active = false;
                return true;
            }
        }
    }
    false
}

/// List all registered ports
pub fn list_ports() -> Vec<Port> {
    let reg = PORT_REGISTRY.lock();
    let mut result = Vec::new();
    for port in reg.ports.iter().flatten() {
        if port.active {
            result.push(port.clone());
        }
    }
    result
}
