/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <time.h>

__BEGIN_DECLS

// Unnamed semaphores, between the threads of one process.
typedef struct {
    unsigned value;
    unsigned waiters;
} sem_t;

#define SEM_VALUE_MAX 2147483647

int sem_init(sem_t*, int shared, unsigned value);
int sem_destroy(sem_t*);
int sem_post(sem_t*);
int sem_wait(sem_t*);
int sem_trywait(sem_t*);
int sem_timedwait(sem_t*, struct timespec const* absolute_time);
int sem_getvalue(sem_t*, int* value);

__END_DECLS
