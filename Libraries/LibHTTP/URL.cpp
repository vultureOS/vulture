/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/CharacterTypes.h>
#include <LibBase/String.h>
#include <LibHTTP/URL.h>

namespace HTTP {

namespace {

String lowered(StringView text)
{
    StringBuilder builder;
    for (size_t i = 0; i < text.length(); ++i) {
        char const c = static_cast<char>(to_ascii_lowercase(text[i]));
        builder.append({ &c, 1 });
    }
    return MUST(builder.to_string());
}

}

ErrorOr<URL> URL::parse(StringView text)
{
    URL url;
    StringView rest = text;
    if (auto found = rest.find("://"_sv); found.has_value()) {
        url.scheme = lowered(rest.substring_view(0, found.value()));
        rest = rest.substring_view(found.value() + 3);
    } else {
        url.scheme = MUST(String::from_view("http"_sv));
    }
    if (url.scheme.view() != "http"_sv && url.scheme.view() != "https"_sv)
        return Error::from_string_literal("URL: a scheme other than http and https");

    size_t host_end = rest.length();
    for (size_t i = 0; i < rest.length(); ++i) {
        if (rest[i] == '/' || rest[i] == '?' || rest[i] == '#') {
            host_end = i;
            break;
        }
    }
    StringView authority = rest.substring_view(0, host_end);
    StringView path = rest.substring_view(host_end);

    if (authority.contains('@'))
        return Error::from_string_literal("URL: a user name in it");
    if (authority.is_empty())
        return Error::from_string_literal("URL: no host");

    StringView host = authority;
    url.port = url.is_https() ? 443 : 80;
    if (auto colon = authority.find(':'); colon.has_value()) {
        host = authority.substring_view(0, colon.value());
        StringView const port_text = authority.substring_view(colon.value() + 1);
        if (!port_text.is_empty()) {
            u32 port = 0;
            for (size_t i = 0; i < port_text.length(); ++i) {
                if (!is_ascii_digit(port_text[i]))
                    return Error::from_string_literal("URL: a bad port");
                port = port * 10 + static_cast<u32>(port_text[i] - '0');
                if (port > 65535)
                    return Error::from_string_literal("URL: a bad port");
            }
            if (port == 0)
                return Error::from_string_literal("URL: a bad port");
            url.port = static_cast<u16>(port);
        }
    }
    if (host.is_empty())
        return Error::from_string_literal("URL: no host");
    for (size_t i = 0; i < host.length(); ++i) {
        char const c = host[i];
        if (!is_ascii_alphanumeric(c) && c != '-' && c != '.' && c != '_')
            return Error::from_string_literal("URL: a bad character in the host");
    }
    url.host = lowered(host);

    if (auto fragment = path.find('#'); fragment.has_value())
        path = path.substring_view(0, fragment.value());
    // A request line can't have these in it.
    for (size_t i = 0; i < path.length(); ++i) {
        if (static_cast<u8>(path[i]) <= 0x20 || path[i] == 0x7f)
            return Error::from_string_literal("URL: a space or a control character in it");
    }
    if (path.is_empty()) {
        url.target = MUST(String::from_view("/"_sv));
    } else if (path[0] == '?') {
        StringBuilder builder;
        builder.append("/"_sv);
        builder.append(path);
        url.target = MUST(builder.to_string());
    } else {
        url.target = MUST(String::from_view(path));
    }
    return url;
}

ErrorOr<URL> URL::resolve(StringView location) const
{
    if (location.contains("://"_sv))
        return parse(location);
    URL url;
    url.scheme = scheme;
    url.host = host;
    url.port = port;
    // Protocol-relative: same scheme, another host.
    if (location.starts_with("//"_sv)) {
        StringBuilder builder;
        builder.append(scheme.view());
        builder.append(":"_sv);
        builder.append(location);
        return parse(builder.view());
    }
    if (location.is_empty()) {
        url.target = target;
        return url;
    }
    StringBuilder path;
    if (location[0] == '/') {
        path.append(location);
    } else if (location[0] == '?') {
        StringView const current = target.view();
        auto const query = current.find('?');
        path.append(query.has_value() ? current.substring_view(0, query.value()) : current);
        path.append(location);
    } else {
        // Beside what is here: up to the last slash of the path.
        StringView current = target.view();
        if (auto query = current.find('?'); query.has_value())
            current = current.substring_view(0, query.value());
        auto const slash = current.find_last('/');
        path.append(slash.has_value() ? current.substring_view(0, slash.value() + 1) : "/"_sv);
        path.append(location);
    }
    StringView const whole = path.view();
    for (size_t i = 0; i < whole.length(); ++i) {
        if (static_cast<u8>(whole[i]) <= 0x20 || whole[i] == 0x7f)
            return Error::from_string_literal("URL: a space or a control character in it");
    }
    auto fragment = whole.find('#');
    url.target = MUST(String::from_view(fragment.has_value() ? whole.substring_view(0, fragment.value()) : whole));
    return url;
}

String URL::host_header() const
{
    if (port == (is_https() ? 443 : 80))
        return host;
    StringBuilder builder;
    builder.appendff("{}:{}", host.view(), port);
    return MUST(builder.to_string());
}

String URL::to_string() const
{
    StringBuilder builder;
    builder.appendff("{}://{}{}", scheme.view(), host_header().view(), target.view());
    return MUST(builder.to_string());
}

String URL::file_name() const
{
    StringView path = target.view();
    if (auto query = path.find('?'); query.has_value())
        path = path.substring_view(0, query.value());
    auto const slash = path.find_last('/');
    return MUST(String::from_view(slash.has_value() ? path.substring_view(slash.value() + 1) : path));
}

}
