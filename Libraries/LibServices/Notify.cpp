/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <NotifyEndpoint.h>
#include <fcntl.h>
#include <notify.h>
#include <string.h>
#include <unistd.h>

namespace {

NotifyProxy* notifyd()
{
    static NotifyProxy* proxy = nullptr;
    if (!proxy) {
        auto port = IPC::look_up("com.vulture.notifyd"_sv);
        if (port.is_error())
            return nullptr;
        proxy = new (Base::nothrow) NotifyProxy(port.release_value());
    }
    return proxy;
}

// What a token stands for, in this process.
struct Registration {
    bool used { false };
    String name;
    u64 last_count { 0 };
    bool checked { false };
};

constexpr int max_tokens = 256;
Registration s_registrations[max_tokens];

int new_token(char const* name)
{
    for (int token = 1; token < max_tokens; ++token) {
        if (!s_registrations[token].used) {
            s_registrations[token] = { true, MUST(String::from_view(StringView(name))), 0, false };
            return token;
        }
    }
    return -1;
}

Registration* registration(int token)
{
    if (token <= 0 || token >= max_tokens || !s_registrations[token].used)
        return nullptr;
    return &s_registrations[token];
}

bool valid_name(char const* name)
{
    return name && *name;
}

}

extern "C" {

uint32_t notify_post(char const* name)
{
    if (!valid_name(name))
        return NOTIFY_STATUS_INVALID_NAME;
    auto* proxy = notifyd();
    if (!proxy || proxy->post(StringView(name)).is_error())
        return NOTIFY_STATUS_FAILED;
    return NOTIFY_STATUS_OK;
}

uint32_t notify_register_check(char const* name, int* out_token)
{
    if (!valid_name(name))
        return NOTIFY_STATUS_INVALID_NAME;
    int token = new_token(name);
    if (token < 0)
        return NOTIFY_STATUS_FAILED;
    *out_token = token;
    return NOTIFY_STATUS_OK;
}

uint32_t notify_check(int token, int* check)
{
    auto* entry = registration(token);
    if (!entry)
        return NOTIFY_STATUS_INVALID_TOKEN;
    auto* proxy = notifyd();
    if (!proxy)
        return NOTIFY_STATUS_FAILED;
    auto count = proxy->post_count(entry->name.view());
    if (count.is_error())
        return NOTIFY_STATUS_FAILED;
    *check = !entry->checked || count.value() != entry->last_count;
    entry->checked = true;
    entry->last_count = count.value();
    return NOTIFY_STATUS_OK;
}

uint32_t notify_register_file_descriptor(char const* name, int* notify_fd, int flags, int* out_token)
{
    if (!valid_name(name))
        return NOTIFY_STATUS_INVALID_NAME;
    auto* proxy = notifyd();
    if (!proxy)
        return NOTIFY_STATUS_FAILED;
    // Only notifyd has the pipe's write end, so a second registration
    // cannot share a descriptor.
    if (flags & NOTIFY_REUSE)
        return NOTIFY_STATUS_INVALID_REQUEST;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return NOTIFY_STATUS_FAILED;
    int read_end = fds[0];
    int write_end = fds[1];
    int token = new_token(name);
    if (token < 0) {
        close(read_end);
        close(write_end);
        return NOTIFY_STATUS_FAILED;
    }
    auto error = proxy->register_file(StringView(name), IPC::Port(write_end), token);
    if (error.is_error() || error.value() != 0) {
        close(read_end);
        s_registrations[token].used = false;
        return NOTIFY_STATUS_FAILED;
    }
    *notify_fd = read_end;
    *out_token = token;
    return NOTIFY_STATUS_OK;
}

uint32_t notify_register_port(char const* name, int* receive_right, int, int* out_token)
{
    if (!valid_name(name))
        return NOTIFY_STATUS_INVALID_NAME;
    auto* proxy = notifyd();
    if (!proxy)
        return NOTIFY_STATUS_FAILED;
    auto port = IPC::create_port();
    if (port.is_error())
        return NOTIFY_STATUS_FAILED;
    int token = new_token(name);
    if (token < 0)
        return NOTIFY_STATUS_FAILED;
    auto error = proxy->register_port(StringView(name), move(port.value().send), token);
    if (error.is_error() || error.value() != 0) {
        s_registrations[token].used = false;
        return NOTIFY_STATUS_FAILED;
    }
    *receive_right = port.value().receive.release();
    *out_token = token;
    return NOTIFY_STATUS_OK;
}

uint32_t notify_cancel(int token)
{
    auto* entry = registration(token);
    if (!entry)
        return NOTIFY_STATUS_INVALID_TOKEN;
    if (auto* proxy = notifyd())
        (void)proxy->cancel(token);
    entry->used = false;
    entry->name = String();
    return NOTIFY_STATUS_OK;
}

uint32_t notify_set_state(int token, uint64_t state)
{
    auto* entry = registration(token);
    if (!entry)
        return NOTIFY_STATUS_INVALID_TOKEN;
    auto* proxy = notifyd();
    if (!proxy)
        return NOTIFY_STATUS_FAILED;
    auto error = proxy->set_state(entry->name.view(), state);
    if (error.is_error())
        return NOTIFY_STATUS_FAILED;
    return error.value() ? NOTIFY_STATUS_NOT_AUTHORIZED : NOTIFY_STATUS_OK;
}

uint32_t notify_get_state(int token, uint64_t* state)
{
    auto* entry = registration(token);
    if (!entry)
        return NOTIFY_STATUS_INVALID_TOKEN;
    auto* proxy = notifyd();
    if (!proxy)
        return NOTIFY_STATUS_FAILED;
    auto value = proxy->get_state(entry->name.view());
    if (value.is_error())
        return NOTIFY_STATUS_FAILED;
    *state = value.value();
    return NOTIFY_STATUS_OK;
}

uint32_t notify_is_valid_token(int token)
{
    return registration(token) != nullptr;
}

}
