/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TreeHelpers.h"
#include <LibJSON/PropertyList.h>

// plutil -lint | -p | -convert xml1|json|binary1 [-r] [-o out] | -extract keypath raw|json | -insert|-replace keypath -type value |
// -remove keypath, then file...

namespace {

int usage()
{
    fprintf(stderr, "usage: plutil [-lint] [-p] [-convert xml1|json|binary1 [-r] [-o path]] [-extract keypath raw|json [-o path]]\n"
                    "              [-insert|-replace keypath -string|-integer|-bool|-float value] [-remove keypath] file ...\n");
    return 1;
}

ErrorOr<JSON::Value> read_plist(char const* path)
{
    auto bytes = TRY(read_whole_file(path));
    return JSON::parse_property_list(bytes.span());
}

bool write_out(char const* path, StringView text)
{
    if (strcmp(path, "-") == 0) {
        fwrite(text.characters_without_null_termination(), 1, text.length(), stdout);
        return true;
    }
    return write_whole_file(path, text.characters_without_null_termination(), text.length(), 0644);
}

// Walks "a.b.c", and returns the parent of the last part with `last` set to it.
JSON::Value const* walk(JSON::Value const& root, StringView keypath)
{
    JSON::Value const* at = &root;
    while (!keypath.is_empty() && at) {
        auto const dot = keypath.find('.');
        auto const part = dot.has_value() ? keypath.substring_view(0, dot.value()) : keypath;
        keypath = dot.has_value() ? keypath.substring_view(dot.value() + 1) : StringView {};
        if (at->is_object()) {
            at = at->get(part);
        } else if (at->is_array()) {
            auto const index = part.to_number<size_t>();
            at = index.has_value() && index.value() < at->size() ? &at->at(index.value()) : nullptr;
        } else {
            at = nullptr;
        }
    }
    return at;
}

// A dictionary with this change made, at the end of this path.
ErrorOr<JSON::Value> changed(JSON::Value const& at, StringView keypath, JSON::Value const* new_value, bool& found)
{
    auto const dot = keypath.find('.');
    auto const part = dot.has_value() ? keypath.substring_view(0, dot.value()) : keypath;
    auto const rest = dot.has_value() ? keypath.substring_view(dot.value() + 1) : StringView {};
    if (!at.is_object())
        return Error::from_string_literal("a key path has to go through dictionaries");
    auto result = JSON::Value::object();
    bool seen = false;
    for (size_t i = 0; i < at.size(); ++i) {
        auto const key = at.key_at(i);
        if (key != part) {
            TRY(result.set(key, at.at(i)));
            continue;
        }
        seen = true;
        if (!rest.is_empty()) {
            TRY(result.set(key, TRY(changed(at.at(i), rest, new_value, found))));
            continue;
        }
        found = true;
        if (new_value)
            TRY(result.set(key, *new_value));
    }
    if (!seen && new_value) {
        if (!rest.is_empty()) {
            TRY(result.set(part, TRY(changed(JSON::Value::object(), rest, new_value, found))));
        } else {
            TRY(result.set(part, *new_value));
        }
    }
    return result;
}

}

int main(int argc, char** argv)
{
    enum class Action {
        Lint,
        Print,
        Convert,
        Extract,
        Insert,
        Replace,
        Remove,
    };
    Action action = Action::Lint;
    bool silent = false, readable = false;
    char const* format = nullptr;
    char const* output = nullptr;
    char const* keypath = nullptr;
    char const* value_type = nullptr;
    char const* value_text = nullptr;
    Vector<char const*> files;
    for (int i = 1; i < argc; ++i) {
        char const* argument = argv[i];
        auto next = [&]() -> char const* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (strcmp(argument, "-lint") == 0) {
            action = Action::Lint;
        } else if (strcmp(argument, "-p") == 0) {
            action = Action::Print;
        } else if (strcmp(argument, "-convert") == 0) {
            action = Action::Convert;
            format = next();
        } else if (strcmp(argument, "-extract") == 0) {
            action = Action::Extract;
            keypath = next();
            format = next();
        } else if (strcmp(argument, "-insert") == 0 || strcmp(argument, "-replace") == 0) {
            action = strcmp(argument, "-insert") == 0 ? Action::Insert : Action::Replace;
            keypath = next();
            value_type = next();
            value_text = next();
        } else if (strcmp(argument, "-remove") == 0) {
            action = Action::Remove;
            keypath = next();
        } else if (strcmp(argument, "-o") == 0) {
            output = next();
        } else if (strcmp(argument, "-r") == 0) {
            readable = true;
        } else if (strcmp(argument, "-s") == 0) {
            silent = true;
        } else if (strcmp(argument, "--") == 0) {
            continue;
        } else if (argument[0] == '-' && argument[1]) {
            return usage();
        } else {
            files.append(argument);
        }
    }
    if (files.is_empty())
        return usage();
    int status = 0;
    for (auto const* path : files) {
        auto plist = read_plist(path);
        if (plist.is_error()) {
            // As a Mac says it, which is about the first thing it couldn't make anything of.
            char const* what = "the start of it";
            char first[2] = {};
            if (auto contents = read_whole_file(path); !contents.is_error() && contents.value().size() > 0) {
                first[0] = static_cast<char>(contents.value()[0]);
                what = first;
            }
            fprintf(stderr, "%s: (Unexpected character %s at line 1)\n", path, what);
            status = 1;
            continue;
        }
        auto const& value = plist.value();
        switch (action) {
        case Action::Lint:
            if (!silent)
                printf("%s: OK\n", path);
            break;
        case Action::Print:
            puts(MUST(JSON::print_property_list_for_people(value)).characters());
            break;
        case Action::Convert: {
            if (!format) {
                status = 1;
                break;
            }
            String text;
            if (strcmp(format, "binary1") == 0) {
                auto bytes = MUST(JSON::serialize_property_list_binary(value));
                char const* where = output ? output : path;
                int const fd = strcmp(where, "-") == 0 ? 1 : open(where, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                size_t done = 0;
                while (fd >= 0 && done < bytes.size()) {
                    ssize_t const wrote = write(fd, bytes.data() + done, bytes.size() - done);
                    if (wrote <= 0)
                        break;
                    done += static_cast<size_t>(wrote);
                }
                if (done != bytes.size()) {
                    warn("plutil", where);
                    status = 1;
                }
                if (fd > 1)
                    close(fd);
                break;
            }
            if (strcmp(format, "xml1") == 0) {
                text = MUST(JSON::serialize_property_list_xml(value));
            } else if (strcmp(format, "json") == 0) {
                text = MUST(value.serialize(readable));
            } else {
                fprintf(stderr, "plutil: this can't convert to %s\n", format);
                status = 1;
                break;
            }
            if (!write_out(output ? output : path, text.view())) {
                warn("plutil", output ? output : path);
                status = 1;
            }
            break;
        }
        case Action::Extract: {
            auto const* found = keypath ? walk(value, StringView(keypath)) : nullptr;
            if (!found) {
                fprintf(stderr, "%s: Could not extract value, error: No value at that key path\n", path);
                status = 1;
                break;
            }
            String text;
            if (format && strcmp(format, "raw") == 0) {
                if (found->is_string()) {
                    text = MUST(String::from_view(found->as_string()));
                } else if (found->is_boolean()) {
                    text = MUST(String::from_view(found->as_boolean() ? "true"_sv : "false"_sv));
                } else if (found->is_number()) {
                    text = MUST(found->serialize());
                } else {
                    fprintf(stderr, "%s: Could not extract value, error: this is a collection\n", path);
                    status = 1;
                    break;
                }
            } else if (format && strcmp(format, "json") == 0) {
                text = MUST(found->serialize());
            } else {
                text = MUST(JSON::serialize_property_list_xml(*found));
            }
            StringBuilder with_newline;
            with_newline.append(text.view());
            if (format && strcmp(format, "raw") == 0)
                with_newline.append('\n');
            (void)write_out(output ? output : "-", with_newline.view());
            break;
        }
        case Action::Insert:
        case Action::Replace:
        case Action::Remove: {
            if (!keypath) {
                status = 1;
                break;
            }
            JSON::Value new_value;
            JSON::Value const* new_value_pointer = nullptr;
            if (action != Action::Remove) {
                if (!value_type || !value_text)
                    return usage();
                if (strcmp(value_type, "-string") == 0) {
                    new_value = MUST(JSON::Value::string(StringView(value_text)));
                } else if (strcmp(value_type, "-integer") == 0) {
                    new_value = JSON::Value(static_cast<i64>(strtoll(value_text, nullptr, 10)));
                } else if (strcmp(value_type, "-float") == 0) {
                    new_value = JSON::Value(strtod(value_text, nullptr));
                } else if (strcmp(value_type, "-bool") == 0) {
                    new_value = JSON::Value(strcmp(value_text, "YES") == 0 || strcmp(value_text, "true") == 0 || strcmp(value_text, "1") == 0);
                } else {
                    return usage();
                }
                new_value_pointer = &new_value;
            }
            if (action == Action::Insert && walk(value, StringView(keypath))) {
                fprintf(stderr, "%s: Could not modify plist, error: Value already exists at that key path\n", path);
                status = 1;
                break;
            }
            bool found = false;
            auto result = changed(value, StringView(keypath), new_value_pointer, found);
            if (result.is_error() || (action == Action::Remove && !found)) {
                fprintf(stderr, "%s: Could not modify plist, error: No value to remove at that key path\n", path);
                status = 1;
                break;
            }
            auto const text = MUST(JSON::serialize_property_list_xml(result.value()));
            if (!write_out(output ? output : path, text.view())) {
                warn("plutil", output ? output : path);
                status = 1;
            }
            break;
        }
        }
    }
    return status;
}
