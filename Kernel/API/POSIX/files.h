/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// open() flags, lseek() origins and file modes, shared by the kernel and
// LibC.

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_ACCMODE 0x0003
#define O_CREAT 0x0040
#define O_EXCL 0x0080
#define O_NOCTTY 0x0100
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
#define O_NONBLOCK 0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC 0x80000
#define O_NOFOLLOW 0x20000
// The kernel's own: open the link itself (for lstat-style lookups).
#define O_NOFOLLOW_INTERNAL O_NOFOLLOW

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFIFO 0010000

#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define S_IFBLK 0060000
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_IFSOCK 0140000
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

// fcntl() commands and descriptor flags.
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1

// access() modes.
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// Directory entry types (d_type).
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10

// One record of getdents(): records are packed back to back, each
// d_reclen bytes (8-byte aligned), with a NUL-terminated name.
struct vulture_dirent {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

#define PIPE_BUF 4096

// What fstat() and stat() report, in a fixed 64-bit layout.
struct vulture_stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    long long st_size;
    long long st_blksize;
    long long st_blocks;
    long long st_mtime_seconds;
    unsigned long long st_rdev;
    long long st_atime_seconds;
    long long st_ctime_seconds;
    long long st_birthtime_seconds;
    long st_atime_nanoseconds;
    long st_mtime_nanoseconds;
    long st_ctime_nanoseconds;
    long st_birthtime_nanoseconds;
};

// The *at() calls: a directory descriptor, or this for the working directory.
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100

// utimensat(): leave a time alone, or set it to now.
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
