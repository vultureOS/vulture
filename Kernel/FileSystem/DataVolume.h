/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/FileSystem/FileSystem.h>

// What a system keeps that changes, on a volume apart from the sealed one, as a Mac has it: mounted at
// /System/Volumes/Data, and showing through at /Users, /private and so on by firmlinks (usr/share/firmlinks says
// which). It gets whatever it lacks of what a system needs from System/Library/Templates/Data, each time it's
// attached: which is everything the first time, and whatever's new after an update. /private/tmp is left empty.
namespace Kernel::DataVolume {

ErrorOr<void> attach(NonnullRefPtr<FileSystem>);

}
