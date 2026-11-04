/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>

__BEGIN_DECLS

// What a thread's work is for, as a Mac says it. A thread with more to do for someone goes first.
typedef enum {
    QOS_CLASS_UNSPECIFIED = 0x00,
    QOS_CLASS_BACKGROUND = 0x09,
    QOS_CLASS_UTILITY = 0x11,
    QOS_CLASS_DEFAULT = 0x15,
    QOS_CLASS_USER_INITIATED = 0x19,
    QOS_CLASS_USER_INTERACTIVE = 0x21,
} qos_class_t;

#define QOS_MIN_RELATIVE_PRIORITY (-15)

qos_class_t qos_class_self(void);
qos_class_t qos_class_main(void);

__END_DECLS
