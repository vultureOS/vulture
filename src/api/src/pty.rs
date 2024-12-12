/**
 * @file pty.rs
 * @author Krisna Pranav
 * @brief pty
 * @version 3.0
 * @date 2024-09-28
 *
 * @copyright Copyright (c) 2022-2024 vultureOS Developers, Krisna Pranav
 *
 */
use crate::ioctl;

pub const TIOCGPTN: usize = ioctl::ior::<u32>('T' as usize, 0x30);
pub const TIOCSPTLCK: usize = ioctl::iow::<i32>('T' as usize, 0x31);
