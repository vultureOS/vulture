/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/RefPtr.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// A process can give up what it doesn't need, for good, and so can everything it starts: anything not
// allowed by its sandbox profile gets EPERM, root or not. A second profile only ever narrows the
// first, as both must allow a thing. What the process already had open it keeps.
//
// What is checked is the path after every link is followed, so that a link can't lead anywhere a
// direct path couldn't.
//
// A profile is lines of text: LibC puts it together from the files in
// /System/Library/Sandbox/Profiles.
//
//     name NAME                 what the log calls it
//     read PATH                 reading that, or anything under it
//     write PATH                reading and changing it
//     exec PATH                 running it
//     metadata PATH             stat and the like (anything it can read, and the folders on the way
//                               to anything it can read, it may stat already)
//     service NAME              looking up that service (asked of vlaunchd, which checks here)
//     network outbound          making connections, sending
//     network inbound           accepting them
//     network configure         setting an interface's address
//
// A sandboxed process can't run anything set-ID, signal anything outside its own sandbox, see other
// processes, change a sysctl, or shut the machine down.

namespace Kernel {

enum class SandboxOperation : u8 {
    FileRead,
    FileWrite,
    FileMetadata,
    Execute,
    NetworkOutbound,
    NetworkInbound,
    NetworkConfigure,
    Service,
};

class SandboxProfile final : public RefCounted<SandboxProfile> {
public:
    [[nodiscard]] static ErrorOr<NonnullRefPtr<SandboxProfile>> parse(StringView text, RefPtr<SandboxProfile> outer);

    [[nodiscard]] bool allows(SandboxOperation, StringView what) const;
    [[nodiscard]] StringView name() const { return m_name.view(); }

private:
    SandboxProfile() = default;

    [[nodiscard]] bool allows_here(SandboxOperation, StringView what) const;

    String m_name;
    RefPtr<SandboxProfile> m_outer;
    Vector<String> m_read;
    Vector<String> m_write;
    Vector<String> m_execute;
    Vector<String> m_metadata;
    Vector<String> m_services;
    bool m_outbound { false };
    bool m_inbound { false };
    bool m_configure { false };
};

// For the current process, if there is one and it is sandboxed: says no, and why, in the log.
[[nodiscard]] ErrorOr<void> sandbox_check(SandboxOperation, StringView what);

}
