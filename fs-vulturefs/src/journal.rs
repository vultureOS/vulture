//! # Filesystem Journal
//!
//! Transaction journal for crash consistency.
//! Records filesystem operations for redo/undo on recovery.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;

/// Journal entry types
#[derive(Debug, Clone)]
pub enum JournalOp {
    /// Create a new inode
    Create { path: String },
    /// Write data to a file
    Write { path: String, offset: u64, len: u64 },
    /// Delete a file
    Delete { path: String },
    /// Rename a file
    Rename { old_path: String, new_path: String },
    /// Change permissions
    Chmod { path: String, mode: u32 },
    /// Change ownership
    Chown { path: String, uid: u32, gid: u32 },
}

/// Transaction state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransactionState {
    /// Transaction is active
    Active,
    /// Transaction has been committed
    Committed,
    /// Transaction was rolled back
    RolledBack,
}

/// A single journal transaction
#[derive(Debug, Clone)]
pub struct Transaction {
    pub id: u64,
    pub description: String,
    pub ops: Vec<JournalOp>,
    pub state: TransactionState,
}

/// The filesystem journal
pub struct Journal {
    /// All transactions
    transactions: Vec<Transaction>,
    /// Current active transaction
    current: Option<Transaction>,
    /// Next transaction ID
    next_id: u64,
}

impl Journal {
    pub const fn new() -> Self {
        Self {
            transactions: Vec::new(),
            current: None,
            next_id: 1,
        }
    }

    /// Begin a new transaction
    pub fn begin_transaction(&mut self, description: &str) {
        let txn = Transaction {
            id: self.next_id,
            description: String::from(description),
            ops: Vec::new(),
            state: TransactionState::Active,
        };
        self.next_id += 1;
        self.current = Some(txn);
    }

    /// Record an operation in the current transaction
    pub fn record(&mut self, op: JournalOp) {
        if let Some(ref mut txn) = self.current {
            txn.ops.push(op);
        }
    }

    /// Commit the current transaction
    pub fn commit(&mut self) {
        if let Some(mut txn) = self.current.take() {
            txn.state = TransactionState::Committed;
            self.transactions.push(txn);
        }
    }

    /// Rollback the current transaction
    pub fn rollback(&mut self) {
        if let Some(mut txn) = self.current.take() {
            txn.state = TransactionState::RolledBack;
            self.transactions.push(txn);
        }
    }

    /// Get the total number of committed transactions
    pub fn transaction_count(&self) -> u64 {
        self.transactions
            .iter()
            .filter(|t| t.state == TransactionState::Committed)
            .count() as u64
    }

    /// Get recent transactions for recovery
    pub fn recent_transactions(&self, count: usize) -> Vec<&Transaction> {
        let start = if self.transactions.len() > count {
            self.transactions.len() - count
        } else {
            0
        };
        self.transactions[start..].iter().collect()
    }

    /// Replay committed transactions (for crash recovery)
    pub fn replay(&self) -> Vec<&Transaction> {
        self.transactions
            .iter()
            .filter(|t| t.state == TransactionState::Committed)
            .collect()
    }
}
