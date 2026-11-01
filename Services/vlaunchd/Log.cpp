/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Log.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace Launchd {

namespace {

int s_log_fd = STDERR_FILENO;

}

void open_log(char const* path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0)
        s_log_fd = fd;
}

static bool s_debug_logging { false };

void set_debug_logging(bool debug)
{
    s_debug_logging = debug;
}

bool debug_logging()
{
    return s_debug_logging;
}

void debug(char const* format, ...)
{
    if (!s_debug_logging)
        return;
    char line[1024];
    int length = snprintf(line, sizeof(line), "vlaunchd: ");
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(line + length, sizeof(line) - static_cast<size_t>(length) - 1, format, arguments);
    va_end(arguments);
    if (written <= 0)
        return;
    size_t total = static_cast<size_t>(length) + static_cast<size_t>(written);
    if (total > sizeof(line) - 2)
        total = sizeof(line) - 2;
    line[total++] = '\n';
    (void)write(STDOUT_FILENO, line, total);
}

void log(char const* format, ...)
{
    char line[1024];
    time_t now = time(nullptr);
    struct tm broken;
    localtime_r(&now, &broken);
    size_t length = strftime(line, sizeof(line), "%b %e %H:%M:%S ", &broken);
    char host[64] = "localhost";
    gethostname(host, sizeof(host));
    if (char* dot = strchr(host, '.'))
        *dot = '\0';
    length += static_cast<size_t>(snprintf(line + length, sizeof(line) - length, "%s vlaunchd[%d]: ", host, getpid()));
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(line + length, sizeof(line) - length - 1, format, arguments);
    va_end(arguments);
    if (written > 0)
        length += static_cast<size_t>(written) < sizeof(line) - length - 1 ? static_cast<size_t>(written) : sizeof(line) - length - 2;
    line[length++] = '\n';
    (void)write(s_log_fd, line, length);
    if (s_debug_logging)
        (void)write(STDOUT_FILENO, line, length);
}

}
