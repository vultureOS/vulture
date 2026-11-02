/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibArchive/Tar.h>
#include <LibCompress/Deflate.h>
#include <LibCrypto/SHA2.h>
#include <LibHTTP/Client.h>
#include <LibPackage/Manager.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <os/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Package {

namespace {

constexpr char const* repositories_path = "/etc/pkg/repositories.json";
constexpr char const* database_directory = "/var/db/pkg";
constexpr char const* cache_directory = "/var/db/pkg/cache";
// A package larger than this is not one we expect.
constexpr u64 largest_package = 1024 * 1024 * 1024;

os_log_t package_log()
{
    static os_log_t const log = os_log_create("com.vulture.pkg", "packages");
    return log;
}

ErrorOr<Vector<u8>> read_whole_file(char const* path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    Vector<u8> bytes;
    for (;;) {
        u8 chunk[65536];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            int code = errno;
            close(fd);
            return Error::from_errno(code);
        }
        if (got == 0)
            break;
        size_t const at = bytes.size();
        TRY(bytes.try_resize(at + static_cast<size_t>(got)));
        memcpy(bytes.data() + at, chunk, static_cast<size_t>(got));
    }
    close(fd);
    return bytes;
}

ErrorOr<void> write_whole_file(char const* path, ReadonlyBytes bytes, mode_t mode)
{
    // The whole file or none of it, however the machine stops.
    StringBuilder temporary;
    temporary.appendff("{}.new", StringView { path, strlen(path) });
    auto temporary_path = TRY(temporary.to_string());
    int fd = open(temporary_path.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return Error::from_errno(errno);
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t written = write(fd, bytes.data() + done, bytes.size() - done);
        if (written < 0) {
            int code = errno;
            close(fd);
            unlink(temporary_path.characters());
            return Error::from_errno(code);
        }
        done += static_cast<size_t>(written);
    }
    fsync(fd);
    close(fd);
    if (rename(temporary_path.characters(), path) != 0) {
        int code = errno;
        unlink(temporary_path.characters());
        return Error::from_errno(code);
    }
    return {};
}

String path_in(char const* directory, StringView name, StringView suffix = {})
{
    StringBuilder builder;
    builder.appendff("{}/{}{}", StringView { directory, strlen(directory) }, name, suffix);
    return MUST(builder.to_string());
}

void make_directories_for(StringView path)
{
    StringBuilder so_far;
    size_t const end = path.find_last('/').value_or(0);
    StringView const directory = path.substring_view(0, end);
    if (directory.is_empty())
        return;
    bool first = true;
    bool const absolute = directory[0] == '/';
    directory.for_each_split_view('/', SplitBehavior::SkipEmpty, [&](StringView part) {
        if (absolute || !first)
            so_far.append("/"_sv);
        so_far.append(part);
        first = false;
        auto text = MUST(so_far.to_string());
        mkdir(text.characters(), 0755);
    });
}

ErrorOr<Vector<u8>> fetch_to_memory(StringView url_text, Repository const& repository, u64 size_limit, Progress const& progress)
{
    HTTP::Request request;
    auto url = HTTP::URL::parse(url_text);
    if (url.is_error())
        return Error::from_string_literal("a repository's URL makes no sense");
    request.url = url.release_value();
    Vector<u8> body;
    HTTP::Options options;
    options.follow_redirects = true;
    options.timeout_seconds = 600;
    options.connect_timeout_seconds = 20;
    options.user_agent = "vultureOS pkg"_sv;
    if (!repository.certificates.is_empty())
        options.certificates_file = repository.certificates.view();
    bool too_large = false;
    options.on_body = [&](ReadonlyBytes bytes) -> ErrorOr<void> {
        if (body.size() + bytes.size() > size_limit) {
            too_large = true;
            return Error::from_string_literal("more than was promised");
        }
        size_t const at = body.size();
        TRY(body.try_resize(at + bytes.size()));
        memcpy(body.data() + at, bytes.data(), bytes.size());
        return {};
    };
    if (progress) {
        options.on_progress = [&progress](u64 received, u64 total) {
            if (total > 0)
                progress(static_cast<u32>(received * 90 / total));
        };
    }
    HTTP::ResponseHead head;
    HTTP::Failure failure;
    if (!HTTP::fetch(request, options, head, failure)) {
        os_log_error(package_log(), "%s: %s", request.url.to_string().characters(), failure.message.characters());
        if (too_large)
            return Error::from_string_literal("a repository sent more than it promised");
        switch (failure.code) {
        case static_cast<int>(HTTP::FailureCode::CouldNotResolve):
            return Error::from_string_literal("the repository's server could not be found");
        case static_cast<int>(HTTP::FailureCode::CouldNotConnect):
        case static_cast<int>(HTTP::FailureCode::TimedOut):
            return Error::from_string_literal("the repository's server could not be reached");
        case static_cast<int>(HTTP::FailureCode::BadCertificate):
            return Error::from_string_literal("the repository's server has a certificate that can't be trusted");
        default:
            return Error::from_string_literal("the repository's server could not be talked to");
        }
    }
    if (head.status != 200) {
        os_log_error(package_log(), "%s: status %d", request.url.to_string().characters(), head.status);
        return Error::from_string_literal("the repository's server doesn't have that");
    }
    return body;
}

String cache_path_of(Repository const& repository, StringView suffix)
{
    return path_in(cache_directory, repository.name.view(), suffix);
}

JSON::Value string_value(StringView text)
{
    return MUST(JSON::Value::string(text));
}

}

ErrorOr<Vector<Repository>> configured_repositories()
{
    auto bytes = read_whole_file(repositories_path);
    if (bytes.is_error()) {
        if (bytes.error().code() == ENOENT)
            return Vector<Repository> {};
        return bytes.release_error();
    }
    auto root = TRY(JSON::parse({ reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size() }));
    Vector<Repository> repositories;
    auto const* list = root.get("repositories"_sv);
    if (!list || !list->is_array())
        return repositories;
    for (size_t i = 0; i < list->size(); ++i) {
        auto const& entry = list->at(i);
        auto const name = entry.get_string("name"_sv);
        auto const url = entry.get_string("url"_sv);
        auto const key = entry.get_string("key"_sv);
        if (!name.has_value() || !url.has_value() || !key.has_value() || !is_valid_name(name.value()))
            return Error::from_string_literal("/etc/pkg/repositories.json: an entry with something missing");
        Repository repository;
        repository.name = TRY(String::from_view(name.value()));
        repository.url = TRY(String::from_view(url.value()));
        repository.key = TRY(String::from_view(key.value()));
        if (auto certificates = entry.get_string("certificates"_sv); certificates.has_value())
            repository.certificates = TRY(String::from_view(certificates.value()));
        TRY(repositories.try_append(move(repository)));
    }
    return repositories;
}

ErrorOr<void> refresh()
{
    auto repositories = TRY(configured_repositories());
    mkdir(database_directory, 0755);
    mkdir(cache_directory, 0755);
    Optional<Error> first_error;
    for (auto const& repository : repositories) {
        auto attempt = [&]() -> ErrorOr<void> {
            StringBuilder index_url;
            index_url.appendff("{}/index.json", repository.url.view());
            StringBuilder signature_url;
            signature_url.appendff("{}/index.json.sig", repository.url.view());
            auto index = TRY(fetch_to_memory(index_url.view(), repository, 64 * 1024 * 1024, nullptr));
            auto signature = TRY(fetch_to_memory(signature_url.view(), repository, 4096, nullptr));
            StringView signature_text { reinterpret_cast<char const*>(signature.data()), signature.size() };
            while (signature_text.ends_with("\n"_sv))
                signature_text = signature_text.substring_view(0, signature_text.length() - 1);
            if (!verify_signature(index.span(), signature_text, repository.key.view()))
                return Error::from_string_literal("a repository's index isn't signed by its key");
            // Only now is any of it believed.
            u64 serial = 0;
            (void)TRY(parse_index(index.span(), repository.url.view(), repository.name.view(), &serial));
            auto cached_path = cache_path_of(repository, ".json"_sv);
            auto previous = read_whole_file(cached_path.characters());
            if (!previous.is_error()) {
                u64 previous_serial = 0;
                auto parsed = parse_index(previous.value().span(), repository.url.view(), repository.name.view(), &previous_serial);
                if (!parsed.is_error() && serial < previous_serial)
                    return Error::from_string_literal("a repository offered an older index than it did before");
            }
            TRY(write_whole_file(cached_path.characters(), index.span(), 0644));
            return {};
        };
        auto result = attempt();
        if (result.is_error()) {
            os_log_error(package_log(), "%s: %.*s", repository.name.characters(), static_cast<int>(result.error().string_literal().length()), result.error().string_literal().characters_without_null_termination());
            if (!first_error.has_value())
                first_error = result.release_error();
        }
    }
    if (first_error.has_value())
        return first_error.release_value();
    return {};
}

ErrorOr<Vector<Available>> available()
{
    auto repositories = TRY(configured_repositories());
    Vector<Available> all;
    for (auto const& repository : repositories) {
        auto bytes = read_whole_file(cache_path_of(repository, ".json"_sv).characters());
        if (bytes.is_error())
            continue;
        auto packages = parse_index(bytes.value().span(), repository.url.view(), repository.name.view());
        if (packages.is_error())
            continue;
        for (auto& package : packages.value())
            TRY(all.try_append(move(package)));
    }
    return all;
}

ErrorOr<Vector<Installed>> installed()
{
    Vector<Installed> all;
    DIR* directory = opendir(database_directory);
    if (!directory)
        return all;
    while (auto* entry = readdir(directory)) {
        StringView const name { entry->d_name, strlen(entry->d_name) };
        if (!name.ends_with(".json"_sv))
            continue;
        auto bytes = read_whole_file(path_in(database_directory, name).characters());
        if (bytes.is_error())
            continue;
        auto root = JSON::parse({ reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size() });
        if (root.is_error())
            continue;
        Installed package;
        package.name = MUST(String::from_view(root.value().get_string("name"_sv).value_or(""_sv)));
        package.version = MUST(String::from_view(root.value().get_string("version"_sv).value_or(""_sv)));
        package.summary = MUST(String::from_view(root.value().get_string("summary"_sv).value_or(""_sv)));
        if (auto const* files = root.value().get("files"_sv); files && files->is_array()) {
            for (size_t i = 0; i < files->size(); ++i) {
                if (files->at(i).is_string())
                    package.files.append(MUST(String::from_view(files->at(i).as_string())));
            }
        }
        if (!package.name.is_empty())
            all.append(move(package));
    }
    closedir(directory);
    return all;
}

ErrorOr<void> install(StringView name, Progress const& progress)
{
    auto packages = TRY(available());
    Available const* package = nullptr;
    for (auto const& candidate : packages) {
        if (candidate.name.view() == name)
            package = &candidate;
    }
    if (!package)
        return Error::from_string_literal("no such package: try \"pkg update\"");
    if (package->size > largest_package)
        return Error::from_string_literal("that package is too large to be believed");

    auto repositories = TRY(configured_repositories());
    Repository const* repository = nullptr;
    for (auto const& candidate : repositories) {
        if (candidate.name.view() == package->repository.view())
            repository = &candidate;
    }
    if (!repository)
        return Error::from_string_literal("the package's repository is gone");

    os_log(package_log(), "Installing %s %s from %s", package->name.characters(), package->version.characters(), repository->name.characters());
    auto bytes = TRY(fetch_to_memory(package->url.view(), *repository, package->size, progress));
    auto const digest = Crypto::SHA256::hash(bytes.span());
    if (bytes.size() != package->size || hex_of(digest.span()).view() != package->sha256.view())
        return Error::from_string_literal("the package isn't what its signed index says it is");
    if (progress)
        progress(92);

    auto unpacked = TRY(Compress::gzip_decompress(bytes.span()));
    auto tar = TRY(Archive::TarReader::parse(unpacked.span()));

    // The manifest must agree with the index.
    bool manifest_agrees = false;
    if (auto manifest = tar.find("Manifest.json"_sv); manifest.has_value()) {
        auto root = TRY(JSON::parse({ reinterpret_cast<char const*>(manifest->data.data()), manifest->data.size() }));
        manifest_agrees = root.get_string("name"_sv).value_or(""_sv) == name && root.get_string("version"_sv).value_or(""_sv) == package->version.view();
    }
    if (!manifest_agrees)
        return Error::from_string_literal("the package's own manifest is for something else");

    // Everything it holds goes under the allowed places, and takes nobody else's files.
    auto current = TRY(installed());
    constexpr StringView payload_prefix = "Payload/"_sv;
    Vector<String> files;
    Optional<Error> problem;
    TRY(tar.for_each_entry([&](Archive::TarEntry const& entry) {
        if (entry.path == "Manifest.json"_sv || entry.path == "Payload"_sv)
            return Archive::IterationDecision::Continue;
        if (!entry.path.starts_with(payload_prefix)) {
            problem = Error::from_string_literal("the package holds something outside its payload");
            return Archive::IterationDecision::Break;
        }
        StringView const target = entry.path.substring_view(payload_prefix.length());
        if (entry.type == Archive::TarEntryType::Directory)
            return Archive::IterationDecision::Continue;
        if (!Archive::is_safe_member_path(target) || !is_allowed_install_path(target)) {
            problem = Error::from_string_literal("the package wants to put a file where it may not");
            return Archive::IterationDecision::Break;
        }
        if (entry.type == Archive::TarEntryType::Other) {
            problem = Error::from_string_literal("the package holds something that is not a file");
            return Archive::IterationDecision::Break;
        }
        if (entry.type == Archive::TarEntryType::SymbolicLink && !Archive::is_safe_link_target(target, entry.link_target)) {
            problem = Error::from_string_literal("the package has a link that leads out of it");
            return Archive::IterationDecision::Break;
        }
        for (auto const& other : current) {
            if (other.name.view() == name)
                continue;
            for (auto const& file : other.files) {
                if (file.view() == target) {
                    problem = Error::from_string_literal("the package has a file another package owns");
                    return Archive::IterationDecision::Break;
                }
            }
        }
        // Nor, if it isn't this package's own already, a file that's already there.
        StringBuilder absolute;
        absolute.appendff("/{}", target);
        auto absolute_path = MUST(absolute.to_string());
        struct stat info;
        if (lstat(absolute_path.characters(), &info) == 0) {
            bool ours = false;
            for (auto const& other : current) {
                if (other.name.view() != name)
                    continue;
                for (auto const& file : other.files) {
                    if (file.view() == target)
                        ours = true;
                }
            }
            if (!ours) {
                problem = Error::from_string_literal("the package would overwrite a file that is already there");
                return Archive::IterationDecision::Break;
            }
        }
        files.append(MUST(String::from_view(target)));
        return Archive::IterationDecision::Continue;
    }));
    if (problem.has_value())
        return problem.release_value();

    // Unpacked beside their places, and then all moved in.
    Vector<String> staged;
    auto undo_staged = [&] {
        for (auto const& path : staged)
            unlink(path.characters());
    };
    Optional<Error> failure;
    TRY(tar.for_each_entry([&](Archive::TarEntry const& entry) {
        if (entry.path == "Manifest.json"_sv || !entry.path.starts_with(payload_prefix) || entry.type == Archive::TarEntryType::Directory)
            return Archive::IterationDecision::Continue;
        StringView const target = entry.path.substring_view(payload_prefix.length());
        StringBuilder absolute;
        absolute.appendff("/{}.vpkg-new", target);
        auto path = MUST(absolute.to_string());
        make_directories_for(path.view());
        unlink(path.characters());
        if (entry.type == Archive::TarEntryType::SymbolicLink) {
            StringBuilder link_target;
            link_target.append(entry.link_target);
            auto text = MUST(link_target.to_string());
            if (symlink(text.characters(), path.characters()) != 0) {
                failure = Error::from_errno(errno);
                return Archive::IterationDecision::Break;
            }
        } else {
            // Nothing set-ID from a package, or that anyone can change.
            int fd = open(path.characters(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, entry.mode & 0755);
            if (fd < 0) {
                failure = Error::from_errno(errno);
                return Archive::IterationDecision::Break;
            }
            size_t done = 0;
            while (done < entry.data.size()) {
                ssize_t written = write(fd, entry.data.data() + done, entry.data.size() - done);
                if (written < 0) {
                    failure = Error::from_errno(errno);
                    close(fd);
                    return Archive::IterationDecision::Break;
                }
                done += static_cast<size_t>(written);
            }
            fsync(fd);
            close(fd);
        }
        staged.append(move(path));
        return Archive::IterationDecision::Continue;
    }));
    if (failure.has_value()) {
        undo_staged();
        return failure.release_value();
    }
    if (progress)
        progress(97);

    for (auto const& staged_path : staged) {
        StringView const view = staged_path.view();
        auto final_path = MUST(String::from_view(view.substring_view(0, view.length() - ".vpkg-new"_sv.length())));
        if (rename(staged_path.characters(), final_path.characters()) != 0) {
            int code = errno;
            undo_staged();
            return Error::from_errno(code);
        }
    }

    // What an older version had and this one doesn't.
    for (auto const& other : current) {
        if (other.name.view() != name)
            continue;
        for (auto const& file : other.files) {
            bool kept = false;
            for (auto const& now : files) {
                if (now.view() == file.view())
                    kept = true;
            }
            if (!kept && is_allowed_install_path(file.view())) {
                StringBuilder absolute;
                absolute.appendff("/{}", file.view());
                unlink(MUST(absolute.to_string()).characters());
            }
        }
    }

    auto record = JSON::Value::object();
    MUST(record.set("name"_sv, string_value(name)));
    MUST(record.set("version"_sv, string_value(package->version.view())));
    MUST(record.set("summary"_sv, string_value(package->summary.view())));
    auto list = JSON::Value::array();
    for (auto const& file : files)
        MUST(list.append(string_value(file.view())));
    MUST(record.set("files"_sv, move(list)));
    auto text = MUST(record.serialize());
    mkdir(database_directory, 0755);
    StringBuilder record_name;
    record_name.appendff("{}.json", name);
    TRY(write_whole_file(path_in(database_directory, record_name.view()).characters(), { reinterpret_cast<u8 const*>(text.characters()), text.length() }, 0644));
    sync();
    if (progress)
        progress(100);
    os_log(package_log(), "Installed %s %s", package->name.characters(), package->version.characters());
    notify_post("com.vulture.software.changed");
    return {};
}

ErrorOr<void> remove(StringView name)
{
    if (!is_valid_name(name))
        return Error::from_string_literal("no such package");
    auto current = TRY(installed());
    Installed const* package = nullptr;
    for (auto const& candidate : current) {
        if (candidate.name.view() == name)
            package = &candidate;
    }
    if (!package)
        return Error::from_string_literal("that package isn't installed");
    os_log(package_log(), "Removing %s", package->name.characters());
    for (auto const& file : package->files) {
        // The record is trusted no further than a package is.
        if (!is_allowed_install_path(file.view()))
            continue;
        StringBuilder absolute;
        absolute.appendff("/{}", file.view());
        auto path = MUST(absolute.to_string());
        unlink(path.characters());
        // Folders it leaves empty go too, up to where it began.
        StringView walk = path.view();
        for (;;) {
            auto const slash = walk.find_last('/');
            if (!slash.has_value() || slash.value() == 0)
                break;
            walk = walk.substring_view(0, slash.value());
            if (walk == "/Applications"_sv || walk == "/usr/local"_sv || walk == "/usr/local/bin"_sv)
                break;
            auto directory = MUST(String::from_view(walk));
            if (rmdir(directory.characters()) != 0)
                break;
        }
    }
    StringBuilder record_name;
    record_name.appendff("{}.json", name);
    unlink(path_in(database_directory, record_name.view()).characters());
    sync();
    notify_post("com.vulture.software.changed");
    return {};
}

String describe_catalog()
{
    auto root = JSON::Value::object();
    auto available_list = JSON::Value::array();
    auto installed_list = JSON::Value::array();
    auto all_available = available();
    auto all_installed = installed();
    auto is_installed = [&](StringView name) -> Installed const* {
        if (all_installed.is_error())
            return nullptr;
        for (auto const& package : all_installed.value()) {
            if (package.name.view() == name)
                return &package;
        }
        return nullptr;
    };
    if (!all_available.is_error()) {
        for (auto const& package : all_available.value()) {
            auto entry = JSON::Value::object();
            MUST(entry.set("name"_sv, string_value(package.name.view())));
            MUST(entry.set("version"_sv, string_value(package.version.view())));
            MUST(entry.set("summary"_sv, string_value(package.summary.view())));
            MUST(entry.set("size"_sv, JSON::Value(static_cast<i64>(package.size))));
            auto const* installed_package = is_installed(package.name.view());
            MUST(entry.set("installed"_sv, JSON::Value(installed_package != nullptr)));
            if (installed_package)
                MUST(entry.set("installedVersion"_sv, string_value(installed_package->version.view())));
            MUST(available_list.append(move(entry)));
        }
    }
    if (!all_installed.is_error()) {
        for (auto const& package : all_installed.value()) {
            auto entry = JSON::Value::object();
            MUST(entry.set("name"_sv, string_value(package.name.view())));
            MUST(entry.set("version"_sv, string_value(package.version.view())));
            MUST(entry.set("summary"_sv, string_value(package.summary.view())));
            // Which bundle it is, so an application can be taken off by its row.
            StringView bundle;
            for (auto const& file : package.files) {
                if (file.view().starts_with("Applications/"_sv)) {
                    auto const end = file.view().find(".app/"_sv);
                    if (end.has_value())
                        bundle = file.view().substring_view(0, end.value() + 4);
                    break;
                }
            }
            MUST(entry.set("bundle"_sv, string_value(bundle)));
            MUST(installed_list.append(move(entry)));
        }
    }
    MUST(root.set("available"_sv, move(available_list)));
    MUST(root.set("installed"_sv, move(installed_list)));
    auto repositories = configured_repositories();
    MUST(root.set("repositories"_sv, JSON::Value(repositories.is_error() ? 0 : static_cast<int>(repositories.value().size()))));
    return MUST(root.serialize());
}

}
