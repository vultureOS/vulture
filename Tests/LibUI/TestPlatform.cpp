/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "TestPlatform.h"
#include <LibUI/WindowRegistry.h>

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
size_t fread(void* buffer, size_t size, size_t count, FILE*);
int fclose(FILE*);
}

using namespace Gfx;

namespace {

class TestBackend final : public UI::WindowBackend {
public:
    TestBackend(UI::Window& owner, IntRect frame, int scale)
        : m_owner(owner)
        , m_frame(frame)
        , m_scale(scale)
    {
        remake();
    }

    Bitmap& back_buffer() override { return *m_bitmap; }
    void present(IntRect) override
    {
        // On screen at once.
        TestPlatform::the().defer([owner = &m_owner] {
            // (It may have gone since.)
            for (auto* window : UI::WindowRegistry::windows()) {
                if (window == owner) {
                    owner->handle_presented();
                    return;
                }
            }
        });
    }
    int scale() const override { return m_scale; }
    IntRect frame() const override { return m_frame; }
    void set_frame(IntRect frame) override
    {
        bool resized = frame.size() != m_frame.size();
        m_frame = frame;
        if (resized)
            remake();
    }
    void set_title(StringView) override { }
    void show(bool) override { }
    void activate() override { m_owner.handle_activation(true); }
    void set_level(int) override { }
    void set_corner_radius(int) override { }
    void set_title_bar_height(int) override { }
    void begin_move() override { }
    void grab_pointer(bool) override { }
    void set_cursor(UI::WS::CursorShape) override { }
    void begin_drag(UI::WindowBackend&, StringView, ReadonlyBytes) override { }
    void accept_drop(bool) override { }
    void set_materials(Span<UI::MaterialRegion const>) override { }

private:
    void remake()
    {
        m_bitmap = MUST(Bitmap::create({ m_frame.width * m_scale, m_frame.height * m_scale }));
    }

    UI::Window& m_owner;
    IntRect m_frame;
    int m_scale;
    RefPtr<Bitmap> m_bitmap;
};

}

TestPlatform& TestPlatform::the()
{
    static TestPlatform* platform = nullptr;
    if (!platform) {
        platform = new TestPlatform;
        UI::Platform::set(platform);
    }
    return *platform;
}

ErrorOr<NonnullOwnPtr<UI::WindowBackend>> TestPlatform::create_window(UI::Window& owner, StringView, IntRect frame, u32)
{
    return adopt_own(*static_cast<UI::WindowBackend*>(new TestBackend(owner, frame, m_scale)));
}

int TestPlatform::add_timer(u64 milliseconds, bool repeating, Function<void()> callback)
{
    int id = m_next_timer++;
    double every = static_cast<double>(milliseconds) / 1000;
    m_timers.append(Timer { id, m_now + every, every, repeating, move(callback) });
    return id;
}

void TestPlatform::cancel_timer(int id)
{
    m_timers.remove_all_matching([id](auto const& timer) { return timer.id == id; });
}

void TestPlatform::defer(Function<void()> callback)
{
    m_deferred.append(move(callback));
}

void TestPlatform::run_deferred()
{
    // What they defer runs too: until there is nothing left to draw.
    for (int round = 0; round < 64 && !m_deferred.is_empty(); ++round) {
        auto batch = move(m_deferred);
        m_deferred = {};
        for (auto& callback : batch)
            callback();
    }
}

void TestPlatform::advance(double seconds)
{
    double end = m_now + seconds;
    for (;;) {
        run_deferred();
        Timer* next = nullptr;
        for (auto& timer : m_timers) {
            if (timer.due <= end && (!next || timer.due < next->due))
                next = &timer;
        }
        if (!next)
            break;
        m_now = next->due;
        int id = next->id;
        // The callback may add or cancel timers: it may be gone, or moved, after.
        if (next->repeating) {
            next->due += next->every;
            auto callback = move(next->callback);
            next->callback = nullptr;
            callback();
            for (auto& timer : m_timers) {
                if (timer.id == id)
                    timer.callback = move(callback);
            }
        } else {
            auto callback = move(next->callback);
            cancel_timer(id);
            callback();
        }
    }
    m_now = end;
    run_deferred();
}

ErrorOr<Vector<u8>> TestPlatform::read_file(StringView path)
{
    auto terminated = TRY(String::from_view(path));
    FILE* file = fopen(terminated.characters(), "rb");
    if (!file)
        return Error::from_errno(2);
    Vector<u8> bytes;
    u8 chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        size_t at = bytes.size();
        TRY(bytes.try_resize(at + got));
        __builtin_memcpy(bytes.data() + at, chunk, got);
    }
    fclose(file);
    return bytes;
}

ErrorOr<Vector<UI::Platform::DirectoryEntry>> TestPlatform::list_directory(StringView path)
{
    Vector<DirectoryEntry> entries;
    // A little tree, that tests of the file panels go through.
    if (path == "/"_sv) {
        entries.append(DirectoryEntry { MUST(String::from_view("Users"_sv)), true });
        entries.append(DirectoryEntry { MUST(String::from_view("readme.txt"_sv)), false });
        return entries;
    }
    if (path == "/Users"_sv) {
        entries.append(DirectoryEntry { MUST(String::from_view("notes.txt"_sv)), false });
        entries.append(DirectoryEntry { MUST(String::from_view("Pictures"_sv)), true });
        return entries;
    }
    return Error::from_errno(2);
}

void TestPlatform::copy_text(StringView text)
{
    m_pasteboard = MUST(String::from_view(text));
}

ErrorOr<String> TestPlatform::paste_text()
{
    return String::from_view(m_pasteboard.view());
}

Bitmap& shown_pixels(UI::Window& window)
{
    TestPlatform::the().run_deferred();
    window.update_now();
    TestPlatform::the().run_deferred();
    return window.backend().back_buffer();
}
