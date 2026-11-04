/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AccessList.h"
#include "Common.h"
#include <dirent.h>
#include <sys/stat.h>

// chmod [-Rv] mode file...: octal (755) or symbolic (u+x,go-w,a=r,+X); and the access control list, as on a Mac:
// +a "who allow|deny what", +a# N "..." to put it at that place, -a "..." to take that away, -a# N, =a# N "...", -N for none.
static bool s_recursive = false, s_verbose = false;

static bool apply_symbolic(char const* spec, mode_t& mode, bool is_directory)
{
    mode_t mask = umask(0);
    umask(mask);
    char const* cursor = spec;
    while (*cursor) {
        mode_t who = 0;
        for (; *cursor && strchr("ugoa", *cursor); ++cursor) {
            if (*cursor == 'u')
                who |= 04700;
            else if (*cursor == 'g')
                who |= 02070;
            else if (*cursor == 'o')
                who |= 01007;
            else
                who |= 07777;
        }
        bool implicit = who == 0;
        if (implicit)
            who = 07777;
        while (*cursor == '+' || *cursor == '-' || *cursor == '=') {
            char op = *cursor++;
            mode_t bits = 0;
            for (; *cursor && strchr("rwxXst", *cursor); ++cursor) {
                switch (*cursor) {
                case 'r':
                    bits |= 0444;
                    break;
                case 'w':
                    bits |= 0222;
                    break;
                case 'x':
                    bits |= 0111;
                    break;
                case 'X':
                    if (is_directory || (mode & 0111))
                        bits |= 0111;
                    break;
                case 's':
                    bits |= 06000;
                    break;
                case 't':
                    bits |= 01000;
                    break;
                }
            }
            bits &= who;
            if (implicit)
                bits &= ~mask;
            if (op == '+')
                mode |= bits;
            else if (op == '-')
                mode &= ~bits;
            else
                mode = (mode & ~(who & 07777)) | bits;
        }
        if (*cursor == ',')
            ++cursor;
        else if (*cursor)
            return false;
    }
    return true;
}

static bool change(char const* path, char const* spec)
{
    struct stat information;
    if (stat(path, &information) < 0) {
        warn("chmod", path);
        return false;
    }
    mode_t mode = information.st_mode & 07777;
    char* end;
    long octal = strtol(spec, &end, 8);
    if (*end == '\0' && end != spec) {
        mode = static_cast<mode_t>(octal);
    } else if (!apply_symbolic(spec, mode, S_ISDIR(information.st_mode))) {
        fprintf(stderr, "chmod: Invalid file mode: %s\n", spec);
        exit(1);
    }
    bool ok = true;
    if (chmod(path, mode) < 0) {
        warn("chmod", path);
        ok = false;
    } else if (s_verbose) {
        printf("%s\n", path);
    }
    if (s_recursive && S_ISDIR(information.st_mode)) {
        DIR* directory = opendir(path);
        if (directory) {
            while (struct dirent* entry = readdir(directory)) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                    continue;
                char* child = path_join(path, entry->d_name);
                ok = change(child, spec) && ok;
                free(child);
            }
            closedir(directory);
        }
    }
    return ok;
}

static bool parse_access_line(char const* text, vulture_acl_entry& entry)
{
    entry = {};
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", text);
    char* context = nullptr;
    char* who = strtok_r(copy, " ", &context);
    char* kind = strtok_r(nullptr, " ", &context);
    char* what = strtok_r(nullptr, " ", &context);
    if (who && kind && strcmp(kind, "inherited") == 0) {
        entry.inheritance |= VULTURE_ACL_INHERITED;
        kind = what;
        what = strtok_r(nullptr, " ", &context);
    }
    if (!who) {
        fprintf(stderr, "chmod: Unable to translate '' to a UUID\n");
        return false;
    }
    // By its kind, if it says, or else whichever there is by that name.
    char const* name = who;
    bool const says_user = strncmp(who, "user:", 5) == 0;
    bool const says_group = strncmp(who, "group:", 6) == 0;
    if (says_user)
        name += 5;
    else if (says_group)
        name += 6;
    struct passwd* user = says_group ? nullptr : getpwnam(name);
    struct group* group = says_user || user ? nullptr : getgrnam(name);
    if (strcmp(name, "everyone") == 0) {
        entry.tag = VULTURE_ACL_EVERYONE;
    } else if (user) {
        entry.tag = VULTURE_ACL_USER;
        entry.id = user->pw_uid;
    } else if (group) {
        entry.tag = VULTURE_ACL_GROUP;
        entry.id = group->gr_gid;
    } else {
        fprintf(stderr, "chmod: Unable to translate '%s' to a UUID\n", name);
        return false;
    }
    if (!kind || (strcmp(kind, "allow") != 0 && strcmp(kind, "deny") != 0) || !what) {
        fprintf(stderr, "chmod: Invalid ACL: %s\n", text);
        return false;
    }
    entry.kind = strcmp(kind, "deny") == 0 ? VULTURE_ACL_DENY : VULTURE_ACL_ALLOW;
    char* inner = nullptr;
    for (char* word = strtok_r(what, ",", &inner); word; word = strtok_r(nullptr, ",", &inner)) {
        bool found = false;
        auto const look_in = [&](auto const& list, auto& into) {
            for (auto const& known : list) {
                if (strcmp(word, known.name) == 0) {
                    into |= known.value;
                    found = true;
                }
            }
        };
        look_in(AccessList::file_rights, entry.permissions);
        look_in(AccessList::directory_rights, entry.permissions);
        look_in(AccessList::common_rights, entry.permissions);
        look_in(AccessList::inheritance_words, entry.inheritance);
        if (!found) {
            fprintf(stderr, "chmod: Invalid ACL: %s\n", word);
            return false;
        }
    }
    return true;
}

static bool same_who(vulture_acl_entry const& a, vulture_acl_entry const& b)
{
    return a.kind == b.kind && a.tag == b.tag && a.id == b.id && (a.inheritance & VULTURE_ACL_INHERITED) == (b.inheritance & VULTURE_ACL_INHERITED);
}

// +a, +a#, -a, -a#, =a#, -N: on each file. `line` is what it's about, if anything; `index`, where, if it says.
static int change_access_list(char kind, int index, char const* line, char** files, int count)
{
    vulture_acl_entry given {};
    if (line && !parse_access_line(line, given))
        return 1;
    int status = 0;
    for (int i = 0; i < count; ++i) {
        vulture_acl_entry entries[VULTURE_ACL_MAX_ENTRIES];
        int have = 0;
        if (kind != 'N') {
            have = vulture_acl_get(files[i], entries, VULTURE_ACL_MAX_ENTRIES, 1);
            if (have < 0) {
                warn("chmod", files[i]);
                status = 1;
                continue;
            }
        }
        if (kind == '+') {
            bool merged = false;
            // Someone it already says the same about gets it added to what's there.
            if (index < 0) {
                for (int line_at = 0; line_at < have; ++line_at) {
                    if (same_who(entries[line_at], given)) {
                        entries[line_at].permissions |= given.permissions;
                        merged = true;
                    }
                }
            }
            if (!merged) {
                if (have >= VULTURE_ACL_MAX_ENTRIES) {
                    fprintf(stderr, "chmod: ACL too long: %s\n", files[i]);
                    status = 1;
                    continue;
                }
                int place = index;
                if (place < 0) {
                    // Where it goes: denying first, then allowing; each new one before others of its kind.
                    place = 0;
                    if (given.kind == VULTURE_ACL_ALLOW) {
                        while (place < have && entries[place].kind == VULTURE_ACL_DENY && !(entries[place].inheritance & VULTURE_ACL_INHERITED))
                            ++place;
                    }
                } else if (place > have) {
                    fprintf(stderr, "chmod: Invalid entry number: %d\n", index);
                    status = 1;
                    continue;
                }
                for (int moving = have; moving > place; --moving)
                    entries[moving] = entries[moving - 1];
                entries[place] = given;
                ++have;
            }
        } else if (kind == '=') {
            if (index >= have) {
                fprintf(stderr, "chmod: Invalid entry number: %d\n", index);
                status = 1;
                continue;
            }
            entries[index] = given;
        } else if (kind == '-') {
            int place = index;
            if (place >= have) {
                fprintf(stderr, "chmod: Invalid entry number: %d\n", index);
                status = 1;
                continue;
            }
            if (place < 0) {
                // Takes those rights from the line about that person.
                for (int line_at = 0; line_at < have; ++line_at) {
                    if (same_who(entries[line_at], given) && (entries[line_at].permissions & given.permissions)) {
                        entries[line_at].permissions &= ~given.permissions;
                        if (entries[line_at].permissions == 0)
                            place = line_at;
                        else
                            place = -2;
                        break;
                    }
                }
                if (place == -1) {
                    fprintf(stderr, "chmod: No matching ACE found: %s\n", line);
                    status = 1;
                    continue;
                }
            }
            if (place >= 0) {
                for (int moving = place; moving + 1 < have; ++moving)
                    entries[moving] = entries[moving + 1];
                --have;
            }
        }
        if (vulture_acl_set(files[i], entries, static_cast<unsigned>(have), 1) != 0) {
            warn("chmod", files[i]);
            status = 1;
        }
    }
    return status;
}

int main(int argc, char** argv)
{
    int first = 1;
    // These can come first, or after the others.
    for (int at = 1; at < argc && argv[at][0] == '-' && (strchr("Rv", argv[at][1]) || at == 1); ++at) {
        char const* const word = argv[at];
        if (strcmp(word, "-N") == 0 && argc - at >= 2)
            return change_access_list('N', -1, nullptr, argv + at + 1, argc - at - 1);
        if (word[1] == 'R' || word[1] == 'v')
            continue;
        break;
    }
    for (int at = 1; at < argc; ++at) {
        char const* const word = argv[at];
        if (word[0] != '+' && word[0] != '-' && word[0] != '=')
            break;
        if (word[1] == 'a' && word[2] == '#' && word[3] == 0 && at + 1 < argc) {
            int const index = atoi(argv[at + 1]);
            if (word[0] == '-') {
                if (argc - at - 2 < 1)
                    break;
                return change_access_list('-', index, nullptr, argv + at + 2, argc - at - 2);
            }
            if (argc - at - 3 < 1)
                break;
            return change_access_list(word[0], index, argv[at + 2], argv + at + 3, argc - at - 3);
        }
        if (word[1] == 'a' && word[2] == 0 && (word[0] == '+' || word[0] == '-')) {
            if (argc - at - 2 < 1)
                break;
            return change_access_list(word[0], -1, argv[at + 1], argv + at + 2, argc - at - 2);
        }
        if (word[0] != '-' || !strchr("Rv", word[1]))
            break;
    }
    for (; first < argc && argv[first][0] == '-' && argv[first][1] && strchr("Rv", argv[first][1]); ++first) {
        for (char const* option = argv[first] + 1; *option; ++option) {
            if (*option == 'R')
                s_recursive = true;
            else if (*option == 'v')
                s_verbose = true;
        }
    }
    if (argc - first < 2) {
        fprintf(stderr, "usage: chmod [-Rv] mode file ...\n");
        return 1;
    }
    int status = 0;
    for (int i = first + 1; i < argc; ++i) {
        if (!change(argv[i], argv[first]))
            status = 1;
    }
    return status;
}
