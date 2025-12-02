//! # Mandatory Access Control (MAC)
//!
//! Policy-based access control engine that restricts operations
//! based on security labels and rules.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;

/// Security labels
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum SecurityLevel {
    /// Untrusted (sandboxed apps)
    Untrusted = 0,
    /// User-level (normal apps)
    User = 1,
    /// System-level (daemons, services)
    System = 2,
    /// Kernel-level (kernel modules)
    Kernel = 3,
}

/// A MAC policy rule
#[derive(Debug, Clone)]
pub struct PolicyRule {
    /// Subject pattern (app name or wildcard)
    pub subject: String,
    /// Object pattern (resource or wildcard)
    pub object: String,
    /// Action (allow or deny)
    pub allow: bool,
    /// Minimum security level required
    pub min_level: SecurityLevel,
}

/// MAC policy engine
pub struct MacPolicy {
    rules: Vec<PolicyRule>,
    enforcing: bool,
}

impl MacPolicy {
    pub const fn new() -> Self {
        Self {
            rules: Vec::new(),
            enforcing: false,
        }
    }

    /// Initialize with default policies
    pub fn init(&mut self) {
        // Kernel has access to everything
        self.add_rule("kernel", "*", true, SecurityLevel::Kernel);
        // Shell has access to most things
        self.add_rule("shell", "*", true, SecurityLevel::System);
        // User apps can access user resources
        self.add_rule("*", "/Users/*", true, SecurityLevel::User);
        // Deny untrusted access to system
        self.add_rule("*", "/System/*", false, SecurityLevel::Untrusted);

        self.enforcing = true;
    }

    /// Add a policy rule
    pub fn add_rule(&mut self, subject: &str, object: &str, allow: bool, min_level: SecurityLevel) {
        self.rules.push(PolicyRule {
            subject: String::from(subject),
            object: String::from(object),
            allow,
            min_level,
        });
    }

    /// Check if a subject can access an object
    pub fn check(&self, subject: &str, object: &str) -> bool {
        if !self.enforcing {
            return true;
        }

        // Find the most specific matching rule
        for rule in self.rules.iter().rev() {
            let subject_matches = rule.subject == "*" || rule.subject == subject;
            let object_matches = rule.object == "*"
                || rule.object == object
                || (rule.object.ends_with('*')
                    && object.starts_with(&rule.object[..rule.object.len() - 1]));

            if subject_matches && object_matches {
                return rule.allow;
            }
        }

        // Default: allow (permissive mode in Phase 1)
        true
    }
}
