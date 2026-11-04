/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// kqueue's: the same numbers and layout as a Mac's.

#define EVFILT_READ (-1)
#define EVFILT_WRITE (-2)
#define EVFILT_VNODE (-4)
#define EVFILT_PROC (-5)
#define EVFILT_SIGNAL (-6)
#define EVFILT_TIMER (-7)
#define EVFILT_USER (-10)

#define EV_ADD 0x0001
#define EV_DELETE 0x0002
#define EV_ENABLE 0x0004
#define EV_DISABLE 0x0008
#define EV_ONESHOT 0x0010
#define EV_CLEAR 0x0020
#define EV_RECEIPT 0x0040
#define EV_DISPATCH 0x0080
#define EV_ERROR 0x4000
#define EV_EOF 0x8000

// EVFILT_VNODE
#define NOTE_DELETE 0x00000001
#define NOTE_WRITE 0x00000002
#define NOTE_EXTEND 0x00000004
#define NOTE_ATTRIB 0x00000008
#define NOTE_LINK 0x00000010
#define NOTE_RENAME 0x00000020
#define NOTE_REVOKE 0x00000040

// EVFILT_PROC
#define NOTE_EXIT 0x80000000
#define NOTE_FORK 0x40000000
#define NOTE_EXEC 0x20000000
#define NOTE_EXITSTATUS 0x04000000

// EVFILT_TIMER: the default is milliseconds.
#define NOTE_SECONDS 0x00000001
#define NOTE_USECONDS 0x00000002
#define NOTE_NSECONDS 0x00000004

// EVFILT_USER
#define NOTE_TRIGGER 0x01000000
#define NOTE_FFNOP 0x00000000
#define NOTE_FFAND 0x40000000
#define NOTE_FFOR 0x80000000
#define NOTE_FFCOPY 0xc0000000
#define NOTE_FFCTRLMASK 0xc0000000
#define NOTE_FFLAGSMASK 0x00ffffff

struct vulture_kevent {
    unsigned long ident;
    short filter;
    unsigned short flags;
    unsigned int fflags;
    long data;
    void* udata;
};
