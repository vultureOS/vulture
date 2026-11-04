/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// /dev/fsevents: what has changed on the system's data, from where a program last looked. The numbers
// in the flags are a Mac's own, as its FSEvents has them.
//
// Reading gives a run of these, each one padded to 8 bytes. It waits for there to be one, unless the
// descriptor says not to.
struct vulture_fsevent {
    unsigned long long id;
    unsigned int flags;
    unsigned int path_length; // With its terminating zero.
    // Followed by the path.
};

// Which paths, and from when: FSEVENTS_SINCE_NOW for only what's to come, or an id for what's happened
// after that, even from before the machine was last started, for as far back as anything is kept.
struct vulture_fsevents_watch {
    unsigned long long since;
    char const* const* paths; // All of them if there are none.
    unsigned int path_count;
    unsigned int reserved;
};

#define FSEVENTS_SINCE_NOW (~0ull)

#define FSEVENTS_WATCH 0x46530001ul
// The id of the most recent change.
#define FSEVENTS_LATEST 0x46530002ul

#define FSE_MUST_SCAN_SUBDIRECTORIES 0x00000001u
#define FSE_KERNEL_DROPPED 0x00000004u
#define FSE_HISTORY_DONE 0x00000010u
#define FSE_ROOT_CHANGED 0x00000020u
#define FSE_ITEM_CREATED 0x00000100u
#define FSE_ITEM_REMOVED 0x00000200u
#define FSE_ITEM_METADATA_CHANGED 0x00000400u
#define FSE_ITEM_RENAMED 0x00000800u
#define FSE_ITEM_MODIFIED 0x00001000u
#define FSE_ITEM_OWNER_CHANGED 0x00004000u
#define FSE_ITEM_XATTR_CHANGED 0x00008000u
#define FSE_ITEM_IS_FILE 0x00010000u
#define FSE_ITEM_IS_DIRECTORY 0x00020000u
#define FSE_ITEM_IS_SYMBOLIC_LINK 0x00040000u
#define FSE_ITEM_CLONED 0x00400000u
