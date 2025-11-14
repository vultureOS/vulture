//! # pranaUI — UI Framework
//!
//! Declarative (SwiftUI-like) and imperative (AppKit-like) UI framework
//! for building vulture applications. Features layout engine, animation
//! engine, accessibility, theming, and rich widget library.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use vulture_graphics::{Color, Rect};

// ─── Layout ─────────────────────────────────────────────────────────────────

/// Layout direction
#[derive(Debug, Clone, Copy)]
pub enum LayoutDirection {
    Horizontal,
    Vertical,
}

/// Alignment
#[derive(Debug, Clone, Copy)]
pub enum Alignment {
    Leading,
    Center,
    Trailing,
}

/// Edge insets (padding/margin)
#[derive(Debug, Clone, Copy)]
pub struct EdgeInsets {
    pub top: f32,
    pub right: f32,
    pub bottom: f32,
    pub left: f32,
}

impl EdgeInsets {
    pub const fn all(value: f32) -> Self {
        Self {
            top: value,
            right: value,
            bottom: value,
            left: value,
        }
    }

    pub const fn zero() -> Self {
        Self::all(0.0)
    }
}

// ─── Widgets ────────────────────────────────────────────────────────────────

/// Base widget types
#[derive(Debug, Clone)]
pub enum Widget {
    /// Text label
    Text {
        content: String,
        font_size: f32,
        color: Color,
        bold: bool,
    },
    /// Clickable button
    Button { label: String, enabled: bool },
    /// Text input field
    TextField {
        placeholder: String,
        value: String,
        secure: bool,
    },
    /// Toggle / checkbox
    Toggle { label: String, checked: bool },
    /// Slider
    Slider { min: f32, max: f32, value: f32 },
    /// Image view
    Image {
        source: String,
        width: u32,
        height: u32,
    },
    /// Vertical stack (VStack)
    VStack {
        children: Vec<Widget>,
        spacing: f32,
        alignment: Alignment,
    },
    /// Horizontal stack (HStack)
    HStack {
        children: Vec<Widget>,
        spacing: f32,
        alignment: Alignment,
    },
    /// List view
    List { items: Vec<Widget> },
    /// Scrollable container
    ScrollView {
        child: Box<Widget>,
        direction: LayoutDirection,
    },
    /// Custom spacer
    Spacer { size: Option<f32> },
    /// Divider line
    Divider,
    /// Progress indicator
    ProgressBar { value: f32, total: f32 },
}

// ─── Declarative UI Builder (SwiftUI-like) ──────────────────────────────────

/// Create a text widget
pub fn text(content: &str) -> Widget {
    Widget::Text {
        content: String::from(content),
        font_size: 14.0,
        color: Color::WHITE,
        bold: false,
    }
}

/// Create a button widget
pub fn button(label: &str) -> Widget {
    Widget::Button {
        label: String::from(label),
        enabled: true,
    }
}

/// Create a vertical stack
pub fn vstack(children: Vec<Widget>) -> Widget {
    Widget::VStack {
        children,
        spacing: 8.0,
        alignment: Alignment::Leading,
    }
}

/// Create a horizontal stack
pub fn hstack(children: Vec<Widget>) -> Widget {
    Widget::HStack {
        children,
        spacing: 8.0,
        alignment: Alignment::Center,
    }
}

// ─── Theme ──────────────────────────────────────────────────────────────────

/// System theme
#[derive(Debug, Clone)]
pub struct Theme {
    pub name: String,
    pub accent_color: Color,
    pub background: Color,
    pub surface: Color,
    pub text_primary: Color,
    pub text_secondary: Color,
    pub border: Color,
    pub corner_radius: f32,
}

impl Theme {
    /// Default dark theme (macOS-like)
    pub fn dark() -> Self {
        Self {
            name: String::from("Dark"),
            accent_color: Color::rgb(0, 122, 255),
            background: Color::rgb(30, 30, 30),
            surface: Color::rgb(44, 44, 46),
            text_primary: Color::WHITE,
            text_secondary: Color::rgb(142, 142, 147),
            border: Color::rgb(58, 58, 60),
            corner_radius: 8.0,
        }
    }

    /// Light theme
    pub fn light() -> Self {
        Self {
            name: String::from("Light"),
            accent_color: Color::rgb(0, 122, 255),
            background: Color::rgb(242, 242, 247),
            surface: Color::WHITE,
            text_primary: Color::BLACK,
            text_secondary: Color::rgb(60, 60, 67),
            border: Color::rgb(209, 209, 214),
            corner_radius: 8.0,
        }
    }
}

// ─── Accessibility ──────────────────────────────────────────────────────────

/// Accessibility role
#[derive(Debug, Clone, Copy)]
pub enum AccessibilityRole {
    Button,
    Label,
    TextField,
    Image,
    List,
    ListItem,
    Header,
    Link,
    Slider,
    Toggle,
}

/// Accessibility properties
#[derive(Debug, Clone)]
pub struct AccessibilityProps {
    pub role: AccessibilityRole,
    pub label: String,
    pub hint: String,
    pub value: String,
    pub is_enabled: bool,
}
