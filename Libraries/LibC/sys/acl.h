/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <Kernel/API/POSIX/files.h>
#include <sys/cdefs.h>

// What these lines say: as a Mac's, whose names `chmod +a` and `ls -e` go by.
#define VULTURE_ACL_ALLOW 0
#define VULTURE_ACL_DENY 1
#define VULTURE_ACL_USER 0
#define VULTURE_ACL_GROUP 1
#define VULTURE_ACL_EVERYONE 2

#define VULTURE_ACL_READ_DATA (1u << 1)
#define VULTURE_ACL_LIST_DIRECTORY VULTURE_ACL_READ_DATA
#define VULTURE_ACL_WRITE_DATA (1u << 2)
#define VULTURE_ACL_ADD_FILE VULTURE_ACL_WRITE_DATA
#define VULTURE_ACL_EXECUTE (1u << 3)
#define VULTURE_ACL_SEARCH VULTURE_ACL_EXECUTE
#define VULTURE_ACL_DELETE (1u << 4)
#define VULTURE_ACL_APPEND_DATA (1u << 5)
#define VULTURE_ACL_ADD_SUBDIRECTORY VULTURE_ACL_APPEND_DATA
#define VULTURE_ACL_DELETE_CHILD (1u << 6)
#define VULTURE_ACL_READ_ATTRIBUTES (1u << 7)
#define VULTURE_ACL_WRITE_ATTRIBUTES (1u << 8)
#define VULTURE_ACL_READ_EXTATTRIBUTES (1u << 9)
#define VULTURE_ACL_WRITE_EXTATTRIBUTES (1u << 10)
#define VULTURE_ACL_READ_SECURITY (1u << 11)
#define VULTURE_ACL_WRITE_SECURITY (1u << 12)
#define VULTURE_ACL_CHANGE_OWNER (1u << 13)

#define VULTURE_ACL_INHERITED (1u << 0)
#define VULTURE_ACL_FILE_INHERIT (1u << 4)
#define VULTURE_ACL_DIRECTORY_INHERIT (1u << 5)
#define VULTURE_ACL_LIMIT_INHERIT (1u << 6)
#define VULTURE_ACL_ONLY_INHERIT (1u << 7)

__BEGIN_DECLS

// How many lines there are, which are put in `entries` if there's room. `follow` says what to do about a symbolic link.
int vulture_acl_get(char const* path, struct vulture_acl_entry* entries, unsigned int room, int follow);
int vulture_acl_set(char const* path, struct vulture_acl_entry const* entries, unsigned int count, int follow);

__END_DECLS
