/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <LibJSON/JSON.h>
#include <SearchEndpoint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// mdls [-name attribute]... [-raw] file...: what Search knows of a file, set out as a Mac's mdls sets it out.

namespace {

bool is_date(StringView attribute)
{
    return attribute.ends_with("Date"_sv);
}

void print_value(JSON::Value const* value, StringView attribute, bool raw)
{
    if (!value) {
        printf("(null)");
        return;
    }
    if (value->is_string()) {
        auto text = value->as_string();
        if (raw)
            printf("%.*s", static_cast<int>(text.length()), text.characters_without_null_termination());
        else
            printf("\"%.*s\"", static_cast<int>(text.length()), text.characters_without_null_termination());
        return;
    }
    auto number = value->as_number();
    if (is_date(attribute)) {
        time_t when = static_cast<time_t>(number);
        tm parts;
        gmtime_r(&when, &parts);
        printf("%04d-%02d-%02d %02d:%02d:%02d +0000", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
        return;
    }
    printf("%lld", static_cast<long long>(number));
}

}

int main(int argc, char** argv)
{
    Vector<StringView> wanted;
    bool raw = false;
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        if (strcmp(argv[first], "-name") == 0 && first + 1 < argc) {
            wanted.append(StringView(argv[first + 1], strlen(argv[first + 1])));
            first += 2;
        } else if (strcmp(argv[first], "-raw") == 0) {
            raw = true;
            ++first;
        } else {
            fprintf(stderr, "usage: mdls [-name attribute]... [-raw] file...\n");
            return 64;
        }
    }
    if (first >= argc) {
        fprintf(stderr, "usage: mdls [-name attribute]... [-raw] file...\n");
        return 64;
    }
    auto port = IPC::look_up("com.vulture.indexd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "mdls: Search isn't running\n");
        return 1;
    }
    SearchProxy service(port.release_value());
    int status = 0;
    for (int i = first; i < argc; ++i) {
        char resolved[PATH_MAX];
        if (!realpath(argv[i], resolved)) {
            fprintf(stderr, "%s: could not find %s.\n", argv[0], argv[i]);
            status = 1;
            continue;
        }
        auto answer = service.attributes(StringView(resolved, strlen(resolved)));
        if (answer.is_error())
            return 1;
        auto json = JSON::parse(answer.value().view());
        if (json.is_error())
            return 1;
        if (!json.value().is_object()) {
            fprintf(stderr, "%s: could not find %s.\n", argv[0], argv[i]);
            status = 1;
            continue;
        }
        if (!wanted.is_empty()) {
            for (auto attribute : wanted) {
                auto const* value = json.value().get(attribute);
                if (raw) {
                    print_value(value, attribute, true);
                } else {
                    printf("%.*s = ", static_cast<int>(attribute.length()), attribute.characters_without_null_termination());
                    print_value(value, attribute, false);
                    printf("\n");
                }
            }
            continue;
        }
        Vector<String> names;
        for (size_t member = 0; member < json.value().size(); ++member)
            names.append(MUST(String::from_view(json.value().key_at(member))));
        for (size_t a = 1; a < names.size(); ++a) {
            for (size_t b = a; b > 0 && names[b].view().compare(names[b - 1].view()) < 0; --b) {
                auto moved = move(names[b]);
                names[b] = move(names[b - 1]);
                names[b - 1] = move(moved);
            }
        }
        size_t widest = 0;
        for (auto const& name : names)
            widest = max(widest, name.length());
        for (auto const& name : names) {
            printf("%-*s = ", static_cast<int>(widest), name.characters());
            print_value(json.value().get(name.view()), name.view(), raw);
            printf("\n");
        }
    }
    return status;
}
