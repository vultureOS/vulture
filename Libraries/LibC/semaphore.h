/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <sys/cdefs.h>
#include <time.h>

__BEGIN_DECLS

// Semaphores: in memory that a program has, or by name, for anyone.
typedef struct {
    unsigned value;
    unsigned waiters;
} sem_t;

#define SEM_VALUE_MAX 2147483647
#define SEM_FAILED ((sem_t*)-1)

sem_t* sem_open(char const* name, int flags, ...);
int sem_close(sem_t*);
int sem_unlink(char const* name);

int sem_init(sem_t*, int shared, unsigned value);
int sem_destroy(sem_t*);
int sem_post(sem_t*);
int sem_wait(sem_t*);
int sem_trywait(sem_t*);
int sem_timedwait(sem_t*, struct timespec const* absolute_time);
int sem_getvalue(sem_t*, int* value);

__END_DECLS
