#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Changes one bit of the block in a disk image that holds this (from a file, or as text repeated to fill a block),
# as a failing disk or someone with the disk in their hands would. Says which block that was.
#
#   flip-block.py IMAGE file PATH
#   flip-block.py IMAGE text TEXT

import sys

BLOCK = 4096


def main():
    image_path, kind, what = sys.argv[1:4]
    if kind == "file":
        with open(what, "rb") as f:
            looking_for = f.read(BLOCK)
        if len(looking_for) < BLOCK:
            sys.exit(f"flip-block: {what} is smaller than a block")
    else:
        text = what.encode()
        looking_for = (text * (BLOCK // len(text) + 1))[:BLOCK]
    with open(image_path, "r+b") as image:
        offset = 0
        while True:
            block = image.read(BLOCK)
            if not block:
                sys.exit("flip-block: not found")
            # (Partitions start 1 MiB in, which is on a block boundary.)
            if block == looking_for:
                image.seek(offset + 1000)
                old = image.read(1)
                image.seek(offset + 1000)
                image.write(bytes([old[0] ^ 1]))
                print(f"flip-block: changed one bit of the block at {offset}")
                return
            offset += BLOCK


main()
