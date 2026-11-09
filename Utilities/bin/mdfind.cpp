/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibIPC/Bootstrap.h>
#include <LibJSON/JSON.h>
#include <SearchEndpoint.h>
#include <stdio.h>
#include <string.h>

// mdfind [-onlyin directory] [-name name] [-count] [-0] query: what Search knows of, as a Mac's mdfind says.

int main(int argc, char** argv)
{
    StringView only_in;
    StringBuilder words;
    bool name_only = false;
    bool count_only = false;
    bool zero = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-onlyin") == 0 && i + 1 < argc) {
            ++i;
            only_in = StringView(argv[i], strlen(argv[i]));
        } else if (strcmp(argv[i], "-name") == 0) {
            name_only = true;
        } else if (strcmp(argv[i], "-count") == 0) {
            count_only = true;
        } else if (strcmp(argv[i], "-0") == 0) {
            zero = true;
        } else {
            if (words.length())
                words.append(' ');
            words.append(StringView(argv[i], strlen(argv[i])));
        }
    }
    if (words.length() == 0) {
        fprintf(stderr, "usage: mdfind [-onlyin directory] [-name name] [-count] [-0] query\n");
        return 64;
    }
    auto port = IPC::look_up("com.vulture.indexd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "mdfind: Search isn't running\n");
        return 1;
    }
    SearchProxy service(port.release_value());
    auto answer = service.find(words.view(), only_in, name_only, 1u << 24);
    if (answer.is_error()) {
        fprintf(stderr, "mdfind: Search didn't answer\n");
        return 1;
    }
    if (!answer.value().error.is_empty()) {
        fprintf(stderr, "mdfind: %s\n", answer.value().error.characters());
        return 1;
    }
    auto json = JSON::parse(answer.value().json.view());
    if (json.is_error())
        return 1;
    if (count_only) {
        printf("%zu\n", json.value().size());
        return 0;
    }
    for (size_t i = 0; i < json.value().size(); ++i) {
        auto path = json.value().at(i).as_string();
        fwrite(path.characters_without_null_termination(), 1, path.length(), stdout);
        fputc(zero ? 0 : '\n', stdout);
    }
    return 0;
}
