//! # Clipboard Service
//!
//! System-wide clipboard for copy/paste operations.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;

/// Clipboard content types
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ClipboardType {
    PlainText,
    RichText,
    Html,
    Image,
    FileList,
    Custom(String),
}

/// A clipboard item
#[derive(Debug, Clone)]
pub struct ClipboardItem {
    pub content_type: ClipboardType,
    pub data: Vec<u8>,
    pub source_app: String,
    pub timestamp: u64,
}

/// The system clipboard
pub struct Clipboard {
    items: Vec<ClipboardItem>,
    max_history: usize,
}

static CLIPBOARD: Mutex<Clipboard> = Mutex::new(Clipboard {
    items: Vec::new(),
    max_history: 25,
});

impl Clipboard {
    /// Copy data to the clipboard
    pub fn copy(&mut self, item: ClipboardItem) {
        if self.items.len() >= self.max_history {
            self.items.remove(0);
        }
        self.items.push(item);
    }

    /// Paste (get the most recent item)
    pub fn paste(&self) -> Option<&ClipboardItem> {
        self.items.last()
    }

    /// Get clipboard history
    pub fn history(&self) -> &[ClipboardItem] {
        &self.items
    }

    /// Clear the clipboard
    pub fn clear(&mut self) {
        self.items.clear();
    }
}

/// Copy text to clipboard
pub fn copy_text(text: &str, source: &str) {
    CLIPBOARD.lock().copy(ClipboardItem {
        content_type: ClipboardType::PlainText,
        data: Vec::from(text.as_bytes()),
        source_app: String::from(source),
        timestamp: 0,
    });
}

/// Paste text from clipboard
pub fn paste_text() -> Option<String> {
    let clip = CLIPBOARD.lock();
    clip.paste().and_then(|item| {
        if item.content_type == ClipboardType::PlainText {
            String::from_utf8(item.data.clone()).ok()
        } else {
            None
        }
    })
}

/// Clipboard history entry
pub struct ClipboardEntry {
    pub data: alloc::vec::Vec<u8>,
    pub mime_type: alloc::string::String,
}
