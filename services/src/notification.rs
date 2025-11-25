//! # Notification Service
//!
//! System notification center for delivering alerts to the user.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;

/// Notification priority
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NotificationPriority {
    Low,
    Default,
    High,
    Critical,
}

/// A notification
#[derive(Debug, Clone)]
pub struct Notification {
    pub id: u64,
    pub title: String,
    pub body: String,
    pub source: String,
    pub priority: NotificationPriority,
    pub timestamp: u64,
    pub read: bool,
}

/// Notification center
static NOTIFICATIONS: Mutex<NotificationCenter> = Mutex::new(NotificationCenter::new());

struct NotificationCenter {
    notifications: Vec<Notification>,
    next_id: u64,
    max_notifications: usize,
}

impl NotificationCenter {
    const fn new() -> Self {
        Self {
            notifications: Vec::new(),
            next_id: 1,
            max_notifications: 100,
        }
    }
}

/// Post a notification
pub fn post(title: &str, body: &str, source: &str, priority: NotificationPriority) -> u64 {
    let mut center = NOTIFICATIONS.lock();
    let id = center.next_id;
    center.next_id += 1;

    if center.notifications.len() >= center.max_notifications {
        center.notifications.remove(0);
    }

    center.notifications.push(Notification {
        id,
        title: String::from(title),
        body: String::from(body),
        source: String::from(source),
        priority,
        timestamp: 0,
        read: false,
    });

    id
}

/// Get unread notification count
pub fn unread_count() -> usize {
    NOTIFICATIONS
        .lock()
        .notifications
        .iter()
        .filter(|n| !n.read)
        .count()
}

/// Mark a notification as read
pub fn mark_read(id: u64) {
    let mut center = NOTIFICATIONS.lock();
    if let Some(n) = center.notifications.iter_mut().find(|n| n.id == id) {
        n.read = true;
    }
}

/// Clear all notifications
pub fn clear_all() {
    NOTIFICATIONS.lock().notifications.clear();
}
