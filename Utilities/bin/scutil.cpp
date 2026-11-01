/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ConfigurationEndpoint.h>
#include <LibIPC/Bootstrap.h>
#include <stdio.h>
#include <string.h>

// scutil: the machine's names, from configd.
//   scutil --get ComputerName | LocalHostName | HostName
//   scutil --set ComputerName | LocalHostName | HostName [newval]
// --set without a value reads one line from standard input; an empty
// HostName removes it.

namespace {

void usage()
{
    fprintf(stderr,
        "usage: scutil --get pref\n"
        "   or: scutil --set pref [newval]\n"
        "\tpref\tdisplay (or set) the specified preference.  Valid preferences\n"
        "\t\tinclude:\n"
        "\t\t\tComputerName, LocalHostName, HostName\n"
        "\tnewval\tNew preference value to be set.  If not specified,\n"
        "\t\tthe new value will be read from standard input.\n");
}

bool is_preference(char const* name)
{
    return strcmp(name, "ComputerName") == 0 || strcmp(name, "LocalHostName") == 0 || strcmp(name, "HostName") == 0;
}

}

int main(int argc, char** argv)
{
    bool get = argc == 3 && strcmp(argv[1], "--get") == 0;
    bool set = (argc == 3 || argc == 4) && strcmp(argv[1], "--set") == 0;
    if ((!get && !set) || !is_preference(argv[2])) {
        usage();
        return 1;
    }
    char const* preference = argv[2];
    auto port = IPC::look_up("com.vulture.SystemConfiguration.configd"_sv);
    if (port.is_error()) {
        fprintf(stderr, "scutil: could not connect to configd: %s\n", strerror(port.error().code()));
        return 1;
    }
    ConfigurationProxy configd(port.release_value());

    if (get) {
        auto answer = configd.get(StringView(preference));
        if (answer.is_error()) {
            fprintf(stderr, "scutil: %s\n", strerror(answer.error().code()));
            return 1;
        }
        if (!answer.value().found) {
            printf("%s: not set\n", preference);
            return 1;
        }
        printf("%s\n", answer.value().value.characters());
        return 0;
    }

    char line[256] = {};
    char const* value = argc == 4 ? argv[3] : nullptr;
    if (!value) {
        if (fgets(line, sizeof(line), stdin))
            line[strcspn(line, "\n")] = '\0';
        value = line;
    }
    auto result = *value == '\0' && strcmp(preference, "HostName") == 0 ? configd.remove(StringView(preference))
                                                                         : configd.set(StringView(preference), StringView(value));
    int error = result.is_error() ? result.error().code() : result.value();
    if (error) {
        if (error == EINVAL)
            fprintf(stderr, "scutil: \"%s\" is not a valid %s\n", value, preference);
        else
            fprintf(stderr, "scutil: could not set %s: %s\n", preference, strerror(error));
        return 1;
    }
    return 0;
}
