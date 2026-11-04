/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <LibJSON/PropertyList.h>
#include <pwd.h>

// defaults read [domain [key]] | read-type domain key | write domain key [-string|-int|-float|-bool|-array|-dict] value... |
// delete domain [key] | domains: a person's settings, each application's a property list in ~/Library/Preferences.

namespace {

char g_preferences[512];

void find_preferences()
{
    char const* home = getenv("HOME");
    if (!home) {
        auto const* user = getpwuid(getuid());
        home = user ? user->pw_dir : "/";
    }
    snprintf(g_preferences, sizeof(g_preferences), "%s/Library/Preferences", home);
}

void path_of(char const* domain, char* path, size_t size)
{
    // "-g" and its longer names are everyone's.
    char const* name = strcmp(domain, "-g") == 0 || strcmp(domain, "-globalDomain") == 0 || strcmp(domain, "NSGlobalDomain") == 0 ? ".GlobalPreferences" : domain;
    // A path is taken as it is.
    if (strchr(name, '/')) {
        size_t const length = strlen(name);
        snprintf(path, size, "%s%s", name, length > 6 && strcmp(name + length - 6, ".plist") == 0 ? "" : ".plist");
        return;
    }
    snprintf(path, size, "%s/%s.plist", g_preferences, name);
}

ErrorOr<JSON::Value> read_domain(char const* domain)
{
    char path[1024];
    path_of(domain, path, sizeof(path));
    auto bytes = TRY(read_whole_file(path));
    return JSON::parse_property_list(bytes.span());
}

bool write_domain(char const* domain, JSON::Value const& value)
{
    char path[1024];
    path_of(domain, path, sizeof(path));
    make_parents(path);
    auto const text = MUST(JSON::serialize_property_list_xml(value));
    return write_whole_file(path, text.characters(), text.view().length(), 0600);
}

// The name of the domain, for saying it doesn't exist.
char const* said_domain(char const* domain)
{
    return strcmp(domain, "-g") == 0 || strcmp(domain, "-globalDomain") == 0 ? "NSGlobalDomain" : domain;
}

int usage()
{
    fprintf(stderr, "usage: defaults read [domain [key]]\n"
                    "       defaults read-type domain key\n"
                    "       defaults write domain key [-string|-int|-float|-bool|-array|-dict] value ...\n"
                    "       defaults delete domain [key]\n"
                    "       defaults domains\n");
    return 1;
}

JSON::Value guess_value(char const* text)
{
    return MUST(JSON::Value::string(StringView(text)));
}

bool is_truthy(char const* text)
{
    return strcasecmp(text, "yes") == 0 || strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0;
}

char const* type_name(JSON::Value const& value)
{
    if (value.is_object())
        return "dictionary";
    if (value.is_array())
        return "array";
    if (value.is_string())
        return "string";
    if (value.is_boolean())
        return "boolean";
    if (value.is_number())
        return value.is_integer() ? "integer" : "float";
    return "string";
}

}

int main(int argc, char** argv)
{
    find_preferences();
    if (argc < 2)
        return usage();
    char const* command = argv[1];

    if (strcmp(command, "domains") == 0) {
        DIR* directory = opendir(g_preferences);
        bool first = true;
        if (directory) {
            while (auto* entry = readdir(directory)) {
                size_t const length = strlen(entry->d_name);
                if (length <= 6 || strcmp(entry->d_name + length - 6, ".plist") != 0 || entry->d_name[0] == '.')
                    continue;
                printf("%s%.*s", first ? "" : ", ", static_cast<int>(length - 6), entry->d_name);
                first = false;
            }
            closedir(directory);
        }
        putchar('\n');
        return 0;
    }
    if (strcmp(command, "read") == 0) {
        if (argc == 2) {
            // Everything: { domain = { ... }; }.
            auto everything = JSON::Value::object();
            DIR* directory = opendir(g_preferences);
            if (directory) {
                while (auto* entry = readdir(directory)) {
                    size_t const length = strlen(entry->d_name);
                    if (length <= 6 || strcmp(entry->d_name + length - 6, ".plist") != 0)
                        continue;
                    char domain[256];
                    snprintf(domain, sizeof(domain), "%.*s", static_cast<int>(length - 6), entry->d_name);
                    auto value = read_domain(domain);
                    if (!value.is_error())
                        MUST(everything.set(StringView(domain), value.release_value()));
                }
                closedir(directory);
            }
            puts(MUST(JSON::describe_property_list(everything)).characters());
            return 0;
        }
        auto value = read_domain(argv[2]);
        if (value.is_error()) {
            fprintf(stderr, "\nDomain %s does not exist\n", said_domain(argv[2]));
            return 1;
        }
        if (argc == 3) {
            puts(MUST(JSON::describe_property_list(value.value())).characters());
            return 0;
        }
        auto const* member = value.value().get(StringView(argv[3]));
        if (!member) {
            fprintf(stderr, "\nThe domain/default pair of (%s, %s) does not exist\n", said_domain(argv[2]), argv[3]);
            return 1;
        }
        if (member->is_string()) {
            // Plain, on its own.
            puts(MUST(String::from_view(member->as_string())).characters());
        } else {
            puts(MUST(JSON::describe_property_list(*member)).characters());
        }
        return 0;
    }
    if (strcmp(command, "read-type") == 0) {
        if (argc != 4)
            return usage();
        auto value = read_domain(argv[2]);
        auto const* member = value.is_error() ? nullptr : value.value().get(StringView(argv[3]));
        if (!member) {
            fprintf(stderr, "\nThe domain/default pair of (%s, %s) does not exist\n", said_domain(argv[2]), argv[3]);
            return 1;
        }
        printf("Type is %s\n", type_name(*member));
        return 0;
    }
    if (strcmp(command, "write") == 0) {
        if (argc < 4)
            return usage();
        auto existing = read_domain(argv[2]);
        auto domain = existing.is_error() ? JSON::Value::object() : existing.release_value();
        // A whole dictionary, as text, isn't here: only a key and its value.
        if (argc < 5)
            return usage();
        char const* key = argv[3];
        char const* type = argv[4];
        JSON::Value value;
        if (type[0] != '-') {
            value = guess_value(type);
        } else if (argc < 6 && strcmp(type, "-array") != 0 && strcmp(type, "-dict") != 0) {
            return usage();
        } else if (strcmp(type, "-string") == 0) {
            value = MUST(JSON::Value::string(StringView(argv[5])));
        } else if (strcmp(type, "-int") == 0 || strcmp(type, "-integer") == 0) {
            value = JSON::Value(static_cast<i64>(strtoll(argv[5], nullptr, 10)));
        } else if (strcmp(type, "-float") == 0) {
            value = JSON::Value(strtod(argv[5], nullptr));
        } else if (strcmp(type, "-bool") == 0 || strcmp(type, "-boolean") == 0) {
            value = JSON::Value(is_truthy(argv[5]));
        } else if (strcmp(type, "-array") == 0) {
            value = JSON::Value::array();
            for (int i = 5; i < argc; ++i)
                MUST(value.append(guess_value(argv[i])));
        } else if (strcmp(type, "-dict") == 0) {
            value = JSON::Value::object();
            for (int i = 5; i + 1 < argc; i += 2)
                MUST(value.set(StringView(argv[i]), guess_value(argv[i + 1])));
        } else {
            return usage();
        }
        MUST(domain.set(StringView(key), move(value)));
        if (!write_domain(argv[2], domain)) {
            fprintf(stderr, "Could not write domain %s; exiting\n", said_domain(argv[2]));
            return 1;
        }
        return 0;
    }
    if (strcmp(command, "delete") == 0) {
        if (argc < 3)
            return usage();
        char path[1024];
        path_of(argv[2], path, sizeof(path));
        if (argc == 3) {
            if (unlink(path) != 0) {
                fprintf(stderr, "\nDomain (%s) not found.\nDefaults have not been changed.\n", said_domain(argv[2]));
                return 1;
            }
            return 0;
        }
        auto value = read_domain(argv[2]);
        if (value.is_error() || !value.value().has(StringView(argv[3]))) {
            fprintf(stderr, "\nDomain (%s) not found.\nDefaults have not been changed.\n", said_domain(argv[2]));
            return 1;
        }
        auto domain = value.release_value();
        (void)domain.remove(StringView(argv[3]));
        if (!write_domain(argv[2], domain)) {
            fprintf(stderr, "Could not write domain %s; exiting\n", said_domain(argv[2]));
            return 1;
        }
        return 0;
    }
    return usage();
}
