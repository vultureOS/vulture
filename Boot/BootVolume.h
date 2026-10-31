/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Boot/Memory.h>

namespace Boot {

struct LoadedFile {
    UEFI::PhysicalAddress base { 0 };
    size_t size { 0 };

    [[nodiscard]] Bytes bytes() const { return { reinterpret_cast<u8*>(base), size }; }
};

// The FAT volume VultureBoot was loaded from (the EFI System Partition).
class BootVolume {
public:
    static ErrorOr<BootVolume> open(UEFI::Handle image_handle, UEFI::BootServices&);

    // Reads a whole file into newly allocated pages of the given type. An
    // empty Optional means the file does not exist.
    ErrorOr<Optional<LoadedFile>> load(char16_t const* path, LoaderMemoryType);

private:
    BootVolume() = default;

    UEFI::BootServices* m_boot_services { nullptr };
    UEFI::FileProtocol* m_root { nullptr };
};

}
