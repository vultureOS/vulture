/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Debug/KernelLog.h>
#include <Kernel/Devices/Storage/StorageControllers.h>
#include <Kernel/Devices/VirtIO/VirtIO.h>

namespace Kernel {

namespace {

enum class Kind {
    None,
    VirtIO,
    NVMe,
    AHCI,
};

Kind kind_of(PCI::Device const& pci)
{
    if (pci.vendor_id() == VirtIO::vendor_id && (pci.device_id() == 0x1001 || pci.device_id() == 0x1042))
        return Kind::VirtIO;
    if (pci.class_code() == 0x01 && pci.subclass() == 0x08)
        return Kind::NVMe;
    if (pci.class_code() == 0x01 && pci.subclass() == 0x06)
        return Kind::AHCI;
    return Kind::None;
}

}

// Registers a disk and its partitions, and says so.
void add_disk(NonnullRefPtr<BlockDevice> device, StringView description)
{
    auto& disk = *device;
    if (auto result = BlockDevice::register_device(device); result.is_error()) {
        dmesgln("Storage: cannot register {}: {}", disk.name(), result.error());
        return;
    }
    auto partitions = scan_partitions(disk);
    dmesgln("Storage: {} ({}) {} MiB{}, {} partition{}", disk.name(), description, disk.block_count() * BlockDevice::block_size / MiB,
        disk.is_read_only() ? ", read-only"_sv : ""_sv, partitions.is_error() ? 0 : partitions.value(),
        partitions.is_error() || partitions.value() != 1 ? "s"_sv : ""_sv);
}

static u32 s_next_disk_number = 0;

u32 take_disk_number()
{
    return s_next_disk_number++;
}

// Disks are named in the order they are found: disk0, disk1...; their
// partitions disk0s1, disk0s2..., as on macOS.
void initialize_storage()
{
    for (auto& pci : PCI::devices()) {
        auto kind = kind_of(pci);
        if (kind == Kind::None)
            continue;
        if (kind == Kind::AHCI) {
            Vector<NonnullRefPtr<BlockDevice>> disks;
            if (auto result = create_ahci_disks(pci, s_next_disk_number, disks); result.is_error())
                dmesgln("Storage: AHCI at {:02x}:{:02x}.{}: {}", pci.bus(), pci.slot(), pci.function(), result.error());
            for (auto& disk : disks)
                add_disk(disk, "AHCI"_sv);
            continue;
        }
        StringBuilder name;
        name.appendff("disk{}", s_next_disk_number);
        auto device = kind == Kind::VirtIO ? create_virtio_block_device(pci, MUST(name.to_string())) : create_nvme_device(pci, MUST(name.to_string()));
        StringView driver = kind == Kind::VirtIO ? "virtio-blk"_sv : "NVMe"_sv;
        if (device.is_error()) {
            dmesgln("Storage: {} at {:02x}:{:02x}.{}: {}", driver, pci.bus(), pci.slot(), pci.function(), device.error());
            continue;
        }
        ++s_next_disk_number;
        StringBuilder description;
        description.append(driver);
        if (kind == Kind::NVMe) {
            description.append(", "_sv);
            description.append(nvme_model(*device.value()));
        }
        add_disk(device.value(), description.view());
    }
}

}
