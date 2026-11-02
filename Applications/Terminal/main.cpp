/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibUI/Application.h>
#include <LibUI/Motion.h>
#include <LibUI/ScrollView.h>
#include <LibVT/Terminal.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// A window with a shell in it: a pseudo-terminal, LibVT for what comes out of it, and
// keys going in. Cells sit on a fixed grid, one glyph each, so ligatures never join.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

size_t encode_utf8(u32 codepoint, char* out)
{
    if (codepoint < 0x80) {
        out[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 4;
}

// What Control and this key send. (Key codes run along the keyboard's rows.)
int control_byte(u16 code)
{
    switch (code) {
    case KeyQ: return 'q' & 0x1F;
    case KeyW: return 'w' & 0x1F;
    case KeyE: return 'e' & 0x1F;
    case KeyR: return 'r' & 0x1F;
    case KeyT: return 't' & 0x1F;
    case KeyY: return 'y' & 0x1F;
    case KeyU: return 'u' & 0x1F;
    case KeyI: return 'i' & 0x1F;
    case KeyO: return 'o' & 0x1F;
    case KeyP: return 'p' & 0x1F;
    case KeyA: return 'a' & 0x1F;
    case KeyS: return 's' & 0x1F;
    case KeyD: return 'd' & 0x1F;
    case KeyF: return 'f' & 0x1F;
    case KeyG: return 'g' & 0x1F;
    case KeyH: return 'h' & 0x1F;
    case KeyJ: return 'j' & 0x1F;
    case KeyK: return 'k' & 0x1F;
    case KeyL: return 'l' & 0x1F;
    case KeyZ: return 'z' & 0x1F;
    case KeyX: return 'x' & 0x1F;
    case KeyC: return 'c' & 0x1F;
    case KeyV: return 'v' & 0x1F;
    case KeyB: return 'b' & 0x1F;
    case KeyN: return 'n' & 0x1F;
    case KeyM: return 'm' & 0x1F;
    case KeySpace: return 0;
    case KeyMinus: return 0x1F;
    case 26: return 0x1B; // [
    case 27: return 0x1D; // ]
    case 43: return 0x1C; // backslash
    default: return -1;
    }
}

class TerminalView final : public View {
public:
    TerminalView()
        : m_terminal(tokens().metric("terminal.columns"_sv), tokens().metric("terminal.rows"_sv))
    {
        m_font.size = tokens().metric("terminal.fontSize"_sv);
        m_font.mono = true;
        load_palette();
        set_clips_children(true);
        m_scroller = &add<Scroller>(Axis::Vertical);
        m_scroller->on_scroll = [this](float position) {
            int most = static_cast<int>(m_terminal.scrollback_size());
            set_scroll_offset(most - static_cast<int>(position * static_cast<float>(most) + 0.5f));
        };
        m_terminal.on_reply = [this](StringView text) { send(text); };
        m_terminal.on_title_change = [this] {
            if (on_title_change)
                on_title_change();
        };
        m_terminal.on_bell = [] { };
    }

    ~TerminalView() override
    {
        stop_watching();
        if (m_retry_timer >= 0)
            Application::the().cancel_timer(m_retry_timer);
        if (m_master >= 0)
            close(m_master);
    }

    Function<void()> on_title_change;

    [[nodiscard]] FloatSize grid_size(int columns, int rows) const
    {
        float padding = tokens().metric("terminal.padding"_sv);
        return { static_cast<float>(columns) * cell_width() + 2 * padding, static_cast<float>(rows) * cell_height() + 2 * padding };
    }

    bool start_shell()
    {
        m_master = posix_openpt(O_RDWR | O_NOCTTY);
        if (m_master < 0 || grantpt(m_master) != 0 || unlockpt(m_master) != 0)
            return false;
        char const* name = ptsname(m_master);
        if (!name)
            return false;
        char slave_path[64];
        strlcpy(slave_path, name, sizeof(slave_path));
        update_window_size();

        struct passwd const* account = getpwuid(getuid());
        char shell[256] = "/bin/vsh";
        char home[256] = "/";
        if (account) {
            if (account->pw_shell && account->pw_shell[0])
                strlcpy(shell, account->pw_shell, sizeof(shell));
            if (account->pw_dir && account->pw_dir[0])
                strlcpy(home, account->pw_dir, sizeof(home));
        }

        m_child = fork();
        if (m_child < 0)
            return false;
        if (m_child == 0) {
            close(m_master);
            if (setsid() < 0)
                _exit(90);
            int slave = open(slave_path, O_RDWR);
            if (slave < 0)
                _exit(91);
            (void)ioctl(slave, TIOCSCTTY, 0);
            dup2(slave, 0);
            dup2(slave, 1);
            dup2(slave, 2);
            if (slave > 2)
                close(slave);
            setenv("TERM", "xterm-256color", 1);
            setenv("COLORTERM", "truecolor", 1);
            setenv("TERM_PROGRAM", "Terminal", 1);
            setenv("SHELL", shell, 1);
            setenv("HOME", home, 1);
            if (!getenv("PATH"))
                setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
            chdir(home);
            // A login shell, by the '-' in front of its name.
            char argument[258];
            char const* base = strrchr(shell, '/');
            snprintf(argument, sizeof(argument), "-%s", base ? base + 1 : shell);
            // As a fresh program has them, and not as this one does.
            signal(SIGPIPE, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            execl(shell, argument, nullptr);
            _exit(127);
        }
        int flags = fcntl(m_master, F_GETFL);
        fcntl(m_master, F_SETFL, flags | O_NONBLOCK);
        m_watch = Application::the().event_loop().watch(m_master, Core::EventLoop::Interest::Read, [this] { read_output(); });
        return true;
    }

    // "name — vsh — 80×24".
    [[nodiscard]] String window_title() const
    {
        StringBuilder builder;
        if (!m_terminal.title().is_empty()) {
            builder.append(m_terminal.title());
        } else {
            struct passwd const* account = getpwuid(getuid());
            builder.append(account ? StringView(account->pw_name, strlen(account->pw_name)) : "vulture"_sv);
            builder.append(" \xE2\x80\x94 vsh"_sv);
        }
        builder.appendff(" \xE2\x80\x94 {}\xC3\x97{}", m_terminal.columns(), m_terminal.rows());
        return MUST(builder.to_string());
    }

    void clear_scrollback()
    {
        m_terminal.feed("\x1b[3J"_sv);
        m_scroll_offset = 0;
        clear_selection();
        update_scroller();
        set_needs_display();
    }

    void change_font_size(float delta)
    {
        auto const& t = tokens();
        float size = delta == 0 ? t.metric("terminal.fontSize"_sv) : m_font.size + delta;
        m_font.size = clamp(size, t.metric("terminal.smallestFont"_sv), t.metric("terminal.largestFont"_sv));
        set_needs_layout();
        set_needs_display();
    }

    void layout() override
    {
        auto const& t = tokens();
        float padding = t.metric("terminal.padding"_sv);
        int columns = max(1, static_cast<int>((width() - 2 * padding) / cell_width()));
        int rows = max(1, static_cast<int>((height() - 2 * padding) / cell_height()));
        if (columns != m_terminal.columns() || rows != m_terminal.rows()) {
            m_terminal.resize(columns, rows);
            update_window_size();
            clamp_scroll_offset();
            if (on_title_change)
                on_title_change();
        }
        float thickness = m_scroller->intrinsic_size().width;
        m_scroller->set_frame({ width() - thickness, 0, thickness, height() });
        update_scroller();
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = tokens();
        graphics.fill({ 0, 0, width(), height() }, m_background);
        float padding = t.metric("terminal.padding"_sv);
        float cw = cell_width();
        float ch = cell_height();
        float ascent = Appearance::ascent(m_font);
        float baseline_offset = (ch - (ascent + Appearance::descent(m_font))) / 2 + ascent;
        FontSpec bold = m_font;
        bold.weight = FontWeight::Bold;
        auto selection = t.color("terminal.selection"_sv);
        Line selection_from, selection_to;
        bool selecting = selection_range(selection_from, selection_to);

        size_t first_absolute = m_terminal.scrollback_size() - static_cast<size_t>(m_scroll_offset);
        for (int row = 0; row < m_terminal.rows(); ++row) {
            size_t absolute = first_absolute + static_cast<size_t>(row);
            auto const& line = line_at(absolute);
            int count = min(static_cast<int>(line.size()), m_terminal.columns());
            float y = padding + static_cast<float>(row) * ch;
            // Backgrounds first: a glyph may reach out of its cell.
            for (int column = 0; column < count; ++column) {
                auto const& attributes = line[column].attributes;
                auto background = cell_background(attributes);
                if (background == m_background)
                    continue;
                // Together, or the seams between cells show at fractional widths.
                int end = column + 1;
                while (end < count && cell_background(line[end].attributes) == background)
                    ++end;
                graphics.fill(graphics.snapped({ padding + static_cast<float>(column) * cw, y, static_cast<float>(end - column) * cw, ch }), background);
                column = end - 1;
            }
            if (selecting && absolute >= selection_from.absolute && absolute <= selection_to.absolute) {
                int from = absolute == selection_from.absolute ? selection_from.column : 0;
                int to = absolute == selection_to.absolute ? selection_to.column : m_terminal.columns();
                graphics.fill(graphics.snapped({ padding + static_cast<float>(from) * cw, y, static_cast<float>(to - from) * cw, ch }), selection);
            }
            float baseline = y + baseline_offset;
            for (int column = 0; column < count; ++column) {
                auto const& cell = line[column];
                auto const& attributes = cell.attributes;
                float x = padding + static_cast<float>(column) * cw;
                auto color = cell_foreground(attributes);
                if (cell.codepoint != ' ') {
                    char encoded[4];
                    size_t length = encode_utf8(cell.codepoint, encoded);
                    graphics.draw_text(StringView(encoded, length), attributes.bold ? bold : m_font, { x, baseline }, color);
                }
                if (attributes.underline)
                    graphics.fill(graphics.snapped({ x, baseline + 1, cw, 1 }), color);
                if (attributes.strikethrough)
                    graphics.fill(graphics.snapped({ x, baseline - ascent * 0.35f, cw, 1 }), color);
            }
        }

        // The cursor: solid in the window with the keyboard, an outline otherwise.
        size_t cursor_absolute = m_terminal.scrollback_size() + static_cast<size_t>(m_terminal.cursor_row());
        if (m_terminal.cursor_visible() && m_master_open && cursor_absolute >= first_absolute && cursor_absolute < first_absolute + static_cast<size_t>(m_terminal.rows())) {
            float x = padding + static_cast<float>(m_terminal.cursor_column()) * cw;
            float y = padding + static_cast<float>(cursor_absolute - first_absolute) * ch;
            auto rect = graphics.snapped({ x, y, cw, ch });
            bool focused = is_first_responder() && window() && window()->is_key();
            if (focused) {
                graphics.fill(rect, t.color("terminal.cursor"_sv));
                auto const& cell = m_terminal.cell(m_terminal.cursor_row(), m_terminal.cursor_column());
                if (cell.codepoint != ' ') {
                    char encoded[4];
                    size_t length = encode_utf8(cell.codepoint, encoded);
                    graphics.draw_text(StringView(encoded, length), cell.attributes.bold ? bold : m_font, { x, y + baseline_offset }, t.color("terminal.cursorText"_sv));
                }
            } else {
                graphics.stroke_rounded(rect, 0, 1, t.color("terminal.cursor"_sv));
            }
        }
    }

    void appearance_changed() override
    {
        load_palette();
        set_needs_display();
    }

    [[nodiscard]] WS::CursorShape cursor() const override { return WS::CursorShape::IBeam; }
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    void did_become_first_responder() override { set_needs_display(); }
    void did_resign_first_responder() override { set_needs_display(); }

    bool key_down(KeyEvent const& event) override
    {
        if (event.has_command())
            return false;
        bool application = m_terminal.application_cursor_keys();
        switch (event.code) {
        case KeyUp:
            send_key(application ? "\x1bOA"_sv : "\x1b[A"_sv);
            return true;
        case KeyDown:
            send_key(application ? "\x1bOB"_sv : "\x1b[B"_sv);
            return true;
        case KeyRight:
            send_key(application ? "\x1bOC"_sv : "\x1b[C"_sv);
            return true;
        case KeyLeft:
            send_key(application ? "\x1bOD"_sv : "\x1b[D"_sv);
            return true;
        case KeyHome:
            if (event.has_shift())
                set_scroll_offset(static_cast<int>(m_terminal.scrollback_size()));
            else
                send_key(application ? "\x1bOH"_sv : "\x1b[H"_sv);
            return true;
        case KeyEnd:
            if (event.has_shift())
                set_scroll_offset(0);
            else
                send_key(application ? "\x1bOF"_sv : "\x1b[F"_sv);
            return true;
        case KeyPageUp:
            if (event.has_shift())
                set_scroll_offset(m_scroll_offset + m_terminal.rows() - 1);
            else
                send_key("\x1b[5~"_sv);
            return true;
        case KeyPageDown:
            if (event.has_shift())
                set_scroll_offset(m_scroll_offset - (m_terminal.rows() - 1));
            else
                send_key("\x1b[6~"_sv);
            return true;
        case KeyDelete:
            send_key("\x1b[3~"_sv);
            return true;
        case KeyEnter:
            send_key("\r"_sv);
            return true;
        case KeyBackspace:
            send_key("\x7f"_sv);
            return true;
        case KeyTab:
            send_key(event.has_shift() ? "\x1b[Z"_sv : "\t"_sv);
            return true;
        case KeyEscape:
            send_key("\x1b"_sv);
            return true;
        default:
            break;
        }
        if (event.has_control()) {
            int byte = control_byte(event.code);
            if (byte < 0)
                return false;
            char c = static_cast<char>(byte);
            send_key(StringView(&c, 1));
            return true;
        }
        if (event.text.is_empty())
            return false;
        send_key(event.text);
        return true;
    }

    bool perform_command(StringView command) override
    {
        if (command == "copy"_sv) {
            auto text = selected_text();
            if (text.is_empty())
                return false;
            Platform::the().copy_text(text.view());
            return true;
        }
        if (command == "paste"_sv) {
            auto text = Platform::the().paste_text();
            if (text.is_error())
                return true;
            paste(text.value().view());
            return true;
        }
        if (command == "selectAll"_sv) {
            m_anchor = { 0, 0 };
            m_extent = { m_terminal.scrollback_size() + static_cast<size_t>(m_terminal.rows()) - 1, m_terminal.columns() };
            m_has_selection = true;
            set_needs_display();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool can_perform_command(StringView command) const override
    {
        if (command == "copy"_sv)
            return m_has_selection;
        return command == "paste"_sv || command == "selectAll"_sv;
    }

    bool mouse_down(MouseEvent const& event) override
    {
        (void)become_first_responder();
        auto where = line_at_point(event.position);
        m_select_unit = event.click_count;
        m_anchor = where;
        m_extent = where;
        m_has_selection = false;
        if (m_select_unit >= 2) {
            extend_to_units();
            m_has_selection = true;
        }
        m_selecting = true;
        set_needs_display();
        return true;
    }

    void mouse_dragged(MouseEvent const& event) override
    {
        if (!m_selecting)
            return;
        m_extent = line_at_point(event.position);
        m_has_selection = !(m_extent.absolute == m_anchor.absolute && m_extent.column == m_anchor.column);
        if (m_select_unit >= 2) {
            extend_to_units();
            m_has_selection = true;
        }
        set_needs_display();
    }

    void mouse_up(MouseEvent const&) override { m_selecting = false; }

    bool scroll_wheel(ScrollEvent const& event) override
    {
        if (m_terminal.using_alternate_screen())
            return false;
        m_wheel_remainder += event.delta_y;
        int lines = static_cast<int>(m_wheel_remainder / cell_height());
        if (lines == 0)
            return true;
        m_wheel_remainder -= static_cast<float>(lines) * cell_height();
        set_scroll_offset(m_scroll_offset - lines);
        m_scroller->flash();
        return true;
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::TextArea; }
    [[nodiscard]] String accessibility_value() const override
    {
        StringBuilder builder;
        for (int row = 0; row < m_terminal.rows(); ++row) {
            builder.append(m_terminal.row_text(row).view());
            builder.append("\n"_sv);
        }
        return MUST(builder.to_string());
    }

private:
    struct Line {
        size_t absolute { 0 };
        int column { 0 };
    };

    [[nodiscard]] float cell_width() const { return Appearance::text_width(m_font, "M"_sv); }
    [[nodiscard]] float cell_height() const
    {
        // To the pixel, for even rows.
        float height = Appearance::ascent(m_font) + Appearance::descent(m_font);
        return static_cast<float>(static_cast<int>(height + 0.999f));
    }

    void load_palette()
    {
        auto const& t = tokens();
        m_background = t.color("terminal.background"_sv);
        m_foreground = t.color("terminal.foreground"_sv);
        m_bold = t.color("terminal.bold"_sv);
        for (int i = 0; i < 16; ++i) {
            StringBuilder name;
            name.appendff("terminal.ansi{}", i);
            m_palette[i] = t.color(name.view());
        }
    }

    [[nodiscard]] Color color_of(VT::Color const& color, Color fallback, bool bold) const
    {
        switch (color.kind) {
        case VT::Color::Kind::Default:
            return fallback;
        case VT::Color::Kind::RGB:
            return { color.red, color.green, color.blue, 255 };
        case VT::Color::Kind::Indexed:
            break;
        }
        int index = color.index;
        if (index < 16) {
            // The old way of saying bright.
            if (bold && index < 8)
                index += 8;
            return m_palette[index];
        }
        if (index < 232) {
            // The cube: 0, 95, 135, 175, 215, 255 along each side.
            int cube = index - 16;
            auto side = [](int level) { return static_cast<u8>(level == 0 ? 0 : 55 + level * 40); };
            return { side(cube / 36), side((cube / 6) % 6), side(cube % 6), 255 };
        }
        auto gray = static_cast<u8>(8 + (index - 232) * 10);
        return { gray, gray, gray, 255 };
    }

    [[nodiscard]] Color cell_background(VT::Attributes const& attributes) const
    {
        if (attributes.inverse)
            return color_of(attributes.foreground, m_foreground, false);
        return color_of(attributes.background, m_background, false);
    }

    [[nodiscard]] Color cell_foreground(VT::Attributes const& attributes) const
    {
        Color color = attributes.inverse
            ? color_of(attributes.background, m_background, false)
            : color_of(attributes.foreground, attributes.bold ? m_bold : m_foreground, attributes.bold);
        if (attributes.faint)
            color.alpha = static_cast<u8>(color.alpha / 2);
        return color;
    }

    [[nodiscard]] Vector<VT::Cell> const& line_at(size_t absolute) const
    {
        size_t scrollback = m_terminal.scrollback_size();
        if (absolute < scrollback)
            return m_terminal.scrollback_line(absolute);
        size_t row = absolute - scrollback;
        return m_terminal.screen_line(static_cast<int>(min(row, static_cast<size_t>(m_terminal.rows() - 1))));
    }

    [[nodiscard]] Line line_at_point(FloatPoint point) const
    {
        float padding = tokens().metric("terminal.padding"_sv);
        int row = clamp(static_cast<int>((point.y - padding) / cell_height()), 0, m_terminal.rows() - 1);
        // Halfway across a cell, the boundary is the next one along.
        int column = clamp(static_cast<int>((point.x - padding) / cell_width() + 0.5f), 0, m_terminal.columns());
        size_t first = m_terminal.scrollback_size() - static_cast<size_t>(m_scroll_offset);
        return { first + static_cast<size_t>(row), column };
    }

    // In text order, however it was dragged.
    bool selection_range(Line& from, Line& to) const
    {
        if (!m_has_selection)
            return false;
        bool anchor_first = m_anchor.absolute < m_extent.absolute || (m_anchor.absolute == m_extent.absolute && m_anchor.column <= m_extent.column);
        from = anchor_first ? m_anchor : m_extent;
        to = anchor_first ? m_extent : m_anchor;
        return true;
    }

    // Double click: the word; triple: the line.
    void extend_to_units()
    {
        Line from, to;
        bool anchor_first = m_anchor.absolute < m_extent.absolute || (m_anchor.absolute == m_extent.absolute && m_anchor.column <= m_extent.column);
        from = anchor_first ? m_anchor : m_extent;
        to = anchor_first ? m_extent : m_anchor;
        if (m_select_unit >= 3) {
            from.column = 0;
            to.column = m_terminal.columns();
        } else {
            auto is_word = [](u32 c) { return c != ' ' && c != '\t'; };
            auto const& first_line = line_at(from.absolute);
            int count = static_cast<int>(first_line.size());
            while (from.column > 0 && from.column <= count && is_word(first_line[from.column - 1].codepoint))
                --from.column;
            auto const& last_line = line_at(to.absolute);
            count = static_cast<int>(last_line.size());
            while (to.column < count && is_word(last_line[to.column].codepoint))
                ++to.column;
        }
        m_anchor = from;
        m_extent = to;
    }

    [[nodiscard]] String selected_text() const
    {
        Line from, to;
        if (!selection_range(from, to))
            return {};
        StringBuilder builder;
        for (size_t absolute = from.absolute; absolute <= to.absolute; ++absolute) {
            auto const& line = line_at(absolute);
            int start = absolute == from.absolute ? from.column : 0;
            int end = absolute == to.absolute ? to.column : static_cast<int>(line.size());
            end = min(end, static_cast<int>(line.size()));
            // Spaces to the end of a line are not text.
            while (end > start && line[end - 1].codepoint == ' ')
                --end;
            for (int column = start; column < end; ++column) {
                char encoded[4];
                size_t length = encode_utf8(line[column].codepoint, encoded);
                builder.append(StringView(encoded, length));
            }
            if (absolute != to.absolute)
                builder.append("\n"_sv);
        }
        return MUST(builder.to_string());
    }

    void clear_selection()
    {
        m_has_selection = false;
        m_selecting = false;
    }

    void set_scroll_offset(int offset)
    {
        int most = m_terminal.using_alternate_screen() ? 0 : static_cast<int>(m_terminal.scrollback_size());
        offset = clamp(offset, 0, most);
        if (offset == m_scroll_offset)
            return;
        m_scroll_offset = offset;
        update_scroller();
        set_needs_display();
    }

    void clamp_scroll_offset()
    {
        m_scroll_offset = clamp(m_scroll_offset, 0, static_cast<int>(m_terminal.scrollback_size()));
    }

    void update_scroller()
    {
        float total = static_cast<float>(m_terminal.scrollback_size()) + static_cast<float>(m_terminal.rows());
        float most = static_cast<float>(m_terminal.scrollback_size());
        float position = most > 0 ? (most - static_cast<float>(m_scroll_offset)) / most : 0;
        m_scroller->set_knob(position, static_cast<float>(m_terminal.rows()) / total);
    }

    void update_window_size()
    {
        if (m_master < 0)
            return;
        struct winsize size { };
        size.ws_row = static_cast<unsigned short>(m_terminal.rows());
        size.ws_col = static_cast<unsigned short>(m_terminal.columns());
        (void)ioctl(m_master, TIOCSWINSZ, &size);
    }

    void stop_watching()
    {
        if (m_watch >= 0)
            Application::the().event_loop().unwatch(m_watch);
        m_watch = -1;
    }

    void read_output()
    {
        // A screenful at a time, to stay answerable under a flood.
        static char buffer[65536];
        ssize_t got = read(m_master, buffer, sizeof(buffer));
        if (got > 0) {
            size_t scrollback_before = m_terminal.scrollback_size();
            m_terminal.feed(ReadonlyBytes(reinterpret_cast<u8 const*>(buffer), static_cast<size_t>(got)));
            // Reading back: what is shown stays where it is.
            if (m_scroll_offset > 0) {
                m_scroll_offset += static_cast<int>(m_terminal.scrollback_size() - scrollback_before);
                clamp_scroll_offset();
            }
            update_scroller();
            set_needs_display();
            return;
        }
        if (got < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        // The shell has gone, and everything it left behind.
        stop_watching();
        m_master_open = false;
        m_terminal.feed("\r\n\r\n[Process completed]"_sv);
        set_needs_display();
    }

    void send_key(StringView bytes)
    {
        // Typing comes back to the bottom, and what was selected is done with.
        set_scroll_offset(0);
        if (m_has_selection) {
            clear_selection();
            set_needs_display();
        }
        send(bytes);
    }

    void paste(StringView text)
    {
        // Lines end as Return does; and a program that asked knows it is a paste.
        StringBuilder builder;
        if (m_terminal.bracketed_paste())
            builder.append("\x1b[200~"_sv);
        for (size_t i = 0; i < text.length(); ++i) {
            char c = text[i];
            if (c == '\n')
                c = '\r';
            builder.append(StringView(&c, 1));
        }
        if (m_terminal.bracketed_paste())
            builder.append("\x1b[201~"_sv);
        send_key(builder.view());
    }

    void send(StringView bytes)
    {
        if (!m_master_open || m_master < 0)
            return;
        for (size_t i = 0; i < bytes.length(); ++i)
            m_pending.append(static_cast<u8>(bytes[i]));
        flush_pending();
    }

    void flush_pending()
    {
        size_t done = 0;
        while (done < m_pending.size()) {
            ssize_t wrote = write(m_master, m_pending.data() + done, m_pending.size() - done);
            if (wrote <= 0)
                break;
            done += static_cast<size_t>(wrote);
        }
        if (done == m_pending.size()) {
            m_pending.clear();
            return;
        }
        Vector<u8> rest;
        for (size_t i = done; i < m_pending.size(); ++i)
            rest.append(m_pending[i]);
        m_pending = move(rest);
        // The program is not reading: try again in a moment.
        if (m_retry_timer < 0) {
            m_retry_timer = Application::the().add_timer(20, false, [this] {
                m_retry_timer = -1;
                flush_pending();
            });
        }
    }

    VT::Terminal m_terminal;
    FontSpec m_font;
    Scroller* m_scroller { nullptr };
    Color m_background;
    Color m_foreground;
    Color m_bold;
    Color m_palette[16];
    int m_master { -1 };
    bool m_master_open { true };
    pid_t m_child { -1 };
    int m_watch { -1 };
    int m_retry_timer { -1 };
    Vector<u8> m_pending;
    int m_scroll_offset { 0 };
    float m_wheel_remainder { 0 };
    Line m_anchor;
    Line m_extent;
    bool m_has_selection { false };
    bool m_selecting { false };
    int m_select_unit { 1 };
};

int s_window_count = 0;

class TerminalWindow final : public UI::Window {
public:
    TerminalWindow()
        : UI::Window("Terminal"_sv, IntRect { 0, 0, 640, 480 })
    {
        auto& view = set_content<TerminalView>();
        m_view = &view;
        auto const& t = tokens();
        auto size = view.grid_size(t.metric("terminal.columns"_sv), t.metric("terminal.rows"_sv));
        auto screen = Platform::the().screen_size();
        int width = static_cast<int>(size.width + 0.999f);
        int height = static_cast<int>(size.height + title_bar_height() + 0.999f);
        // Each from the last, down and across.
        int offset = (s_window_count++ % 8) * 24;
        set_frame({ (screen.width - width) / 2 + offset, max(40, (screen.height - height) / 3) + offset, width, height });
        view.on_title_change = [this] { update_title(); };
        update_title();
        make_first_responder(&view);
        on_key_change = [this](bool) { m_view->set_needs_display(); };
        if (!view.start_shell())
            fprintf(stderr, "Terminal: cannot start a shell\n");
    }

    [[nodiscard]] TerminalView& view() { return *m_view; }

private:
    void update_title() { set_title(m_view->window_title().view()); }

    TerminalView* m_view { nullptr };
};

TerminalView* key_view()
{
    auto* window = Application::the().key_window();
    if (!window)
        return nullptr;
    auto* responder = window->first_responder();
    auto* view = responder ? responder->as_view() : nullptr;
    // There is nothing else in these windows that takes keys.
    return static_cast<TerminalView*>(view);
}

void new_window()
{
    Application::the().make_window<TerminalWindow>().show();
}

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    // Shells are not waited for: the terminal learns they went from the master.
    signal(SIGCHLD, SIG_IGN);
    auto application_or_error = Application::create("Terminal"_sv, "com.vulture.Terminal"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Terminal: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();

    auto menus = application->standard_menus();
    auto& shell = *new Menu("Shell"_sv);
    shell.add_with_shortcut("New Window"_sv, KeyN, "\xE2\x8C\x98N"_sv, [] { new_window(); });
    shell.add_separator();
    shell.add_with_shortcut("Close Window"_sv, KeyW, "\xE2\x8C\x98W"_sv, [] {
        if (auto* window = Application::the().key_window())
            window->close();
    });
    menus->items().insert_returning(1, "Shell"_sv).submenu = adopt_own(shell);
    auto& view = *new Menu("View"_sv);
    view.add_with_shortcut("Bigger"_sv, KeyEqual, "\xE2\x8C\x98+"_sv, [] {
        if (auto* terminal = key_view())
            terminal->change_font_size(1);
    });
    view.add_with_shortcut("Smaller"_sv, KeyMinus, "\xE2\x8C\x98-"_sv, [] {
        if (auto* terminal = key_view())
            terminal->change_font_size(-1);
    });
    view.add_with_shortcut("Default Size"_sv, Key1 + 9, "\xE2\x8C\x98" "0"_sv, [] {
        if (auto* terminal = key_view())
            terminal->change_font_size(0);
    });
    view.add_separator();
    view.add_with_shortcut("Clear Scrollback"_sv, KeyK, "\xE2\x8C\x98K"_sv, [] {
        if (auto* terminal = key_view())
            terminal->clear_scrollback();
    });
    menus->items().insert_returning(3, "View"_sv).submenu = adopt_own(view);
    application->set_main_menu(move(menus));

    new_window();
    printf("terminal: ready\n");
    fflush(stdout);
    return application->exec();
}
