/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/Bus/PCI/PCI.h>
#include <Kernel/Devices/Storage/BlockDevice.h>

// The disk drivers, for initialize_storage().

namespace Kernel {

// virtio-blk (vendor 0x1af4, device 0x1001 or 0x1042).
ErrorOr<NonnullRefPtr<BlockDevice>> create_virtio_block_device(PCI::Device const&, String name);

// NVM Express (class 01, subclass 08).
ErrorOr<NonnullRefPtr<BlockDevice>> create_nvme_device(PCI::Device const&, String name);
[[nodiscard]] StringView nvme_model(BlockDevice&);

// For a disk found some other way: the next number there is, and adding it.
[[nodiscard]] u32 take_disk_number();
void add_disk(NonnullRefPtr<BlockDevice>, StringView description);

// AHCI (class 01, subclass 06), with however many disks are on it.
ErrorOr<void> create_ahci_disks(PCI::Device const&, u32& next_disk, Vector<NonnullRefPtr<BlockDevice>>& disks);

}
