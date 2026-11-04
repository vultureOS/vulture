#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Makes the tables iconv needs, from the Encoding Standard's indexes (https://encoding.spec.whatwg.org/, index-*.txt).

    Meta/generate-encodings.py INDEX_DIRECTORY Libraries/LibC/Generated/EncodingTables.cpp

The result is checked in, so building needs no download.
"""

import pathlib
import sys

SINGLE_BYTE = [
    "iso-8859-2", "iso-8859-3", "iso-8859-4", "iso-8859-5", "iso-8859-6", "iso-8859-7", "iso-8859-8", "iso-8859-10", "iso-8859-13", "iso-8859-14", "iso-8859-15", "iso-8859-16",
    "koi8-r", "koi8-u", "macintosh", "x-mac-cyrillic", "ibm866", "windows-874",
    "windows-1250", "windows-1251", "windows-1252", "windows-1253", "windows-1254", "windows-1255", "windows-1256", "windows-1257", "windows-1258",
]

HEADER = """/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Made by Meta/generate-encodings.py from the Encoding Standard's indexes. Don't edit it.

#include "../Encodings.h"

namespace LibC::Encodings {

"""


def read_index(directory, name):
    entries = {}
    with open(directory / f"index-{name}.txt", encoding="utf-8") as file:
        for raw in file:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            entries[int(fields[0])] = int(fields[1], 16)
    return entries


def table(out, kind, name, values, per_line=16):
    out.append(f"{kind} const {name}[] = {{")
    for i in range(0, len(values), per_line):
        out.append("    " + ", ".join(f"{value:#x}" for value in values[i : i + per_line]) + ",")
    out.append("};")
    out.append(f"size_t const {name}_count = {len(values)};")
    out.append("")


def flat(entries):
    size = max(entries) + 1
    return [entries.get(pointer, 0) for pointer in range(size)]


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    directory = pathlib.Path(sys.argv[1])
    out = []
    names = ", ".join(f'"{name}"' for name in SINGLE_BYTE)
    out.append(f"char const* const single_byte_names[single_byte_count] = {{ {names} }};")
    out.append("u16 const single_byte_tables[single_byte_count][128] = {")
    for name in SINGLE_BYTE:
        entries = read_index(directory, name)
        values = [entries.get(pointer, 0) for pointer in range(128)]
        assert max(values) < 0x10000
        out.append("    { " + ", ".join(f"{value:#x}" for value in values) + " },")
    out.append("};")
    out.append("")
    for index_name, name in (("jis0208", "jis0208"), ("jis0212", "jis0212"), ("euc-kr", "euc_kr"), ("gb18030", "gb18030")):
        values = flat(read_index(directory, index_name))
        assert max(values) < 0x10000, index_name
        table(out, "u16", name, values)
    table(out, "u32", "big5", flat(read_index(directory, "big5")))
    ranges = read_index(directory, "gb18030-ranges")
    pairs = []
    for pointer in sorted(ranges):
        pairs.extend((pointer, ranges[pointer]))
    table(out, "u32", "gb18030_ranges", pairs)
    pathlib.Path(sys.argv[2]).write_text(HEADER + "\n".join(out) + "\n}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
