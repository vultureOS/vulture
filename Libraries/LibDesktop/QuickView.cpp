/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "QuickView.h"
#include "ContentTypes.h"
#include "Desktop.h"
#include <LibBase/HashMap.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Vector.h>
#include <LibCore/EventLoop.h>
#include <errno.h>
#include <fcntl.h>
#include <sandbox.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Desktop {

namespace {

constexpr unsigned magic = 0x31565156;
constexpr size_t header_size = 12;
constexpr size_t most_pixels = 64 * MiB;
// A plugin that's looping forever is as good as one that's crashed.
constexpr u64 helper_time_limit_milliseconds = 8000;
constexpr StringView helper_path = "/usr/libexec/QuickViewHelper"_sv;

StringView extension_of(StringView path)
{
    auto slash = path.find_last('/');
    auto name = slash.has_value() ? path.substring_view(slash.value() + 1) : path;
    auto dot = name.find_last('.');
    if (!dot.has_value() || dot.value() == 0)
        return {};
    return name.substring_view(dot.value() + 1);
}

RefPtr<Gfx::Bitmap> bitmap_from(u8 const* bytes, size_t size)
{
    if (size < header_size)
        return nullptr;
    unsigned header[3];
    memcpy(header, bytes, sizeof(header));
    size_t const expected = static_cast<size_t>(header[1]) * header[2] * 4;
    if (header[0] != magic || header[1] == 0 || header[2] == 0 || expected > most_pixels || size - header_size != expected)
        return nullptr;
    auto bitmap = Gfx::Bitmap::create({ static_cast<int>(header[1]), static_cast<int>(header[2]) });
    if (bitmap.is_error())
        return nullptr;
    for (int y = 0; y < static_cast<int>(header[2]); ++y)
        memcpy(bitmap.value()->scanline(y), bytes + header_size + static_cast<size_t>(y) * header[1] * 4, static_cast<size_t>(header[1]) * 4);
    return bitmap.release_value();
}

class Request {
public:
    Request(int fd, pid_t helper, Function<void(RefPtr<Gfx::Bitmap>)> then)
        : m_fd(fd)
        , m_helper(helper)
        , m_then(move(then))
    {
        auto& loop = Core::EventLoop::current();
        m_watch = loop.watch(m_fd, Core::EventLoop::Interest::Read, [this] { readable(); });
        m_timer = loop.add_timer(helper_time_limit_milliseconds, false, [this] {
            kill(m_helper, SIGKILL);
            finish();
        });
    }

private:
    void readable()
    {
        static u8 buffer[256 * 1024];
        ssize_t got = read(m_fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            return;
        if (got > 0 && m_bytes.size() + static_cast<size_t>(got) <= most_pixels + header_size) {
            MUST(m_bytes.try_ensure_capacity(m_bytes.size() + static_cast<size_t>(got)));
            for (ssize_t i = 0; i < got; ++i)
                m_bytes.append(buffer[i]);
            return;
        }
        finish();
    }

    void finish()
    {
        auto& loop = Core::EventLoop::current();
        loop.unwatch(m_watch);
        loop.cancel_timer(m_timer);
        close(m_fd);
        int status = 0;
        (void)waitpid(m_helper, &status, 0);
        RefPtr<Gfx::Bitmap> result;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            result = bitmap_from(m_bytes.data(), m_bytes.size());
        auto then = move(m_then);
        auto* self = this;
        loop.deferred_invoke([self] { delete self; });
        if (then)
            then(move(result));
    }

    int m_fd;
    pid_t m_helper;
    Function<void(RefPtr<Gfx::Bitmap>)> m_then;
    Vector<u8> m_bytes;
    int m_watch { -1 };
    int m_timer { -1 };
};

String cache_folder()
{
    StringBuilder folder;
    folder.append(home_directory().view());
    folder.append("/Library/Caches/com.vulture.QuickView"_sv);
    return MUST(folder.to_string());
}

void make_cache_folder()
{
    StringBuilder library;
    library.append(home_directory().view());
    library.append("/Library/Caches"_sv);
    (void)mkdir(MUST(library.to_string()).characters(), 0700);
    (void)mkdir(cache_folder().characters(), 0700);
}

Optional<String> cache_path_for(StringView path, int pixels)
{
    struct stat information;
    if (stat(MUST(String::from_view(path)).characters(), &information) != 0)
        return {};
    StringBuilder key;
    key.appendff("{}|{}|{}|{}", path, static_cast<i64>(information.st_mtime), static_cast<i64>(information.st_size), pixels);
    StringBuilder cached;
    cached.appendff("{}/{:016x}", cache_folder().view(), Base::hash_bytes(key.view().characters_without_null_termination(), key.view().length()));
    return MUST(cached.to_string());
}

RefPtr<Gfx::Bitmap> read_cached(StringView cached)
{
    int fd = open(MUST(String::from_view(cached)).characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    Vector<u8> bytes;
    u8 buffer[65536];
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got <= 0)
            break;
        MUST(bytes.try_ensure_capacity(bytes.size() + static_cast<size_t>(got)));
        for (ssize_t i = 0; i < got; ++i)
            bytes.append(buffer[i]);
    }
    close(fd);
    return bitmap_from(bytes.data(), bytes.size());
}

void write_cached(StringView cached, Gfx::Bitmap const& bitmap)
{
    int fd = open(MUST(String::from_view(cached)).characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    unsigned const header[3] = { magic, static_cast<unsigned>(bitmap.width()), static_cast<unsigned>(bitmap.height()) };
    (void)write(fd, header, sizeof(header));
    for (int y = 0; y < bitmap.height(); ++y)
        (void)write(fd, bitmap.scanline(y), static_cast<size_t>(bitmap.width()) * 4);
    close(fd);
}

}

bool can_preview(StringView path)
{
    auto type = content_type_of_path(path);
    if (type == "public.folder"_sv || type == "com.apple.application-bundle"_sv || type == "public.symlink"_sv)
        return false;
    return true;
}

void request_preview(StringView path, int most_width, int most_height, Function<void(RefPtr<Gfx::Bitmap>)> then)
{
    int document = open(MUST(String::from_view(path)).characters(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (document < 0) {
        then(nullptr);
        return;
    }
    int answer[2];
    if (pipe(answer) != 0) {
        close(document);
        then(nullptr);
        return;
    }
    auto type = MUST(String::from_view(content_type_of_path(path)));
    auto extension = MUST(String::from_view(extension_of(path)));
    StringBuilder width_text;
    width_text.appendff("{}", most_width);
    StringBuilder height_text;
    height_text.appendff("{}", most_height);
    auto width_string = MUST(width_text.to_string());
    auto height_string = MUST(height_text.to_string());
    auto program = MUST(String::from_view(helper_path));

    pid_t helper = fork();
    if (helper < 0) {
        close(document);
        close(answer[0]);
        close(answer[1]);
        then(nullptr);
        return;
    }
    if (helper == 0) {
        dup2(document, STDIN_FILENO);
        dup2(answer[1], STDOUT_FILENO);
        // Before it starts, so that nothing of it ever runs with more than this.
        if (sandbox_init_for_program("service-quickview", SANDBOX_NAMED, program.characters(), nullptr) != 0)
            _exit(126);
        char* argv[] = { const_cast<char*>(program.characters()), const_cast<char*>(type.characters()), const_cast<char*>(extension.characters()),
            const_cast<char*>(width_string.characters()), const_cast<char*>(height_string.characters()), nullptr };
        execv(argv[0], argv);
        _exit(127);
    }
    close(document);
    close(answer[1]);
    new Request(answer[0], helper, move(then));
}

void request_thumbnail(StringView path, int pixels, Function<void(RefPtr<Gfx::Bitmap>)> then)
{
    auto cached = cache_path_for(path, pixels);
    if (cached.has_value()) {
        if (auto bitmap = read_cached(cached->view())) {
            then(move(bitmap));
            return;
        }
    }
    auto where = cached.has_value() ? cached.release_value() : String {};
    request_preview(path, pixels, pixels, [where = move(where), then = move(then)](RefPtr<Gfx::Bitmap> bitmap) mutable {
        if (bitmap && !where.is_empty()) {
            make_cache_folder();
            write_cached(where.view(), *bitmap);
        }
        then(move(bitmap));
    });
}

}
