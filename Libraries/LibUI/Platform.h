/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibAnimation/Curve.h>
#include <LibBase/Error.h>
#include <LibBase/Function.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibUI/Event.h>
#include <LibWindow/Protocol.h>

// What pranaUI needs from the system: windows, time and files. The window server
// provides it in a running program (see Application); a test provides its own, and
// so runs the toolkit with no server at all, and on the host.

namespace UI {

class Window;

struct MaterialRegion {
    Gfx::IntRect rect;
    WS::Material material;
    int radius { 0 };
};

// One of a window's layers, as whatever shows it sees it.
class LayerBackend {
public:
    virtual ~LayerBackend() = default;
    [[nodiscard]] virtual ErrorOr<void> set_surface(Gfx::IntSize pixels) = 0;
    [[nodiscard]] virtual Gfx::Bitmap& back_buffer() = 0;
    [[nodiscard]] virtual Gfx::Bitmap const& shown_buffer() const = 0;
};

// One window's link to whatever shows it.
class WindowBackend {
public:
    virtual ~WindowBackend() = default;

    [[nodiscard]] virtual Gfx::Bitmap& back_buffer() = 0;
    virtual void present(Gfx::IntRect damage) = 0;
    [[nodiscard]] virtual int scale() const = 0;
    // What the system knows this window by, if anything does.
    [[nodiscard]] virtual u32 system_id() const { return 0; }
    // On the screen, in points.
    [[nodiscard]] virtual Gfx::IntRect frame() const = 0;
    virtual void set_frame(Gfx::IntRect) = 0;
    virtual void set_title(StringView) = 0;
    virtual void show(bool visible) = 0;
    virtual void activate() = 0;
    virtual void set_level(int) = 0;
    virtual void set_corner_radius(int) = 0;
    virtual void set_title_bar_height(int) = 0;
    virtual void begin_move() = 0;
    virtual void grab_pointer(bool) = 0;
    virtual void set_cursor(WS::CursorShape) = 0;
    virtual void minimize() { }
    virtual void arrange(WS::WindowArrangement) { }
    virtual void toggle_full_screen() { }
    virtual void take_place_of(WindowBackend&) { }
    virtual void tile_in_split_view(bool) { }
    virtual void zoom() { }
    virtual void set_input_region(Gfx::IntRect) { }
    virtual void begin_drag(WindowBackend& image, StringView type, ReadonlyBytes data) = 0;
    virtual void accept_drop(bool) = 0;
    virtual void set_materials(Span<MaterialRegion const>) = 0;

    // Layers, for what can have them: with none, a view that wants one is drawn along with everything else, and looks the same.
    [[nodiscard]] virtual bool supports_layers() const { return false; }
    [[nodiscard]] virtual ErrorOr<NonnullOwnPtr<LayerBackend>> create_layer(LayerBackend const*);
    // Changes, to go together: all on the screen in the same frame.
    virtual void layer_set(LayerBackend const&, WS::LayerProperty, float = 0, float = 0, float = 0, float = 0) { }
    virtual void layer_animate(LayerBackend const&, WS::LayerProperty, Span<float const>, Span<float const>, Animation::Timing const&, u32, bool, int) { }
    virtual void layer_destroy(LayerBackend const&) { }
    virtual void layer_present(LayerBackend&, Gfx::IntRect) { }
    virtual void layer_present_window(Gfx::IntRect) { }
    [[nodiscard]] virtual bool has_layer_changes() const { return false; }
    virtual void layer_commit() { }
};

class Platform {
public:
    virtual ~Platform() = default;

    // There is one, set before any window is made.
    [[nodiscard]] static Platform& the();
    static void set(Platform*);

    [[nodiscard]] virtual ErrorOr<NonnullOwnPtr<WindowBackend>> create_window(Window& owner, StringView title, Gfx::IntRect frame, u32 flags) = 0;
    [[nodiscard]] virtual Gfx::IntSize screen_size() const = 0;
    [[nodiscard]] virtual int scale() const = 0;

    // Timers, on the program's one thread.
    virtual int add_timer(u64 milliseconds, bool repeating, Function<void()>) = 0;
    virtual void cancel_timer(int id) = 0;
    // At the start of the next round of the loop.
    virtual void defer(Function<void()>) = 0;
    // Monotonic.
    [[nodiscard]] virtual double now() const = 0;

    [[nodiscard]] virtual ErrorOr<Vector<u8>> read_file(StringView path) = 0;
    // Where the fonts and themes are: /System/Library, in a running system.
    [[nodiscard]] virtual StringView library_directory() const = 0;

    struct DirectoryEntry {
        String name;
        bool is_directory;
    };
    [[nodiscard]] virtual ErrorOr<Vector<DirectoryEntry>> list_directory(StringView path) = 0;

    // The general pasteboard, as text.
    virtual void copy_text(StringView) = 0;
    [[nodiscard]] virtual ErrorOr<String> paste_text() = 0;

    // One of the system's sounds, if there is anything to hear it with (see LibAudio/Sounds.h).
    virtual void play_sound(StringView) { }
};

}
