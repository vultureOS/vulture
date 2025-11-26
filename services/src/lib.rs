//! # System Services (Daemons)
//!
//! Framework for system services/daemons in vultureOS.
//! Provides launch agent, clipboard, notification, power,
//! indexer, and update services.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

pub mod clipboard;
pub mod indexer;
pub mod launcher;
pub mod notification;
pub mod power;
pub mod update;

use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;

/// Service state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ServiceState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Failed,
}

/// A registered system service
#[derive(Debug, Clone)]
pub struct ServiceInfo {
    pub name: String,
    pub description: String,
    pub state: ServiceState,
    pub auto_start: bool,
    pub pid: Option<u64>,
}

/// The global service manager
pub struct ServiceManager {
    services: Vec<ServiceInfo>,
    initialized: bool,
}

impl ServiceManager {
    pub const fn new() -> Self {
        Self {
            services: Vec::new(),
            initialized: false,
        }
    }

    /// Initialize all system services
    pub fn init(&mut self) {
        // Register core system services
        self.register("com.vultureos.launcher", "App Launcher", true);
        self.register("com.vultureos.clipboard", "Clipboard Service", true);
        self.register("com.vultureos.notification", "Notification Center", true);
        self.register("com.vultureos.power", "Power Management", true);
        self.register("com.vultureos.indexer", "File Indexer", true);
        self.register("com.vultureos.update", "Update Service", false);
        self.register("com.vultureos.network", "Network Service", true);
        self.register("com.vultureos.time", "Time Service", true);

        // Start all auto-start services
        for service in &mut self.services {
            if service.auto_start {
                service.state = ServiceState::Running;
            }
        }

        self.initialized = true;
    }

    /// Register a new service
    pub fn register(&mut self, name: &str, description: &str, auto_start: bool) {
        self.services.push(ServiceInfo {
            name: String::from(name),
            description: String::from(description),
            state: ServiceState::Stopped,
            auto_start,
            pid: None,
        });
    }

    /// Start a service by name
    pub fn start(&mut self, name: &str) -> bool {
        if let Some(svc) = self.services.iter_mut().find(|s| s.name == name) {
            svc.state = ServiceState::Running;
            true
        } else {
            false
        }
    }

    /// Stop a service
    pub fn stop(&mut self, name: &str) -> bool {
        if let Some(svc) = self.services.iter_mut().find(|s| s.name == name) {
            svc.state = ServiceState::Stopped;
            true
        } else {
            false
        }
    }

    /// Get service info
    pub fn get(&self, name: &str) -> Option<&ServiceInfo> {
        self.services.iter().find(|s| s.name == name)
    }

    /// List all services
    pub fn list(&self) -> &[ServiceInfo] {
        &self.services
    }

    /// Check if initialized
    pub fn is_ready(&self) -> bool {
        self.initialized
    }
}
