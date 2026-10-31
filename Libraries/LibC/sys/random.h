/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

// Unpredictable bytes from the kernel (ChaCha20, keyed from firmware entropy).
ssize_t getrandom(void* buffer, size_t length, unsigned flags);
int getentropy(void* buffer, size_t length);
void arc4random_buf(void* buffer, size_t length);
unsigned arc4random(void);
unsigned arc4random_uniform(unsigned bound);

__END_DECLS
