/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/Inode.h>
#include <LibBase/Error.h>
#include <LibBase/Span.h>

// What may run. Code is only ever a program or library file, never memory that was written: nothing
// anonymous may become executable, and nothing that has been writable. And the file must be vouched
// for, by its SHA-256:
//
//   - by the system's seal (Security/Seal.h), for what came with the system;
//   - or by root, who adds it to a list here, as the installer does for a package it has checked
//     the signature of (LibPackage).
//
// Anything else gets EPERM, and a line in the kernel's log. The boot word "nocodesign" turns this off.

namespace Kernel::CodeSigning {

constexpr size_t hash_size = 32;

enum class Origin : u8 {
    // Came with the system.
    System,
    // Installed; or, with signing turned off, anything.
    ThirdParty,
};

void initialize(bool enforced);
[[nodiscard]] bool is_enforced();

// exec: these are the bytes about to be loaded, from this file. (They are a copy, so what is checked
// is what runs, whatever then happens to the file.)
[[nodiscard]] ErrorOr<Origin> check_program(Inode&, ReadonlyBytes, StringView path);
// mmap(PROT_EXEC), and mprotect to it: the pages will come straight from the file, so this also
// wants nobody but root to be able to change it.
[[nodiscard]] ErrorOr<void> check_mapped_file(Inode&);

// For root. How the installer vouches for what it installed, and takes that back.
[[nodiscard]] ErrorOr<void> trust(u8 const* hash);
void distrust(u8 const* hash);

}
