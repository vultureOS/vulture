/**
 * @file ioctl.rs
 * @author Krisna Pranav
 * @brief ioctl
 * @version 3.0
 * @date 2024-09-28
 * 
 * @copyright Copyright (c) 2022-2024 vultureOS Developers, Krisna Pranav
 * 
 */


pub const IOC_NRBITS: usize = 8;
pub const IOC_TYPEBITS: usize = 8;
pub const IOC_SIZEBITS: usize = 14;
pub const IOC_NRSHIFT: usize = 0;
pub const IOC_TYPESHIFT: usize = IOC_NRSHIFT + IOC_NRBITS;
pub const IOC_SIZESHIFT: usize = IOC_TYPESHIFT + IOC_TYPEBITS;
pub const IOC_DIRSHIFT: usize = IOC_SIZESHIFT + IOC_SIZEBITS;
pub const IOC_NONE: usize = 0;
pub const IOC_WRITE: usize = 1;
pub const IOC_READ: usize = 2;

pub const fn ioc(dir: usize, ty: usize, nr: usize, size: usize) -> usize {
    ((dir) << IOC_DIRSHIFT)
        | ((ty) << IOC_TYPESHIFT)
        | ((nr) << IOC_NRSHIFT)
        | ((size) << IOC_SIZESHIFT)
}

#[inline]
pub const fn io(typ: usize, nr: usize) -> usize {
    ioc(IOC_NONE, typ, nr, 0)
}

#[inline]
pub const fn ior<T>(typ: usize, nr: usize) -> usize {
    ioc(IOC_READ, typ, nr, core::mem::size_of::<T>())
}