#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Reads a file off the EFI System Partition of a whole-disk image, or changes what's in it, the way anything else that
understands FAT would find it: by the partition table and the directories, not by knowing where it was put.

    Meta/read-fat-file.py IMAGE PATH                 prints it
    Meta/read-fat-file.py IMAGE PATH --set TEXT      replaces its contents, padded with spaces; it can't grow
    Meta/read-fat-file.py IMAGE PATH --fill TEXT     fills it with TEXT, over and over
"""

import argparse
import struct
import sys

SECTOR = 512
ESP_TYPE = bytes.fromhex("28732AC11FF8D211BA4B00A0C93EC93B")


def find_partition(image):
    image.seek(SECTOR)
    header = image.read(92)
    if header[:8] != b"EFI PART":
        raise SystemExit("read-fat-file: no GPT on this disk")
    entries_at, entry_count, entry_size = struct.unpack_from("<QII", header, 72)
    image.seek(entries_at * SECTOR)
    for _ in range(entry_count):
        entry = image.read(entry_size)
        if entry[:16] == ESP_TYPE:
            return struct.unpack_from("<Q", entry, 32)[0] * SECTOR
    raise SystemExit("read-fat-file: no EFI System Partition on this disk")


class Volume:
    def __init__(self, image, start):
        self.image = image
        self.start = start
        image.seek(start)
        boot = image.read(SECTOR)
        self.bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
        self.sectors_per_cluster = boot[13]
        reserved = struct.unpack_from("<H", boot, 14)[0]
        fat_count = boot[16]
        fat_size = struct.unpack_from("<I", boot, 36)[0]
        self.root_cluster = struct.unpack_from("<I", boot, 44)[0]
        if fat_size == 0 or self.bytes_per_sector != SECTOR:
            raise SystemExit("read-fat-file: this isn't the FAT32 that was expected")
        self.fat_at = start + reserved * self.bytes_per_sector
        self.data_at = self.fat_at + fat_count * fat_size * self.bytes_per_sector
        self.cluster_size = self.sectors_per_cluster * self.bytes_per_sector

    def cluster_offset(self, cluster):
        return self.data_at + (cluster - 2) * self.cluster_size

    def chain(self, cluster):
        while 2 <= cluster < 0x0FFFFFF8:
            yield cluster
            self.image.seek(self.fat_at + cluster * 4)
            cluster = struct.unpack("<I", self.image.read(4))[0] & 0x0FFFFFFF

    def read_chain(self, cluster):
        data = bytearray()
        for link in self.chain(cluster):
            self.image.seek(self.cluster_offset(link))
            data += self.image.read(self.cluster_size)
        return bytes(data)

    def entries(self, cluster):
        data = self.read_chain(cluster)
        long_parts = []
        for offset in range(0, len(data), 32):
            record = data[offset : offset + 32]
            if record[0] == 0:
                return
            if record[0] == 0xE5:
                long_parts = []
                continue
            attributes = record[11]
            if attributes == 0x0F:
                text = record[1:11] + record[14:26] + record[28:32]
                long_parts.insert(0, text.decode("utf-16-le").split("\0")[0].rstrip("￿"))
                continue
            if long_parts:
                name = "".join(long_parts)
            else:
                stem = record[:8].decode("ascii", "replace").rstrip()
                extension = record[8:11].decode("ascii", "replace").rstrip()
                name = f"{stem}.{extension}" if extension else stem
            long_parts = []
            first = struct.unpack_from("<H", record, 20)[0] << 16 | struct.unpack_from("<H", record, 26)[0]
            size = struct.unpack_from("<I", record, 28)[0]
            yield name, bool(attributes & 0x10), first, size

    def find(self, path):
        cluster = self.root_cluster
        parts = [part for part in path.split("/") if part]
        for index, part in enumerate(parts):
            for name, is_directory, first, size in self.entries(cluster):
                if name.lower() != part.lower():
                    continue
                if index == len(parts) - 1:
                    return first, size
                if not is_directory:
                    raise SystemExit(f"read-fat-file: {name} isn't a directory")
                cluster = first
                break
            else:
                raise SystemExit(f"read-fat-file: there's no {path}")
        raise SystemExit(f"read-fat-file: there's no {path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("path")
    parser.add_argument("--set", dest="text")
    parser.add_argument("--fill")
    arguments = parser.parse_args()
    changing = arguments.text is not None or arguments.fill is not None
    with open(arguments.image, "r+b" if changing else "rb") as image:
        volume = Volume(image, find_partition(image))
        first, size = volume.find(arguments.path)
        if not changing:
            sys.stdout.buffer.write(volume.read_chain(first)[:size])
            return 0
        if arguments.fill is not None:
            arguments.text = (arguments.fill * (size // len(arguments.fill) + 1))[:size]
        new = arguments.text.encode()
        if len(new) > size:
            raise SystemExit(f"read-fat-file: {arguments.path} holds {size} bytes, not {len(new)}")
        new = new.ljust(size, b" ")
        written = 0
        for link in volume.chain(first):
            image.seek(volume.cluster_offset(link))
            image.write(new[written : written + volume.cluster_size])
            written += volume.cluster_size
    return 0


if __name__ == "__main__":
    sys.exit(main())
