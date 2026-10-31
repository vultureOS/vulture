/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/BootVolume.h>

namespace Boot {

ErrorOr<BootVolume> BootVolume::open(UEFI::Handle image_handle, UEFI::BootServices& boot_services)
{
    void* interface = nullptr;
    if (UEFI::is_error(boot_services.HandleProtocol(image_handle, &UEFI::loaded_image_protocol_guid, &interface)) || !interface)
        return Error::from_string_literal("cannot find the loader's own image");
    auto* loaded_image = static_cast<UEFI::LoadedImageProtocol*>(interface);

    interface = nullptr;
    if (UEFI::is_error(boot_services.HandleProtocol(loaded_image->DeviceHandle, &UEFI::simple_file_system_protocol_guid, &interface)) || !interface)
        return Error::from_string_literal("the boot device has no file system");
    auto* file_system = static_cast<UEFI::SimpleFileSystemProtocol*>(interface);

    BootVolume volume;
    volume.m_boot_services = &boot_services;
    if (UEFI::is_error(file_system->OpenVolume(file_system, &volume.m_root)))
        return Error::from_string_literal("cannot open the boot volume");
    return volume;
}

ErrorOr<Optional<LoadedFile>> BootVolume::load(char16_t const* path, LoaderMemoryType type)
{
    UEFI::FileProtocol* file = nullptr;
    auto status = m_root->Open(m_root, &file, path, UEFI::file_mode_read, 0);
    if (status == UEFI::StatusCode::NotFound)
        return Optional<LoadedFile> {};
    if (UEFI::is_error(status))
        return Error::from_string_literal("cannot open file");

    u64 size = 0;
    if (UEFI::is_error(file->SetPosition(file, UEFI::file_position_end)) || UEFI::is_error(file->GetPosition(file, &size)) || UEFI::is_error(file->SetPosition(file, 0))) {
        file->Close(file);
        return Error::from_string_literal("cannot determine file size");
    }

    auto base = allocate_pages(*m_boot_services, size, type);
    if (base.is_error()) {
        file->Close(file);
        return base.release_error();
    }

    // Firmware may return fewer bytes than asked for; keep reading.
    u64 done = 0;
    while (done < size) {
        UEFI::UIntN chunk = size - done;
        if (UEFI::is_error(file->Read(file, &chunk, reinterpret_cast<u8*>(base.value()) + done)) || chunk == 0) {
            file->Close(file);
            return Error::from_string_literal("cannot read file");
        }
        done += chunk;
    }
    file->Close(file);
    return Optional<LoadedFile> { LoadedFile { base.value(), size } };
}

}
