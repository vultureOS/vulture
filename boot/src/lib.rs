//! # Filename: lib
//!
//! ### Description
//! boot loader — Multiboot2 compatible boot sequence
//!
//! ### Legal Information
//! * **Copyright:** (C) 2022-2026 Krisna Pranav
//! * **License:** GNU General Public License v3.0 (GPL-3.0-or-later)
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

#![no_std]

/// Multiboot2 tag types
pub const TAG_TYPE_END: u32 = 0;
pub const TAG_TYPE_CMDLINE: u32 = 1;
pub const TAG_TYPE_BOOT_LOADER: u32 = 2;
pub const TAG_TYPE_MODULE: u32 = 3;
pub const TAG_TYPE_BASIC_MEMINFO: u32 = 4;
pub const TAG_TYPE_BOOTDEV: u32 = 5;
pub const TAG_TYPE_MMAP: u32 = 6;
pub const TAG_TYPE_FRAMEBUFFER: u32 = 8;
pub const TAG_TYPE_ELF_SECTIONS: u32 = 9;
pub const TAG_TYPE_APM: u32 = 10;

/// Multiboot2 tag header
#[repr(C)]
pub struct MultibootTag {
    pub type_: u32,
    pub size: u32,
}

/// Memory map entry from GRUB
#[repr(C)]
pub struct MemoryMapEntry {
    pub base: u64,
    pub len: u64,
    pub type_: u32,
    pub reserved: u32,
}

/// Memory region types
pub const MEMORY_AVAILABLE: u32 = 1;
pub const MEMORY_RESERVED: u32 = 2;
pub const MEMORY_ACPI_RECLAIMABLE: u32 = 3;
pub const MEMORY_NVS: u32 = 4;
pub const MEMORY_BADRAM: u32 = 5;

/// Basic memory info
#[repr(C)]
pub struct BasicMemInfo {
    pub mem_lower: u32, // in KB
    pub mem_upper: u32, // in KB
}

/// Framebuffer info from bootloader
#[repr(C)]
pub struct FramebufferInfo {
    pub addr: u64,
    pub pitch: u32,
    pub width: u32,
    pub height: u32,
    pub bpp: u8,
    pub fb_type: u8,
}

/// Parse memory map from multiboot info
///
/// # Safety
/// The address must point to valid multiboot2 information.
pub unsafe fn parse_memory_map(_addr: u64) -> Option<BasicMemInfo> {
    // In production, walk the multiboot2 tag list starting at `addr`
    // For now, return a default
    Some(BasicMemInfo {
        mem_lower: 640,    // 640 KB conventional memory
        mem_upper: 262144, // 256 MB (in KB)
    })
}

/// Boot information passed from bootloader to kernel
pub struct BootInfo {
    pub mem_lower_kb: u32,
    pub mem_upper_kb: u32,
    pub framebuffer: Option<FramebufferInfo>,
    pub command_line: Option<&'static str>,
}

impl BootInfo {
    pub fn total_memory_mb(&self) -> u32 {
        (self.mem_lower_kb + self.mem_upper_kb) / 1024
    }
}
