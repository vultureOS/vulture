/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

// The unified log, as on macOS: entries go to logd, `log stream` and
// `log show` print them. Formats are printf's; "%{public}s" and
// "%{private}s" are taken as "%s" (nothing is redacted yet).

typedef struct vulture_os_log_s* os_log_t;
#define OS_LOG_DEFAULT ((os_log_t)0)

typedef enum {
    OS_LOG_TYPE_DEFAULT = 0x00,
    OS_LOG_TYPE_INFO = 0x01,
    OS_LOG_TYPE_DEBUG = 0x02,
    OS_LOG_TYPE_ERROR = 0x10,
    OS_LOG_TYPE_FAULT = 0x11,
} os_log_type_t;

// A log for one subsystem ("com.vulture.configd") and category ("server").
os_log_t os_log_create(char const* subsystem, char const* category);
bool os_log_type_enabled(os_log_t, os_log_type_t);
void vulture_os_log(os_log_t, os_log_type_t, char const* format, ...) __attribute__((format(os_log, 3, 4)));

#define os_log_with_type(log, type, format, ...) vulture_os_log((log), (type), (format), ##__VA_ARGS__)
#define os_log(log, format, ...) os_log_with_type((log), OS_LOG_TYPE_DEFAULT, (format), ##__VA_ARGS__)
#define os_log_info(log, format, ...) os_log_with_type((log), OS_LOG_TYPE_INFO, (format), ##__VA_ARGS__)
#define os_log_debug(log, format, ...) os_log_with_type((log), OS_LOG_TYPE_DEBUG, (format), ##__VA_ARGS__)
#define os_log_error(log, format, ...) os_log_with_type((log), OS_LOG_TYPE_ERROR, (format), ##__VA_ARGS__)
#define os_log_fault(log, format, ...) os_log_with_type((log), OS_LOG_TYPE_FAULT, (format), ##__VA_ARGS__)

__END_DECLS
