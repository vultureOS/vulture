/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// The records behind the sysctl names "kern.proc.all" (one per process)
// and "vfs.mounts" (one per mounted filesystem), shared by the kernel and
// the tools that read them (ps, top, df, mount).

struct vulture_process_info {
    int pid;
    int ppid;
    int pgid;
    int sid;
    unsigned uid;
    unsigned euid;
    unsigned gid;
    char state; // R running or runnable, S sleeping, T stopped, Z zombie
    char reserved[3];
    unsigned long long terminal;       // Device number of the controlling terminal, or 0.
    unsigned long long resident_bytes; // Memory the process has touched.
    unsigned long long cpu_nanoseconds;
    long long start_time; // Seconds since 1970.
    unsigned long long threads;
    char name[32];
};

struct vulture_mount_info {
    char type[16];
    char point[256];
    char device[32];
    unsigned long long block_size;
    unsigned long long blocks;
    unsigned long long free_blocks;
    int read_only;
    int reserved;
};
