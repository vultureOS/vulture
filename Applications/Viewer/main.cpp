/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibImage/Image.h>
#include <LibUI/Application.h>
#include <LibUI/Containers.h>
#include <LibUI/Panels.h>
#include <LibUI/ScrollView.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

// A picture in a window: to fit, or at any size with the rest scrolled to. At 100%,
// one pixel of the picture is one point, whatever the screen.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

StringView last_component(StringView path)
{
    for (size_t i = path.length(); i > 0; --i) {
        if (path[i - 1] == '/')
            return path.substring_view(i, path.length() - i);
    }
    return path;
}

class PictureView final : public View {
public:
    explicit PictureView(RefPtr<Bitmap> bitmap)
        : m_bitmap(move(bitmap))
    {
    }

    void set_zoom(float zoom)
    {
        m_zoom = zoom;
        set_needs_layout();
        set_needs_display();
    }

    [[nodiscard]] Bitmap const* bitmap() const { return m_bitmap.ptr(); }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        if (!m_bitmap)
            return { 0, 0 };
        return { static_cast<float>(m_bitmap->width()) * m_zoom, static_cast<float>(m_bitmap->height()) * m_zoom };
    }

    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, tokens().color("viewer.background"_sv));
        if (!m_bitmap)
            return;
        float shown_width = static_cast<float>(m_bitmap->width()) * m_zoom;
        float shown_height = static_cast<float>(m_bitmap->height()) * m_zoom;
        // In the middle, when there is room around it.
        auto rect = graphics.snapped({ max(0.f, (width() - shown_width) / 2), max(0.f, (height() - shown_height) / 2), shown_width, shown_height });
        graphics.painter().draw_bitmap(*m_bitmap, rect);
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Image; }

private:
    RefPtr<Bitmap> m_bitmap;
    float m_zoom { 1 };
};

class ViewerContent final : public View {
public:
    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, tokens().color("viewer.background"_sv));
    }

    void layout() override
    {
        for (auto const& child : children())
            child->set_frame({ 0, 0, width(), height() });
    }
};

int s_windows_opened = 0;

class ViewerWindow final : public UI::Window {
public:
    ViewerWindow(StringView path, RefPtr<Bitmap> bitmap, StringView problem)
        : UI::Window(path.is_empty() ? "Viewer"_sv : last_component(path), start_frame(bitmap.ptr()), WS::WindowTitled | WS::WindowResizable | WS::WindowUnifiedTitle)
    {
        auto const& t = tokens();
        set_sheet_top(t.metric("window.toolbarUnified"_sv));
        auto& root = set_content<ViewerContent>();
        auto& stack = root.add<StackView>(Axis::Vertical);
        auto& toolbar = stack.add<Toolbar>();
        toolbar.set_title(title());
        toolbar.add_flexible_space();
        m_zoom_label = &toolbar.add_item<Label>("100%"_sv);
        m_zoom_label->set_style(LabelStyle::Secondary);
        (void)toolbar.add_button(Symbol::Minus, [this] { zoom_by(1 / tokens().metric("viewer.zoomStep"_sv)); }, "Zoom Out"_sv);
        (void)toolbar.add_button(Symbol::Plus, [this] { zoom_by(tokens().metric("viewer.zoomStep"_sv)); }, "Zoom In"_sv);
        auto& sizes = toolbar.add_segmented();
        sizes.set_mode(SegmentedControl::Mode::Momentary);
        sizes.add_segment("Fit"_sv);
        sizes.add_segment("100%"_sv);
        sizes.on_change = [this](size_t segment) {
            if (segment == 0)
                zoom_to_fit();
            else
                actual_size();
        };
        auto& scroll = stack.add<ScrollView>();
        scroll.set_stretch(1, 1);
        m_scroll = &scroll;
        m_picture = &scroll.make_document<PictureView>(move(bitmap));
        on_resize = [this](FloatSize) {
            if (m_fits)
                schedule_fit();
        };
        if (!problem.is_empty()) {
            auto text = MUST(String::from_view(problem));
            Application::the().event_loop().deferred_invoke([this, text = move(text)] {
                Alert alert("The file could not be opened."_sv, text.view());
                alert.add_button("OK"_sv);
                alert.show(*this, [](size_t) { });
            });
        }
        schedule_fit();
    }

    void zoom_by(float factor)
    {
        set_zoom(m_zoom * factor);
        m_fits = false;
    }

    void actual_size()
    {
        set_zoom(1);
        m_fits = false;
    }

    void zoom_to_fit()
    {
        m_fits = true;
        fit();
    }

private:
    // The picture's own shape, and at most most of the screen.
    [[nodiscard]] static IntRect start_frame(Bitmap const* bitmap)
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("viewer.startWidth"_sv));
        int height = static_cast<int>(t.metric("viewer.startHeight"_sv));
        auto screen = Platform::the().screen_size();
        if (bitmap && bitmap->width() > 0 && bitmap->height() > 0) {
            float toolbar = t.metric("window.toolbarUnified"_sv);
            float room_width = static_cast<float>(screen.width) * 0.7f;
            float room_height = static_cast<float>(screen.height) * 0.7f - toolbar;
            float zoom = min(1.f, min(room_width / static_cast<float>(bitmap->width()), room_height / static_cast<float>(bitmap->height())));
            width = max(static_cast<int>(t.metric("viewer.smallestWidth"_sv)), static_cast<int>(static_cast<float>(bitmap->width()) * zoom + 0.5f));
            height = static_cast<int>(static_cast<float>(bitmap->height()) * zoom + toolbar + 0.5f);
        }
        int offset = (s_windows_opened++ % 8) * 24;
        return { (screen.width - width) / 2 + offset, max(40, (screen.height - height) / 3) + offset, width, height };
    }

    void schedule_fit()
    {
        // Once the views have their sizes.
        Application::the().event_loop().deferred_invoke([this] {
            if (m_fits)
                fit();
        });
    }

    void fit()
    {
        auto const* bitmap = m_picture->bitmap();
        if (!bitmap)
            return;
        update_layout_now();
        float room_width = m_scroll->width();
        float room_height = m_scroll->height();
        if (room_width <= 0 || room_height <= 0)
            return;
        // Never bigger than itself, just to fill the window.
        float zoom = min(1.f, min(room_width / static_cast<float>(bitmap->width()), room_height / static_cast<float>(bitmap->height())));
        set_zoom(zoom);
    }

    void set_zoom(float zoom)
    {
        auto const& t = tokens();
        m_zoom = clamp(zoom, t.metric("viewer.smallestZoom"_sv), t.metric("viewer.largestZoom"_sv));
        m_picture->set_zoom(m_zoom);
        m_scroll->set_needs_layout();
        StringBuilder text;
        text.appendff("{}%", static_cast<int>(m_zoom * 100 + 0.5f));
        m_zoom_label->set_text(text.view());
    }

    ScrollView* m_scroll { nullptr };
    PictureView* m_picture { nullptr };
    Label* m_zoom_label { nullptr };
    float m_zoom { 1 };
    bool m_fits { true };
};

Vector<ViewerWindow*> s_windows;

void open_picture(StringView path)
{
    RefPtr<Bitmap> bitmap;
    String problem;
    auto bytes = Platform::the().read_file(path);
    if (bytes.is_error()) {
        auto code = bytes.error().code();
        problem = MUST(String::from_view(StringView(strerror(code), strlen(strerror(code)))));
    } else {
        auto decoded = Image::decode(bytes.value().span());
        if (decoded.is_error())
            problem = MUST(String::from_view("It is not a picture this version can read."_sv));
        else
            bitmap = decoded.release_value();
    }
    auto& window = Application::the().make_window<ViewerWindow>(path, move(bitmap), problem.view());
    s_windows.append(&window);
    window.on_closed = [&window] {
        s_windows.remove_all_matching([&](ViewerWindow* other) { return other == &window; });
    };
    window.show();
}

ViewerWindow* key_viewer()
{
    auto* key = Application::the().key_window();
    for (auto* window : s_windows) {
        if (window == key)
            return window;
    }
    return nullptr;
}

void choose_picture()
{
    auto* owner = Application::the().key_window();
    if (!owner) {
        // A panel hangs from a window: an empty one, until something is chosen.
        auto& empty = Application::the().make_window<ViewerWindow>(StringView {}, RefPtr<Bitmap> {}, StringView {});
        s_windows.append(&empty);
        empty.on_closed = [&empty] {
            s_windows.remove_all_matching([&](ViewerWindow* other) { return other == &empty; });
        };
        empty.show();
        owner = &empty;
    }
    auto home = Desktop::home_directory();
    FilePanel::run(*owner, FilePanel::Kind::Open, home.view(), {}, [](StringView chosen) {
        if (!chosen.is_empty())
            open_picture(chosen);
    });
}

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Viewer"_sv, "com.vulture.Viewer"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Viewer: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();

    auto menus = application->standard_menus();
    auto& file = *new Menu("File"_sv);
    file.add_with_shortcut("Open\xE2\x80\xA6"_sv, KeyO, "\xE2\x8C\x98O"_sv, [] { choose_picture(); });
    file.add_separator();
    file.add_with_shortcut("Close"_sv, KeyW, "\xE2\x8C\x98W"_sv, [] {
        if (auto* window = Application::the().key_window())
            window->close();
    });
    menus->items().insert_returning(1, "File"_sv).submenu = adopt_own(file);
    auto& view = *new Menu("View"_sv);
    view.add_with_shortcut("Actual Size"_sv, Key1 + 9, "\xE2\x8C\x98" "0"_sv, [] {
        if (auto* window = key_viewer())
            window->actual_size();
    });
    view.add_with_shortcut("Zoom to Fit"_sv, Key1 + 8, "\xE2\x8C\x98" "9"_sv, [] {
        if (auto* window = key_viewer())
            window->zoom_to_fit();
    });
    view.add_with_shortcut("Zoom In"_sv, KeyEqual, "\xE2\x8C\x98+"_sv, [] {
        if (auto* window = key_viewer())
            window->zoom_by(tokens().metric("viewer.zoomStep"_sv));
    });
    view.add_with_shortcut("Zoom Out"_sv, KeyMinus, "\xE2\x8C\x98-"_sv, [] {
        if (auto* window = key_viewer())
            window->zoom_by(1 / tokens().metric("viewer.zoomStep"_sv));
    });
    menus->items().insert_returning(3, "View"_sv).submenu = adopt_own(view);
    application->set_main_menu(move(menus));

    int opened = 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-')
            continue;
        open_picture(StringView(argv[i], strlen(argv[i])));
        ++opened;
    }
    if (opened == 0)
        choose_picture();
    printf("viewer: ready\n");
    fflush(stdout);
    return application->exec();
}
