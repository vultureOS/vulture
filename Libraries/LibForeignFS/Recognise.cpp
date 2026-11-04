/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/errno.h>
#include <LibForeignFS/Volume.h>

namespace ForeignFS {

Kind recognise(Store& store)
{
    if (store.sector_count() == 0)
        return Kind::None;
    u8 sector[Store::sector_size];
    if (store.read_sectors(0, 1, { sector, sizeof(sector) }).is_error())
        return Kind::None;

    if (sector[510] == 0x55 && sector[511] == 0xaa) {
        if (__builtin_memcmp(sector + 3, "EXFAT   ", 8) == 0)
            return Kind::ExFAT;
        // What FAT has in its first sector, which a partition table has no reason to.
        u16 bytes_per_sector;
        __builtin_memcpy(&bytes_per_sector, sector + 11, 2);
        u8 const sectors_per_cluster = sector[13];
        u16 reserved;
        __builtin_memcpy(&reserved, sector + 14, 2);
        u8 const fat_count = sector[16];
        bool const jumps = sector[0] == 0xeb || sector[0] == 0xe9;
        bool const sensible = bytes_per_sector >= 512 && bytes_per_sector <= 4096 && (bytes_per_sector & (bytes_per_sector - 1)) == 0
            && sectors_per_cluster != 0 && (sectors_per_cluster & (sectors_per_cluster - 1)) == 0 && reserved != 0 && fat_count != 0 && fat_count <= 4;
        if (jumps && sensible)
            return Kind::FAT;
    }

    // ISO 9660 leaves its first 32 KiB to whatever else wants them.
    constexpr u64 iso_descriptor_sector = 16 * 2048 / Store::sector_size;
    if (store.sector_count() > iso_descriptor_sector && !store.read_sectors(iso_descriptor_sector, 1, { sector, sizeof(sector) }).is_error()) {
        if (__builtin_memcmp(sector + 1, "CD001", 5) == 0)
            return Kind::ISO9660;
    }
    return Kind::None;
}

StringView name_of(Kind kind)
{
    switch (kind) {
    case Kind::FAT:
        return "msdos"_sv;
    case Kind::ExFAT:
        return "exfat"_sv;
    case Kind::ISO9660:
        return "cd9660"_sv;
    case Kind::None:
        break;
    }
    return {};
}

ErrorOr<NonnullOwnPtr<Volume>> open_volume(Store& store, Clock clock, bool read_only)
{
    switch (recognise(store)) {
    case Kind::FAT:
        return open_fat(store, clock, read_only);
    case Kind::ExFAT:
        return open_exfat(store, clock, read_only);
    case Kind::ISO9660:
        return open_iso9660(store);
    case Kind::None:
        break;
    }
    return Error::from_errno(EINVAL);
}

}
