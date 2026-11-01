/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <LogEndpoint.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

struct vulture_os_log_s {
    String subsystem;
    String category;
};

namespace {

// logd, found once (a process that cannot find it logs nothing).
LogProxy* logd()
{
    static LogProxy* proxy = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        auto port = IPC::look_up("com.vulture.logd"_sv);
        if (!port.is_error())
            proxy = new (Base::nothrow) LogProxy(port.release_value());
    }
    return proxy;
}

// printf's format, with os_log's "%{public}" and "%{private}" as "%".
void plain_format(char const* format, char* out, size_t size)
{
    size_t length = 0;
    for (char const* c = format; *c && length + 1 < size; ++c) {
        if (c[0] == '%' && c[1] == '{') {
            char const* close = strchr(c, '}');
            if (close) {
                out[length++] = '%';
                c = close;
                continue;
            }
        }
        out[length++] = *c;
    }
    out[length] = '\0';
}

}

extern "C" {

os_log_t os_log_create(char const* subsystem, char const* category)
{
    auto* log = new (Base::nothrow) vulture_os_log_s;
    if (!log)
        return OS_LOG_DEFAULT;
    log->subsystem = MUST(String::from_view(StringView(subsystem ? subsystem : "")));
    log->category = MUST(String::from_view(StringView(category ? category : "")));
    return log;
}

bool os_log_type_enabled(os_log_t, os_log_type_t type)
{
    // logd keeps everything; `log` decides what to show.
    return type == OS_LOG_TYPE_DEFAULT || type == OS_LOG_TYPE_INFO || type == OS_LOG_TYPE_DEBUG || type == OS_LOG_TYPE_ERROR
        || type == OS_LOG_TYPE_FAULT;
}

void vulture_os_log(os_log_t log, os_log_type_t type, char const* format, ...)
{
    auto* proxy = logd();
    if (!proxy)
        return;
    char plain[512];
    plain_format(format, plain, sizeof(plain));
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), plain, arguments);
    va_end(arguments);
    StringView subsystem = log ? log->subsystem.view() : StringView();
    StringView category = log ? log->category.view() : StringView();
    timeval now {};
    gettimeofday(&now, nullptr);
    u64 time = static_cast<u64>(now.tv_sec) * 1'000'000 + static_cast<u64>(now.tv_usec);
    (void)proxy->log(time, static_cast<u64>(gettid()), StringView(getprogname()), static_cast<u8>(type), subsystem, category, StringView(message));
}

}
