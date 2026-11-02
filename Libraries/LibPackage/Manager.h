/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibPackage/Catalog.h>

// Putting packages on the system, and taking them off. All of it wants root: that's for the
// callers to see to (sudo, or the installer service, which asks who is asking).

namespace Package {

struct Installed {
    String name;
    String version;
    String summary;
    Vector<String> files;
};

struct Repository {
    String name;
    String url;
    String key; // The Ed25519 public key, in hexadecimal.
    String certificates; // Whose certificates to trust for it; empty for the system's.
};

[[nodiscard]] ErrorOr<Vector<Repository>> configured_repositories();

// Asks each repository what it has, checks its signature, and keeps the answer for available().
// A repository that won't answer leaves what was known of it as it was, and the failure is
// reported. One whose index goes backward is refused: that is how an old, flawed version is
// offered again.
ErrorOr<void> refresh();
[[nodiscard]] ErrorOr<Vector<Available>> available();
[[nodiscard]] ErrorOr<Vector<Installed>> installed();

using Progress = Function<void(u32 percent)>;

// From the repository that offers it. Nothing is put in place until all of it has been checked
// and unpacked.
ErrorOr<void> install(StringView name, Progress const& = nullptr);
ErrorOr<void> remove(StringView name);

// The catalog, as the installer service describes it.
[[nodiscard]] String describe_catalog();

}
