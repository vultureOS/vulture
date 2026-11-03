/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What the kernel has said, kept where a person can get at it afterwards: two files on the EFI System Partition, which any
// computer can read. A real machine usually has no serial port, so otherwise what happened on it is only ever on its
// screen, for as long as it stays there.
//
//   Logs/boot.txt      this boot, from the start, written a little at a time. It stops when the file's full.
//   Logs/panic.txt     the end of the log, at the last kernel panic. It stays until the next one.
//
// Making these files is the job of whatever makes the disk (see Tools/vfs/FAT32.h), since this kernel can't change a FAT
// volume's structure: it only overwrites what's in them, sector by sector, which leaves the volume as sound as it found it.
// Without the files, or on a partition that can't be written, all of this quietly does nothing.

namespace Kernel::DiskLog {

// Once volumes are mounted. Finds the files and starts keeping the boot log up to date.
void start();

// In a panic, after everything's been said. It can't promise anything: this is the ordinary driver in a broken kernel. If
// that goes wrong, the report is still on the screen and the serial port, where it has already gone.
void write_panic();

}
