/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/HashMap.h>
#include <LibGfx/Bitmap.h>
#include <LibUI/Platform.h>
#include <LibUI/Window.h>

// pranaUI's platform in a test: windows are bitmaps, time passes when told to, and the
// timers run when it does.
class TestPlatform final : public UI::Platform {
public:
    static TestPlatform& the();

    ErrorOr<NonnullOwnPtr<UI::WindowBackend>> create_window(UI::Window&, StringView title, Gfx::IntRect frame, u32 flags) override;
    Gfx::IntSize screen_size() const override { return { 1280, 800 }; }
    int scale() const override { return m_scale; }
    int add_timer(u64 milliseconds, bool repeating, Function<void()>) override;
    void cancel_timer(int id) override;
    void defer(Function<void()>) override;
    double now() const override { return m_now; }
    ErrorOr<Vector<u8>> read_file(StringView path) override;
    StringView library_directory() const override { return StringView(VULTURE_LIBRARY_DIRECTORY); }
    ErrorOr<Vector<DirectoryEntry>> list_directory(StringView path) override;
    void copy_text(StringView) override;
    ErrorOr<String> paste_text() override;

    void set_scale(int scale) { m_scale = scale; }
    // Runs what was deferred, and the timers due by then.
    void advance(double seconds);
    void run_deferred();

private:
    struct Timer {
        int id;
        double due;
        double every;
        bool repeating;
        Function<void()> callback;
    };

    int m_scale { 2 };
    double m_now { 100 };
    int m_next_timer { 1 };
    Vector<Timer> m_timers;
    Vector<Function<void()>> m_deferred;
    String m_pasteboard;
};

// The window's picture as it now stands, once everything pending has been drawn.
[[nodiscard]] Gfx::Bitmap& shown_pixels(UI::Window&);
