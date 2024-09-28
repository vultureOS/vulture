/**
 * @file drm.rs
 * @author Krisna Pranav
 * @brief drm
 * @version 3.0
 * @date 2024-09-28
 * 
 * @copyright Copyright (c) 2022-2024 vultureOS Developers, Krisna Pranav
 * 
*/


use crate::ioctl;
use core::ffi;

pub const DRM_IOCTL_BASE: usize = ' d' as usize;

#[inline]
pub const fn drm_io(nr: usize) -> usize {
    ioctl::io(DRM_IOCTL_BASE, nr)
}