/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <pthread.h>
#include <sys/qos.h>

__BEGIN_DECLS

int pthread_set_qos_class_self_np(qos_class_t, int relative_priority);
int pthread_get_qos_class_np(pthread_t, qos_class_t*, int* relative_priority);
int pthread_attr_set_qos_class_np(pthread_attr_t*, qos_class_t, int relative_priority);
int pthread_attr_get_qos_class_np(pthread_attr_t*, qos_class_t*, int* relative_priority);

__END_DECLS
