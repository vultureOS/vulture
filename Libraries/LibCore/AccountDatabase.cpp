/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibCore/Account.h>
#include <LibCore/AccountDatabase.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Core::AccountDatabase {

namespace {

constexpr char const* master_path = "/etc/master.passwd";
constexpr char const* passwd_path = "/etc/passwd";
constexpr char const* group_path = "/etc/group";
constexpr char const* lock_path = "/etc/ptmp";
constexpr char const* hints_path = "/private/var/db/hints";
constexpr char const* setup_marker_path = "/private/var/db/.VultureSetupDone";
// The system's own are below; nobody and the like, at the top.
constexpr uid_t last_person_uid = 59999;

ErrorOr<String> read_text(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    StringBuilder text;
    char buffer[4096];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            int error = errno;
            close(fd);
            return Error::from_errno(error);
        }
        if (got == 0)
            break;
        text.append(StringView(buffer, static_cast<size_t>(got)));
    }
    close(fd);
    return text.to_string();
}

// The new contents beside the old, then in their place in one step.
ErrorOr<void> write_text(char const* path, StringView text, mode_t mode)
{
    StringBuilder new_path;
    new_path.append(StringView(path, strlen(path)));
    new_path.append(".new"_sv);
    auto temporary = TRY(new_path.to_string());
    int fd = open(temporary.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return Error::from_errno(errno);
    (void)fchmod(fd, mode);
    size_t done = 0;
    while (done < text.length()) {
        ssize_t wrote = write(fd, text.characters_without_null_termination() + done, text.length() - done);
        if (wrote <= 0) {
            int error = wrote < 0 ? errno : EIO;
            close(fd);
            unlink(temporary.characters());
            return Error::from_errno(error);
        }
        done += static_cast<size_t>(wrote);
    }
    if (fsync(fd) < 0) {
        int error = errno;
        close(fd);
        unlink(temporary.characters());
        return Error::from_errno(error);
    }
    close(fd);
    if (rename(temporary.characters(), path) < 0) {
        int error = errno;
        unlink(temporary.characters());
        return Error::from_errno(error);
    }
    return {};
}

// A line's fields between colons, empty ones too. How many there were.
size_t fields_of(StringView line, StringView* fields, size_t capacity)
{
    size_t count = 0;
    line.for_each_split_view(':', SplitBehavior::KeepEmpty, [&](StringView field) {
        if (count < capacity)
            fields[count] = field;
        ++count;
    });
    return count;
}

// Each line of a file, without its newline; comments and blank lines too, so they're kept when it's written again.
template<typename Callback>
void for_each_line(StringView text, Callback callback)
{
    size_t start = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] != '\n')
            continue;
        callback(text.substring_view(start, i - start));
        start = i + 1;
    }
    if (start < text.length())
        callback(text.substring_view(start));
}

// Held while the files are changed: one change at a time, whoever's making it.
class Lock {
public:
    Lock()
    {
        for (int attempt = 0; attempt < 100; ++attempt) {
            m_fd = open(lock_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (m_fd >= 0 || errno != EEXIST)
                return;
            // Left behind by one that never finished: after a few seconds, it's nobody's.
            struct stat information;
            if (attempt == 99 && stat(lock_path, &information) == 0) {
                unlink(lock_path);
                m_fd = open(lock_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                return;
            }
            usleep(20000);
        }
    }
    ~Lock()
    {
        if (m_fd < 0)
            return;
        close(m_fd);
        unlink(lock_path);
    }
    [[nodiscard]] bool held() const { return m_fd >= 0; }

private:
    int m_fd { -1 };
};

bool is_person(uid_t uid) { return uid >= first_person_uid && uid <= last_person_uid; }

// The members of a group, as /etc/group lists them.
Vector<String> members_of(StringView group_text, StringView group_name)
{
    Vector<String> members;
    for_each_line(group_text, [&](StringView line) {
        StringView fields[4];
        if (fields_of(line, fields, 4) != 4 || fields[0] != group_name)
            return;
        fields[3].for_each_split_view(',', SplitBehavior::SkipEmpty, [&](StringView member) {
            members.append(MUST(String::from_view(member)));
        });
    });
    return members;
}

bool contains(Vector<String> const& names, StringView name)
{
    for (auto const& each : names) {
        if (each.view() == name)
            return true;
    }
    return false;
}

// /etc/group, with `name` in the group or out of it.
ErrorOr<String> group_with_member(StringView group_text, StringView group_name, StringView name, bool member)
{
    StringBuilder out;
    for_each_line(group_text, [&](StringView line) {
        StringView fields[4];
        if (fields_of(line, fields, 4) != 4 || fields[0] != group_name) {
            out.append(line);
            out.append('\n');
            return;
        }
        out.appendff("{}:{}:{}:", fields[0], fields[1], fields[2]);
        bool first = true;
        bool there = false;
        fields[3].for_each_split_view(',', SplitBehavior::SkipEmpty, [&](StringView each) {
            if (each == name) {
                there = true;
                if (!member)
                    return;
            }
            if (!first)
                out.append(',');
            out.append(each);
            first = false;
        });
        if (member && !there) {
            if (!first)
                out.append(',');
            out.append(name);
        }
        out.append('\n');
    });
    return out.to_string();
}

// A full name as a field can hold it: no colons, no new lines.
String clean_field(StringView text)
{
    StringBuilder out;
    for (char c : text) {
        if (c == ':' || c == '\n' || c == '\r')
            continue;
        out.append(c);
    }
    return MUST(out.to_string());
}

ErrorOr<void> remove_tree(String const& path)
{
    struct stat information;
    if (lstat(path.characters(), &information) < 0)
        return Error::from_errno(errno);
    if (!S_ISDIR(information.st_mode)) {
        if (unlink(path.characters()) < 0)
            return Error::from_errno(errno);
        return {};
    }
    DIR* directory = opendir(path.characters());
    if (!directory)
        return Error::from_errno(errno);
    Vector<String> children;
    while (auto* entry = readdir(directory)) {
        StringView name(entry->d_name, strlen(entry->d_name));
        if (name == "."_sv || name == ".."_sv)
            continue;
        StringBuilder child;
        child.append(path.view());
        child.append('/');
        child.append(name);
        children.append(MUST(child.to_string()));
    }
    closedir(directory);
    for (auto const& child : children)
        TRY(remove_tree(child));
    if (rmdir(path.characters()) < 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<String> hints_without(StringView name)
{
    auto text = read_text(hints_path);
    StringBuilder out;
    if (!text.is_error()) {
        for_each_line(text.value().view(), [&](StringView line) {
            auto tab = line.find('\t');
            if (!tab.has_value() || line.substring_view(0, tab.value()) == name)
                return;
            out.append(line);
            out.append('\n');
        });
    }
    return out.to_string();
}

}

Vector<Person> people()
{
    Vector<Person> found;
    auto passwd = read_text(passwd_path);
    if (passwd.is_error())
        return found;
    auto group = read_text(group_path);
    Vector<String> administrators = group.is_error() ? Vector<String> {} : members_of(group.value().view(), "admin"_sv);
    for_each_line(passwd.value().view(), [&](StringView line) {
        if (line.starts_with('#'))
            return;
        StringView fields[7];
        if (fields_of(line, fields, 7) != 7)
            return;
        auto uid = fields[2].to_number<u32>();
        if (!uid.has_value() || !is_person(uid.value()))
            return;
        Person person;
        person.name = MUST(String::from_view(fields[0]));
        StringView full = fields[4];
        if (auto comma = full.find(','); comma.has_value())
            full = full.substring_view(0, comma.value());
        person.full_name = MUST(String::from_view(full.is_empty() ? fields[0] : full));
        person.home = MUST(String::from_view(fields[5]));
        person.uid = uid.value();
        person.administrator = contains(administrators, fields[0]);
        found.append(move(person));
    });
    return found;
}

Optional<Person> person_named(StringView name)
{
    for (auto& person : people()) {
        if (person.name.view() == name)
            return move(person);
    }
    return {};
}

bool has_people()
{
    return !people().is_empty();
}

bool is_valid_name(StringView name)
{
    if (name.is_empty() || name.length() > 31 || name[0] < 'a' || name[0] > 'z')
        return false;
    for (char c : name) {
        bool const allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!allowed)
            return false;
    }
    return true;
}

String name_from_full_name(StringView full_name)
{
    StringBuilder name;
    for (char c : full_name) {
        if (name.length() >= 31)
            break;
        if (c >= 'A' && c <= 'Z')
            name.append(static_cast<char>(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9' && name.length() > 0))
            name.append(c);
    }
    return MUST(name.to_string());
}

ErrorOr<uid_t> create(NewAccount const& account)
{
    if (!is_valid_name(account.name) || account.password.contains('\n'))
        return Error::from_errno(EINVAL);
    Lock lock;
    if (!lock.held())
        return Error::from_errno(EBUSY);
    auto master = TRY(read_text(master_path));
    auto passwd = TRY(read_text(passwd_path));
    auto group = TRY(read_text(group_path));
    // The name, or one only differing in case, as the Mac's names are told apart without it, is taken; the next user ID.
    uid_t uid = first_person_uid;
    bool taken = false;
    for_each_line(master.view(), [&](StringView line) {
        StringView fields[10];
        if (fields_of(line, fields, 10) != 10)
            return;
        if (fields[0] == account.name)
            taken = true;
        auto each = fields[2].to_number<u32>();
        if (each.has_value() && is_person(each.value()) && each.value() >= uid)
            uid = each.value() + 1;
    });
    if (taken)
        return Error::from_errno(EEXIST);
    if (!is_person(uid))
        return Error::from_errno(ENOSPC);
    auto hash = account.password.is_empty() ? String {} : TRY(Account::hash_password(account.password));
    auto full_name = clean_field(account.full_name.is_empty() ? account.name : account.full_name);
    StringBuilder home;
    home.appendff("/Users/{}", account.name);

    StringBuilder new_master;
    new_master.append(master.view());
    if (!master.view().is_empty() && !master.view().ends_with('\n'))
        new_master.append('\n');
    new_master.appendff("{}:{}:{}:{}::0:0:{}:{}:/bin/vsh\n", account.name, hash.view(), uid, staff_gid, full_name.view(), home.view());
    StringBuilder new_passwd;
    new_passwd.append(passwd.view());
    if (!passwd.view().is_empty() && !passwd.view().ends_with('\n'))
        new_passwd.append('\n');
    new_passwd.appendff("{}:*:{}:{}:{}:{}:/bin/vsh\n", account.name, uid, staff_gid, full_name.view(), home.view());
    auto new_group = TRY(group_with_member(group.view(), "staff"_sv, account.name, true));
    if (account.administrator)
        new_group = TRY(group_with_member(new_group.view(), "admin"_sv, account.name, true));

    // The password first: an account in /etc/passwd with none in the master file can't log in, which is the safe way to
    // be half made.
    TRY(write_text(master_path, new_master.view(), 0600));
    TRY(write_text(passwd_path, new_passwd.view(), 0644));
    TRY(write_text(group_path, new_group.view(), 0644));
    if (!account.hint.is_empty())
        TRY(set_hint(account.name, account.hint));
    auto made = TRY(Account::from_name(account.name));
    TRY(made.create_home_directory());
    // And what the desktop keeps there.
    StringView const more[] = { "Library/Preferences"_sv, ".Trash"_sv };
    for (auto folder : more) {
        StringBuilder path;
        path.appendff("{}/{}", home.view(), folder);
        auto text = TRY(path.to_string());
        if (mkdir(text.characters(), 0700) == 0)
            (void)chown(text.characters(), uid, staff_gid);
    }
    return uid;
}

ErrorOr<void> remove(StringView name, HomeFolder home_folder)
{
    auto person = person_named(name);
    if (!person.has_value())
        return Error::from_errno(ENOENT);
    Lock lock;
    if (!lock.held())
        return Error::from_errno(EBUSY);
    auto master = TRY(read_text(master_path));
    auto passwd = TRY(read_text(passwd_path));
    auto group = TRY(read_text(group_path));
    if (person->administrator) {
        size_t others = 0;
        for (auto const& each : people()) {
            if (each.administrator && each.name.view() != name)
                ++others;
        }
        if (others == 0)
            return Error::from_errno(EPERM);
    }
    auto without = [&](StringView text) -> ErrorOr<String> {
        StringBuilder out;
        for_each_line(text, [&](StringView line) {
            StringView fields[1];
            fields_of(line, fields, 1);
            if (fields[0] == name && line.contains(':'))
                return;
            out.append(line);
            out.append('\n');
        });
        return out.to_string();
    };
    auto new_group = TRY(group_with_member(group.view(), "staff"_sv, name, false));
    new_group = TRY(group_with_member(new_group.view(), "admin"_sv, name, false));
    TRY(write_text(passwd_path, TRY(without(passwd.view())).view(), 0644));
    TRY(write_text(master_path, TRY(without(master.view())).view(), 0600));
    TRY(write_text(group_path, new_group.view(), 0644));
    auto hints = TRY(hints_without(name));
    (void)write_text(hints_path, hints.view(), 0600);
    struct stat information;
    if (stat(person->home.characters(), &information) == 0) {
        if (home_folder == HomeFolder::Delete) {
            TRY(remove_tree(person->home));
        } else {
            StringBuilder kept;
            kept.appendff("{} (Deleted)", person->home.view());
            auto kept_path = TRY(kept.to_string());
            if (rename(person->home.characters(), kept_path.characters()) < 0)
                return Error::from_errno(errno);
        }
    }
    return {};
}

ErrorOr<void> set_administrator(StringView name, bool administrator)
{
    auto person = person_named(name);
    if (!person.has_value())
        return Error::from_errno(ENOENT);
    if (person->administrator == administrator)
        return {};
    Lock lock;
    if (!lock.held())
        return Error::from_errno(EBUSY);
    if (!administrator) {
        size_t others = 0;
        for (auto const& each : people()) {
            if (each.administrator && each.name.view() != name)
                ++others;
        }
        if (others == 0)
            return Error::from_errno(EPERM);
    }
    auto group = TRY(read_text(group_path));
    auto new_group = TRY(group_with_member(group.view(), "admin"_sv, name, administrator));
    return write_text(group_path, new_group.view(), 0644);
}

ErrorOr<void> set_hint(StringView name, StringView hint)
{
    StringBuilder out;
    out.append(TRY(hints_without(name)).view());
    if (!hint.is_empty()) {
        out.append(name);
        out.append('\t');
        for (char c : hint) {
            if (c != '\n' && c != '\r' && c != '\t')
                out.append(c);
        }
        out.append('\n');
    }
    return write_text(hints_path, out.view(), 0600);
}

Optional<String> hint(StringView name)
{
    auto text = read_text(hints_path);
    if (text.is_error())
        return {};
    Optional<String> found;
    for_each_line(text.value().view(), [&](StringView line) {
        auto tab = line.find('\t');
        if (tab.has_value() && line.substring_view(0, tab.value()) == name)
            found = MUST(String::from_view(line.substring_view(tab.value() + 1)));
    });
    return found;
}

bool setup_done()
{
    return access(setup_marker_path, F_OK) == 0 || has_people();
}

ErrorOr<void> mark_setup_done()
{
    int fd = open(setup_marker_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return Error::from_errno(errno);
    close(fd);
    return {};
}

}
