#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Marks a free cluster near the end of a FAT32 or exFAT volume as in use by nothing, as a machine that lost power part way
# through would have left it: what a repair has to find and put right.
#
#   damage-volume.py IMAGE

import struct
import sys


def main():
    with open(sys.argv[1], "r+b") as image:
        boot = image.read(512)
        if boot[3:11] == b"EXFAT   ":
            fat_offset, fat_length, heap, clusters, root = struct.unpack_from("<IIIII", boot, 80)
            sector = 1 << boot[108]
            cluster = sector << boot[109]
            image.seek(heap * sector + (root - 2) * cluster)
            entries = image.read(cluster)
            for at in range(0, len(entries), 32):
                if entries[at] == 0x81:
                    bitmap = struct.unpack_from("<I", entries, at + 20)[0]
                    break
            else:
                sys.exit("damage-volume: there's no bitmap")
            index = clusters - 5
            where = heap * sector + (bitmap - 2) * cluster + index // 8
            image.seek(where)
            byte = image.read(1)[0]
            image.seek(where)
            image.write(bytes([byte | (1 << (index % 8))]))
            return
        sector = struct.unpack_from("<H", boot, 11)[0]
        per_cluster = boot[13]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fats = boot[16]
        total = struct.unpack_from("<I", boot, 32)[0]
        fat_size = struct.unpack_from("<I", boot, 36)[0]
        clusters = (total - reserved - fats * fat_size) // per_cluster
        index = clusters + 2 - 5
        for copy in range(fats):
            image.seek((reserved + copy * fat_size) * sector + index * 4)
            image.write(struct.pack("<I", 0x0fffffff))


main()
