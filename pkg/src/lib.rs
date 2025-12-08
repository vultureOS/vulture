//! # vulturePkg — Package Manager
//!
//! System package manager supporting binary packages, source builds,
//! dependency resolution, sandboxed installs, rollbacks, and updates.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

/// Package state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PackageState {
    Available,
    Downloading,
    Installing,
    Installed,
    Updating,
    Removing,
    Broken,
}

/// Package metadata
#[derive(Debug, Clone)]
pub struct Package {
    pub name: String,
    pub version: String,
    pub description: String,
    pub author: String,
    pub license: String,
    pub size_bytes: u64,
    pub dependencies: Vec<String>,
    pub state: PackageState,
    pub install_path: String,
}

/// Dependency resolution result
#[derive(Debug)]
pub enum ResolveResult {
    /// All dependencies satisfied
    Resolved(Vec<String>),
    /// Missing dependencies
    Missing(Vec<String>),
    /// Circular dependency detected
    Circular(String),
}

/// The package manager
pub struct PackageManager {
    /// Registry of all known packages
    registry: BTreeMap<String, Package>,
    /// Installed packages
    installed: BTreeMap<String, Package>,
    /// Repository URLs
    repositories: Vec<String>,
}

impl PackageManager {
    pub fn new() -> Self {
        Self {
            registry: BTreeMap::new(),
            installed: BTreeMap::new(),
            repositories: Vec::new(),
        }
    }

    /// Initialize with default repository
    pub fn init(&mut self) {
        self.repositories
            .push(String::from("https://packages.vultureos.dev/stable"));

        // Pre-register system packages as installed
        self.register_installed("vulture-kernel", "0.1.0", "vultureOS Kernel");
        self.register_installed("vulture-shell", "0.1.0", "System Shell");
        self.register_installed("vulture-fs", "0.1.0", "vultureFS Filesystem");
    }

    fn register_installed(&mut self, name: &str, version: &str, desc: &str) {
        let pkg = Package {
            name: String::from(name),
            version: String::from(version),
            description: String::from(desc),
            author: String::from("vultureOS Team"),
            license: String::from("GPL-3.0"),
            size_bytes: 0,
            dependencies: Vec::new(),
            state: PackageState::Installed,
            install_path: alloc::format!("/System/{}", name),
        };
        self.installed.insert(String::from(name), pkg);
    }

    /// Install a package
    pub fn install(&mut self, name: &str) -> Result<(), String> {
        if self.installed.contains_key(name) {
            return Err(alloc::format!("Package '{}' is already installed", name));
        }

        if let Some(pkg) = self.registry.get(name) {
            let mut pkg = pkg.clone();
            pkg.state = PackageState::Installed;
            self.installed.insert(String::from(name), pkg);
            Ok(())
        } else {
            Err(alloc::format!("Package '{}' not found in registry", name))
        }
    }

    /// Remove a package
    pub fn remove(&mut self, name: &str) -> Result<(), String> {
        if self.installed.remove(name).is_some() {
            Ok(())
        } else {
            Err(alloc::format!("Package '{}' is not installed", name))
        }
    }

    /// List installed packages
    pub fn list_installed(&self) -> Vec<&Package> {
        self.installed.values().collect()
    }

    /// Search available packages
    pub fn search(&self, query: &str) -> Vec<&Package> {
        self.registry
            .values()
            .filter(|p| p.name.contains(query) || p.description.contains(query))
            .collect()
    }

    /// Resolve dependencies for a package
    pub fn resolve_deps(&self, name: &str) -> ResolveResult {
        if let Some(pkg) = self.registry.get(name) {
            let mut missing = Vec::new();
            for dep in &pkg.dependencies {
                if !self.installed.contains_key(dep.as_str()) {
                    missing.push(dep.clone());
                }
            }
            if missing.is_empty() {
                ResolveResult::Resolved(pkg.dependencies.clone())
            } else {
                ResolveResult::Missing(missing)
            }
        } else {
            ResolveResult::Missing(vec![String::from(name)])
        }
    }

    /// Update all installed packages
    pub fn update_all(&mut self) -> usize {
        // In production, check registry for newer versions
        0
    }

    /// Rollback a package to previous version
    pub fn rollback(&mut self, _name: &str) -> bool {
        // In production, restore from snapshot
        false
    }
}
