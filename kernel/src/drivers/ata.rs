//! # ATA PIO Driver
//!
//! Basic LBA28 PIO driver for IDE disks to provide persistent storage.
//!
//! SPDX-License-Identifier: GPL-3.0-or-later

use alloc::vec::Vec;
use lazy_static::lazy_static;
use spin::Mutex;
use x86_64::instructions::port::Port;

const ATA_PRIMARY_IO: u16 = 0x1F0;
const ATA_PRIMARY_CTRL: u16 = 0x3F6;

const ATA_REG_DATA: u16 = ATA_PRIMARY_IO;
const ATA_REG_ERROR_FEATURES: u16 = ATA_PRIMARY_IO + 1;
const ATA_REG_SECTOR_COUNT: u16 = ATA_PRIMARY_IO + 2;
const ATA_REG_LBA0: u16 = ATA_PRIMARY_IO + 3;
const ATA_REG_LBA1: u16 = ATA_PRIMARY_IO + 4;
const ATA_REG_LBA2: u16 = ATA_PRIMARY_IO + 5;
const ATA_REG_HDDEVSEL: u16 = ATA_PRIMARY_IO + 6;
const ATA_REG_COMMAND_STATUS: u16 = ATA_PRIMARY_IO + 7;

const ATA_CMD_READ_PIO: u8 = 0x20;
const ATA_CMD_WRITE_PIO: u8 = 0x30;
const ATA_CMD_IDENTIFY: u8 = 0xEC;

pub struct AtaDrive {
    is_master: bool,
}

lazy_static! {
    pub static ref PRIMARY_MASTER: Mutex<AtaDrive> = Mutex::new(AtaDrive { is_master: true });
}

impl AtaDrive {
    /// Wait for the drive to become ready
    fn wait_ready(&self) -> Result<(), &'static str> {
        let mut status_port = Port::<u8>::new(ATA_REG_COMMAND_STATUS);
        for _ in 0..10000 {
            let status = unsafe { status_port.read() };
            if (status & 0x80) == 0 {
                // BSY cleared
                if (status & 0x01) != 0 {
                    return Err("ATA Drive Error");
                }
                return Ok(());
            }
        }
        Err("ATA Timeout")
    }

    /// Read a single 512-byte sector using LBA28
    pub fn read_sector(&self, lba: u32, buf: &mut [u8; 512]) -> Result<(), &'static str> {
        let drive_sel = if self.is_master { 0xE0 } else { 0xF0 };

        unsafe {
            Port::<u8>::new(ATA_REG_HDDEVSEL).write(drive_sel | ((lba >> 24) & 0x0F) as u8);
            Port::<u8>::new(ATA_REG_ERROR_FEATURES).write(0); // Clear error register technically
            Port::<u8>::new(ATA_REG_SECTOR_COUNT).write(1);
            Port::<u8>::new(ATA_REG_LBA0).write(lba as u8);
            Port::<u8>::new(ATA_REG_LBA1).write((lba >> 8) as u8);
            Port::<u8>::new(ATA_REG_LBA2).write((lba >> 16) as u8);
            Port::<u8>::new(ATA_REG_COMMAND_STATUS).write(ATA_CMD_READ_PIO);
        }

        self.wait_ready()?;

        unsafe {
            let mut data_port = Port::<u16>::new(ATA_REG_DATA);
            for i in 0..256 {
                let word = data_port.read();
                buf[i * 2] = (word & 0xFF) as u8;
                buf[i * 2 + 1] = (word >> 8) as u8;
            }
        }

        Ok(())
    }

    /// Write a single 512-byte sector using LBA28
    pub fn write_sector(&self, lba: u32, buf: &[u8; 512]) -> Result<(), &'static str> {
        let drive_sel = if self.is_master { 0xE0 } else { 0xF0 };

        unsafe {
            Port::<u8>::new(ATA_REG_HDDEVSEL).write(drive_sel | ((lba >> 24) & 0x0F) as u8);
            Port::<u8>::new(ATA_REG_ERROR_FEATURES).write(0);
            Port::<u8>::new(ATA_REG_SECTOR_COUNT).write(1);
            Port::<u8>::new(ATA_REG_LBA0).write(lba as u8);
            Port::<u8>::new(ATA_REG_LBA1).write((lba >> 8) as u8);
            Port::<u8>::new(ATA_REG_LBA2).write((lba >> 16) as u8);
            Port::<u8>::new(ATA_REG_COMMAND_STATUS).write(ATA_CMD_WRITE_PIO);
        }

        self.wait_ready()?;

        unsafe {
            let mut data_port = Port::<u16>::new(ATA_REG_DATA);
            for i in 0..256 {
                let word = (buf[i * 2] as u16) | ((buf[i * 2 + 1] as u16) << 8);
                data_port.write(word);
            }

            // Flush cache
            Port::<u8>::new(ATA_REG_COMMAND_STATUS).write(0xE7);
            self.wait_ready()?;
        }

        Ok(())
    }
}

pub fn init() {
    crate::serial_println!("[ata] Initializing ATA PIO driver...");
    let drive = PRIMARY_MASTER.lock();
    crate::serial_println!("[ata] Primary Master selected.");
}
