/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What's known of the machine's disks, a row each ("kern.disks"): whole disks and what they're divided into.
struct vulture_disk_info {
    char name[32];    // disk2s1
    char whole[32];   // disk2, the same for a whole disk
    char bus[16];     // USB, virtio-blk, disk image...
    char content[16]; // What it recognises as: msdos, exfat, cd9660, vulture-system, vulture-data, or nothing.
    char label[64];
    unsigned long long sectors;
    unsigned int sector_size;
    int read_only;
    int removable;
    int partition_number; // 0 for a whole disk.
    unsigned char partition_type[16];
    unsigned char partition_unique[16];
};

// On a disk's device. As a Mac's are, where a Mac has one.
#define DKIOCEJECT 0x20006415ul
#define DKIOCSYNCHRONIZE 0x20006416ul
#define DKIOCGETBLOCKSIZE 0x40046418ul
#define DKIOCGETBLOCKCOUNT 0x40086419ul
// Reads its partition table again, after someone has written a new one.
#define VULTURE_DKIOCRESCAN 0x20006480ul

// /dev/diskimages: a file as a disk. Its descriptor goes in, and the disk's number comes back.
#define VULTURE_DISKIMAGE_ATTACH 0x44490001ul
#define VULTURE_DISKIMAGE_ATTACH_READ_ONLY 0x44490002ul

// /dev/diskevents: a line for each disk that comes or goes: "+disk2s1", "-disk2s1". What's there already comes first.

// For mount(), with `type` "auto": whichever it recognises as.
