/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Server.h"
#include <LibAnimation/Curve.h>
#include <LibBase/Math.h>
#include <LibGesture/Recognizer.h>
#include <time.h>

// Where windows are, on the large scale. Each display has its own row of Spaces, one showing. A window that's full screen has a Space to
// itself, or to itself and one other (Split View), which goes when it does. Overview looks at all of them from above; Stage shows one
// application's windows at a time; and the wallpaper can be uncovered with a click.

namespace WindowServer {

using namespace Gfx;
namespace Math = Base::Math;

namespace {

double now_seconds()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1'000'000'000.0;
}

float ease_out(float t)
{
    float inverse = 1 - t;
    return 1 - inverse * inverse * inverse;
}

FloatRect between(FloatRect const& a, FloatRect const& b, float t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.width + (b.width - a.width) * t, a.height + (b.height - a.height) * t };
}

String text_of(StringView view)
{
    StringBuilder builder;
    builder.append(view);
    return MUST(builder.to_string());
}

// Split View: what's between its sides, and how small either can be.
constexpr int divider_width = 6;
constexpr int smallest_side = 320;
// A window out of the way of the desktop leaves this much of itself showing.
constexpr int desktop_sliver = 32;
constexpr float reveal_seconds = 0.2f;
constexpr float desktop_reveal_seconds = 0.35f;
// How long the pointer stays in a corner for it to count.
constexpr u64 hot_corner_milliseconds = 250;
// Stage's strip, down the left.
constexpr float stage_strip_left = 14;
constexpr float stage_strip_width = 132;
constexpr float stage_strip_gap = 18;
constexpr float stage_strip_tallest = 110;
// How far a press goes before it's a drag.
constexpr int drag_slop = 4;
// The row of Spaces, under the tile: its name.
constexpr float tile_top = 8;
constexpr float tile_label_height = 18;

}

// ---- The model

void Server::sync_display_spaces()
{
    auto const& displays = m_screen.displays();
    // A display that's new has a Space.
    for (auto const& display : displays) {
        bool found = false;
        for (auto const& one : m_display_spaces)
            found = found || one.output == display.output;
        if (found)
            continue;
        DisplaySpaces one;
        one.output = display.output;
        Space space;
        space.id = m_next_space_id++;
        one.spaces.append(move(space));
        m_display_spaces.append(move(one));
    }
    // One that's gone leaves its Spaces to the main display, and what was full screen there comes back.
    for (size_t i = m_display_spaces.size(); i > 0; --i) {
        bool found = false;
        for (auto const& display : displays)
            found = found || m_display_spaces[i - 1].output == display.output;
        if (found)
            continue;
        if (m_sliding && m_slide_output == m_display_spaces[i - 1].output)
            finish_slide();
        Vector<ServerWindow*> full;
        for (auto const& space : m_display_spaces[i - 1].spaces) {
            if (space.full)
                full.append(space.full);
            if (space.beside)
                full.append(space.beside);
        }
        for (auto* window : full)
            come_back_from_full_screen(*window);
        auto gone = move(m_display_spaces[i - 1]);
        m_display_spaces.remove(i - 1);
        auto& main = spaces_of(m_screen.main_display());
        for (auto& space : gone.spaces) {
            if (!space.full && !space.leaving)
                main.spaces.append(move(space));
        }
    }
}

DisplaySpaces& Server::spaces_of(Display const& display)
{
    for (auto& one : m_display_spaces) {
        if (one.output == display.output)
            return one;
    }
    VERIFY_NOT_REACHED();
}

DisplaySpaces const& Server::spaces_of_const(Display const& display) const
{
    for (auto const& one : m_display_spaces) {
        if (one.output == display.output)
            return one;
    }
    VERIFY_NOT_REACHED();
}

DisplaySpaces& Server::focused_spaces()
{
    return spaces_of(m_screen.display_at(m_pointer));
}

Display const& Server::display_of_spaces(DisplaySpaces const& where) const
{
    for (auto const& display : m_screen.displays()) {
        if (display.output == where.output)
            return display;
    }
    return m_screen.main_display();
}

Space* Server::space_with_id(int id, DisplaySpaces** where)
{
    for (auto& one : m_display_spaces) {
        for (auto& space : one.spaces) {
            if (space.id != id)
                continue;
            if (where)
                *where = &one;
            return &space;
        }
    }
    return nullptr;
}

bool Server::space_is_showing(int id) const
{
    for (auto const& one : m_display_spaces) {
        if (one.spaces[static_cast<size_t>(one.current)].id == id)
            return true;
    }
    return false;
}

bool Server::space_is_sliding(int id) const
{
    if (!m_sliding)
        return false;
    for (auto const& one : m_display_spaces) {
        if (one.output != m_slide_output)
            continue;
        return one.spaces[static_cast<size_t>(one.current)].id == id
            || (m_slide_from < static_cast<int>(one.spaces.size()) && one.spaces[static_cast<size_t>(m_slide_from)].id == id);
    }
    return false;
}

int Server::index_of_space(int id) const
{
    for (auto const& one : m_display_spaces) {
        for (size_t i = 0; i < one.spaces.size(); ++i) {
            if (one.spaces[i].id == id)
                return static_cast<int>(i);
        }
    }
    return 0;
}

int Server::space_for_new_window(ServerWindow const& window)
{
    auto const& where = spaces_of(m_screen.display_at(window.frame.center()));
    auto const& showing = where.spaces[static_cast<size_t>(where.current)];
    if (!showing.full)
        return showing.id;
    // Not on someone else's: on the Space it came from, or the nearest that isn't one of those.
    for (int i = where.current; i >= 0; --i) {
        if (!where.spaces[static_cast<size_t>(i)].full)
            return where.spaces[static_cast<size_t>(i)].id;
    }
    return where.spaces[0].id;
}

void Server::settle_on_display(ServerWindow& window)
{
    if (window.is_on_every_space() || window.full_screen)
        return;
    auto const& where = spaces_of(m_screen.display_at(window.frame.center()));
    DisplaySpaces* now = nullptr;
    if (space_with_id(window.space, &now) && now == &where)
        return;
    window.space = space_for_new_window(window);
    update_visibility(window);
}

void Server::show_space_of(ServerWindow const& window)
{
    if (window.is_on_every_space() || space_is_showing(window.space))
        return;
    DisplaySpaces* where = nullptr;
    if (space_with_id(window.space, &where))
        go_to_space(*where, index_of_space(window.space), true);
}

void Server::switch_space(IPC::Sender const&, i32 index)
{
    if (index < 0)
        return;
    auto& where = focused_spaces();
    while (index >= static_cast<int>(where.spaces.size())) {
        Space space;
        space.id = m_next_space_id++;
        where.spaces.append(move(space));
    }
    go_to_space(where, index, true);
}

void Server::go_to_space(DisplaySpaces& where, int index, bool animated)
{
    if (index == where.current || index < 0 || index >= static_cast<int>(where.spaces.size()))
        return;
    if (m_sliding)
        finish_slide();
    reveal_desktop(false, false);
    m_slide_output = where.output;
    m_slide_from = where.current;
    where.current = index;
    animated = animated && !m_reduce_motion;
    m_sliding = animated;
    m_slide_started = now_seconds();
    m_slide_start_travel = 0;
    if (animated)
        start_animating();
    update_all_visibility();
    // The new Space's frontmost has the keyboard, or nothing does.
    if (!animated)
        finish_slide();
    refresh_lasting_overlays();
    shell_state_changed();
    schedule_frame();
}

void Server::finish_slide()
{
    m_sliding = false;
    m_swiping = false;
    for (auto* window : m_windows) {
        window->slide_x = 0;
        window->slide_clip = {};
    }
    remove_leaving_spaces();
    update_all_visibility();
    if (m_active && !m_active->is_on_every_space() && !space_is_showing(m_active->space))
        activate(nullptr);
    if (!m_active)
        activate_frontmost();
    refresh_lasting_overlays();
    m_compositor.invalidate_all();
}

bool Server::advance_slide(double now)
{
    if (!m_sliding)
        return false;
    auto t = static_cast<float>((now - m_slide_started) / static_cast<double>(m_theme.space_slide_seconds));
    if (!m_swiping && t >= 1) {
        finish_slide();
        return false;
    }
    DisplaySpaces* where = nullptr;
    for (auto& one : m_display_spaces) {
        if (one.output == m_slide_output)
            where = &one;
    }
    if (!where)
        return false;
    auto const& display = display_of_spaces(*where);
    int const scale = m_screen.scale();
    int const width = display.points.width * scale;
    int const direction = where->current > m_slide_from ? 1 : -1;
    int const to_id = where->spaces[static_cast<size_t>(where->current)].id;
    int const from_id = where->spaces[static_cast<size_t>(m_slide_from)].id;
    auto const origin = m_screen.origin();
    // With more than one, what's sliding stays on its own.
    IntRect clip;
    if (m_screen.displays().size() > 1)
        clip = { (display.points.x + origin.x) * scale, (display.points.y + origin.y) * scale, width, display.points.height * scale };
    // With the fingers on it, it's where they are. Let go, it goes on from there.
    float const travel = m_swiping ? m_swipe_travel : m_slide_start_travel + (1 - m_slide_start_travel) * ease_out(clamp(t, 0.0f, 1.0f));
    for (auto* window : m_windows) {
        if (window->is_on_every_space() || (window->space != to_id && window->space != from_id))
            continue;
        // Those leaving slide off; those coming, in from the other side.
        float const from_new = window->space == to_id ? 1 - travel : -travel;
        window->slide_x = static_cast<int>(static_cast<float>(direction * width) * from_new);
        window->slide_clip = clip;
    }
    m_compositor.invalidate_all();
    return true;
}

void Server::remove_leaving_spaces()
{
    for (auto& one : m_display_spaces) {
        for (size_t i = one.spaces.size(); i > 0; --i) {
            if (!one.spaces[i - 1].leaving || static_cast<int>(i - 1) == one.current)
                continue;
            one.spaces.remove(i - 1);
            if (static_cast<int>(i - 1) < one.current)
                --one.current;
        }
    }
}

void Server::swipe_changed(float across, float speed, bool ended)
{
    auto& where = focused_spaces();
    float const width = static_cast<float>(display_of_spaces(where).points.width);
    // Fingers going left bring on what's to the right.
    float const towards_next = -across / width;
    if (!m_swiping && !ended) {
        if (m_overview || m_sliding || towards_next == 0)
            return;
        int const target = where.current + (towards_next > 0 ? 1 : -1);
        // Nothing that way: it stays where it is.
        if (target < 0 || target >= static_cast<int>(where.spaces.size()))
            return;
        reveal_desktop(false, false);
        m_slide_output = where.output;
        m_slide_from = where.current;
        where.current = target;
        m_sliding = true;
        m_swiping = true;
        update_all_visibility();
        start_animating();
    }
    if (!m_swiping)
        return;
    bool const towards_higher = where.current > m_slide_from;
    float const travel = clamp(towards_higher ? towards_next : -towards_next, 0.0f, 1.0f);
    m_swipe_travel = travel;
    if (!ended)
        return;
    m_swiping = false;
    if (!::Gesture::swipe_carries_on(travel, towards_higher ? -speed : speed)) {
        int const was_going_to = where.current;
        where.current = m_slide_from;
        m_slide_from = was_going_to;
        m_swipe_travel = 1 - travel;
    }
    m_slide_start_travel = m_swipe_travel;
    m_slide_started = now_seconds();
    shell_state_changed();
}

// ---- Full screen, and Split View

int Server::hidden_top_of(ServerWindow const& window) const
{
    // A toolbar stays, as it's what the window's controls are on.
    return window.is_unified() ? 0 : title_bar_height(window);
}

void Server::layout_space(Space& space, Display const& display)
{
    if (!space.full)
        return;
    auto const area = display.points;
    float const reveal = m_reveal_output == display.output ? m_reveal : 0;
    auto put = [&](ServerWindow& window, int x, int width) {
        int const top = hidden_top_of(window);
        int const pushed = static_cast<int>(static_cast<float>(m_work_top + top) * reveal);
        IntRect const frame { x, area.y - top + pushed, width, area.height + top };
        if (frame.size() != window.frame.size() || window.resize_waiting) {
            resize_live(window, frame);
        } else if (frame != window.frame) {
            move_window_to(window, frame);
            send_moved(window);
        }
    };
    if (!space.beside && !space.choosing) {
        put(*space.full, area.x, area.width);
        return;
    }
    int const left_width = clamp(static_cast<int>(static_cast<float>(area.width - divider_width) * space.divider), smallest_side, area.width - divider_width - smallest_side);
    auto* left = space.full_on_left ? space.full : space.beside;
    auto* right = space.full_on_left ? space.beside : space.full;
    if (left)
        put(*left, area.x, left_width);
    if (right)
        put(*right, area.x + left_width + divider_width, area.width - left_width - divider_width);
}

void Server::enter_full_screen(ServerWindow& window)
{
    if (window.full_screen || !window.is_resizable() || !window.is_titled() || window.is_on_every_space())
        return;
    auto const& display = m_screen.display_at(window.frame.center());
    auto& where = spaces_of(display);
    window.full_screen_restore = window.frame;
    window.full_screen_from = window.space;
    window.full_screen = true;
    Space space;
    space.id = m_next_space_id++;
    space.full = &window;
    int const index = where.current + 1;
    where.spaces.insert(static_cast<size_t>(index), move(space));
    window.space = where.spaces[static_cast<size_t>(index)].id;
    layout_space(where.spaces[static_cast<size_t>(index)], display);
    go_to_space(where, index, true);
    activate(&window);
    notify_full_screen(window);
    shell_state_changed();
}

void Server::enter_split_view(ServerWindow& window, bool on_left)
{
    enter_full_screen(window);
    DisplaySpaces* where = nullptr;
    auto* space = space_with_id(window.space, &where);
    if (!space || space->beside)
        return;
    space->choosing = true;
    space->full_on_left = on_left;
    layout_space(*space, display_of_spaces(*where));
    enter_overview(OverviewMode::ChoosingSplit);
    // Nothing to choose from: it's only full screen after all.
    if (m_thumbnails.is_empty() || !m_overview) {
        finish_overview_at_once();
        space->choosing = false;
        layout_space(*space, display_of_spaces(*where));
    }
}

void Server::finish_split_view(ServerWindow& chosen)
{
    auto* full = m_active;
    DisplaySpaces* where = nullptr;
    Space* space = full ? space_with_id(full->space, &where) : nullptr;
    if (!space || !space->choosing)
        return;
    chosen.full_screen_restore = chosen.frame;
    chosen.full_screen_from = chosen.space;
    chosen.full_screen = true;
    chosen.minimized = false;
    chosen.space = space->id;
    space->beside = &chosen;
    space->choosing = false;
    layout_space(*space, display_of_spaces(*where));
    update_visibility(chosen);
    activate(&chosen);
    notify_full_screen(chosen);
    refresh_lasting_overlays();
    shell_state_changed();
}

void Server::come_back_from_full_screen(ServerWindow& window)
{
    window.full_screen = false;
    if (space_with_id(window.full_screen_from))
        window.space = window.full_screen_from;
    else
        window.space = spaces_of(m_screen.main_display()).spaces[0].id;
    if (!window.full_screen_restore.is_empty())
        resize_live(window, window.full_screen_restore);
    update_visibility(window);
    notify_full_screen(window);
}

void Server::leave_full_screen(ServerWindow& window, bool window_is_going)
{
    if (!window.full_screen)
        return;
    DisplaySpaces* where = nullptr;
    auto* space = space_with_id(window.space, &where);
    if (!space) {
        window.full_screen = false;
        return;
    }
    auto* other = space->full == &window ? space->beside : space->full;
    if (m_overview && m_overview_mode == OverviewMode::ChoosingSplit)
        finish_overview_at_once();
    if (other) {
        // The other one has it to itself now.
        space->full = other;
        space->beside = nullptr;
        layout_space(*space, display_of_spaces(*where));
        if (!window_is_going)
            come_back_from_full_screen(window);
        else
            window.full_screen = false;
        refresh_lasting_overlays();
        shell_state_changed();
        return;
    }
    int const from = window.full_screen_from;
    if (!window_is_going)
        come_back_from_full_screen(window);
    else
        window.full_screen = false;
    space->full = nullptr;
    space->choosing = false;
    space->leaving = true;
    // Back where it came from, or the nearest there is.
    int target = -1;
    for (size_t i = 0; i < where->spaces.size(); ++i) {
        if (where->spaces[i].id == from && !where->spaces[i].leaving)
            target = static_cast<int>(i);
    }
    if (target < 0) {
        for (int i = where->current; i >= 0 && target < 0; --i) {
            if (!where->spaces[static_cast<size_t>(i)].leaving)
                target = i;
        }
    }
    if (target < 0) {
        for (size_t i = 0; i < where->spaces.size() && target < 0; ++i) {
            if (!where->spaces[i].leaving)
                target = static_cast<int>(i);
        }
    }
    if (target >= 0) {
        go_to_space(*where, target, true);
        if (!window_is_going)
            activate(&window);
    }
    if (!m_sliding)
        remove_leaving_spaces();
    set_menu_reveal(-1, false);
    set_dock_reveal(false);
    shell_state_changed();
}

void Server::notify_full_screen(ServerWindow& window)
{
    if (!window.client)
        return;
    IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::FullScreenChanged));
    encoder.encode(window.id);
    encoder.encode(window.full_screen);
    send_to(*window.client, encoder);
}

void Server::set_full_screen(IPC::Sender const& sender, u32 id, bool full)
{
    auto* window = window_of(sender, id);
    if (!window) {
        auto* client = client_of(sender);
        if (!client || !client->shows_window_menus || !is_in_session(client))
            return;
        for (auto* other : m_windows) {
            if (other->id == id)
                window = other;
        }
    }
    if (!window)
        return;
    if (full)
        enter_full_screen(*window);
    else
        leave_full_screen(*window, false);
}

void Server::toggle_full_screen(IPC::Sender const& sender, u32 id)
{
    auto* window = window_of(sender, id);
    if (!window)
        return;
    if (window->full_screen)
        leave_full_screen(*window, false);
    else
        enter_full_screen(*window);
}

void Server::tile_in_split_view(IPC::Sender const& sender, u32 id, bool on_left)
{
    auto* window = window_of(sender, id);
    if (!window) {
        auto* client = client_of(sender);
        if (!client || !client->shows_window_menus || !is_in_session(client))
            return;
        for (auto* other : m_windows) {
            if (other->id == id)
                window = other;
        }
    }
    if (window && !window->full_screen)
        enter_split_view(*window, on_left);
}

bool Server::is_kept_out_of_the_way(ServerWindow const& window) const
{
    if (window.level != Window::LevelMenuBar && window.level != Window::LevelDock && !is_desktop_level(window.level))
        return false;
    auto const& display = m_screen.display_at(window.frame.center());
    for (auto const& one : m_display_spaces) {
        if (one.output != display.output)
            continue;
        if (!one.spaces[static_cast<size_t>(one.current)].full)
            return false;
        // A full screen Space has no desktop: nothing of it shows there, icons included.
        if (is_desktop_level(window.level))
            return true;
        if (window.level == Window::LevelMenuBar)
            return !(m_reveal > 0 && m_reveal_output == display.output);
        return m_dock_reveal <= 0;
    }
    return false;
}

Optional<int> Server::split_divider_at(IntPoint point)
{
    auto const& display = m_screen.display_at(point);
    auto const& where = spaces_of(display);
    auto const& space = where.spaces[static_cast<size_t>(where.current)];
    if (!space.full || (!space.beside && !space.choosing))
        return {};
    auto const area = display.points;
    int const left_width = clamp(static_cast<int>(static_cast<float>(area.width - divider_width) * space.divider), smallest_side, area.width - divider_width - smallest_side);
    int const x = area.x + left_width;
    if (point.x >= x - 2 && point.x < x + divider_width + 2)
        return x;
    return {};
}

void Server::drag_divider(IntPoint point)
{
    auto const& display = m_screen.display_at(point);
    auto& where = spaces_of(display);
    auto& space = where.spaces[static_cast<size_t>(where.current)];
    if (!space.full)
        return;
    space.divider = static_cast<float>(point.x - display.points.x) / static_cast<float>(display.points.width - divider_width);
    layout_space(space, display);
    refresh_lasting_overlays();
}

void Server::check_full_screen_edges(IntPoint point)
{
    auto const& display = m_screen.display_at(point);
    auto const& where = spaces_of(display);
    auto const& space = where.spaces[static_cast<size_t>(where.current)];
    if (!space.full || m_overview) {
        if (m_reveal_wanted)
            set_menu_reveal(-1, false);
        if (m_dock_wanted)
            set_dock_reveal(false);
        return;
    }
    int const from_top = point.y - display.points.y;
    if (!m_reveal_wanted && from_top <= 1)
        set_menu_reveal(display.output, true);
    else if (m_reveal_wanted && from_top > m_work_top + hidden_top_of(*space.full) + 16)
        set_menu_reveal(-1, false);
    if (&display != &m_screen.main_display())
        return;
    // The Dock, from whichever edge it's on.
    int const from_bottom = display.points.bottom() - 1 - point.y;
    int const from_left = point.x - display.points.x;
    int const from_right = display.points.right() - 1 - point.x;
    int const from_edge = m_dock_edge == 1 ? from_left : (m_dock_edge == 2 ? from_right : from_bottom);
    int const reach = m_dock_thickness > 0 ? m_dock_thickness : m_work_bottom;
    if (!m_dock_wanted && from_edge <= 1)
        set_dock_reveal(true);
    else if (m_dock_wanted && from_edge > reach + 16)
        set_dock_reveal(false);
}

void Server::set_menu_reveal(int output, bool wanted)
{
    if (m_reveal_wanted == wanted)
        return;
    m_reveal_wanted = wanted;
    if (wanted)
        m_reveal_output = output;
    m_reveal_from = m_reveal;
    m_reveal_started = now_seconds();
    start_animating();
}

void Server::set_dock_reveal(bool wanted)
{
    if (m_dock_wanted == wanted)
        return;
    m_dock_wanted = wanted;
    m_dock_reveal_from = m_dock_reveal;
    m_dock_reveal_started = now_seconds();
    start_animating();
}

bool Server::advance_reveals(double now)
{
    bool still_going = false;
    auto advance = [&](float& amount, float from, double started, bool wanted) {
        float const target = wanted ? 1.0f : 0.0f;
        if (amount == target)
            return false;
        float const t = m_reduce_motion ? 1 : clamp(static_cast<float>((now - started) / static_cast<double>(reveal_seconds)), 0.0f, 1.0f);
        amount = from + (target - from) * ease_out(t);
        if (t >= 1)
            amount = target;
        return true;
    };
    bool const menu_moved = advance(m_reveal, m_reveal_from, m_reveal_started, m_reveal_wanted);
    bool const dock_moved = advance(m_dock_reveal, m_dock_reveal_from, m_dock_reveal_started, m_dock_wanted);
    if (!menu_moved && !dock_moved)
        return false;
    int const scale = m_screen.scale();
    for (auto* window : m_windows) {
        if (window->level == Window::LevelMenuBar && menu_moved) {
            window->slide_y = -static_cast<int>((1 - m_reveal) * static_cast<float>(window->frame.height * scale));
        } else if (window->level == Window::LevelDock && dock_moved) {
            float const away = 1 - m_dock_reveal;
            window->slide_x = m_dock_edge == 1 ? -static_cast<int>(away * static_cast<float>(window->frame.width * scale)) : (m_dock_edge == 2 ? static_cast<int>(away * static_cast<float>(window->frame.width * scale)) : 0);
            window->slide_y = m_dock_edge == 0 ? static_cast<int>(away * static_cast<float>(window->frame.height * scale)) : 0;
        }
    }
    update_all_visibility();
    if (menu_moved) {
        for (auto& one : m_display_spaces) {
            if (one.output == m_reveal_output)
                layout_space(one.spaces[static_cast<size_t>(one.current)], display_of_spaces(one));
        }
    }
    if (m_reveal == 0 && m_dock_reveal == 0) {
        for (auto* window : m_windows) {
            if (window->level == Window::LevelMenuBar || window->level == Window::LevelDock) {
                window->slide_y = 0;
                if (window->level == Window::LevelDock)
                    window->slide_x = 0;
            }
        }
    }
    still_going = m_reveal != (m_reveal_wanted ? 1.0f : 0.0f) || m_dock_reveal != (m_dock_wanted ? 1.0f : 0.0f);
    m_compositor.invalidate_all();
    return still_going;
}

// ---- The desktop, uncovered

void Server::reveal_desktop(bool revealed, bool animated)
{
    if (m_desktop_revealed == revealed && (animated || m_desktop_reveal == (revealed ? 1.0f : 0.0f)))
        return;
    m_desktop_revealed = revealed;
    m_desktop_reveal_from = m_desktop_reveal;
    m_desktop_reveal_started = now_seconds();
    if (!animated || m_reduce_motion)
        m_desktop_reveal_started = 0;
    start_animating();
    if (!animated) {
        (void)advance_desktop_reveal(now_seconds());
    }
    shell_state_changed();
}

bool Server::advance_desktop_reveal(double now)
{
    float const target = m_desktop_revealed ? 1.0f : 0.0f;
    if (m_desktop_reveal == target)
        return false;
    float const t = m_desktop_reveal_started == 0 ? 1 : clamp(static_cast<float>((now - m_desktop_reveal_started) / static_cast<double>(desktop_reveal_seconds)), 0.0f, 1.0f);
    m_desktop_reveal = m_desktop_reveal_from + (target - m_desktop_reveal_from) * ease_out(t);
    if (t >= 1)
        m_desktop_reveal = target;
    int const scale = m_screen.scale();
    auto const origin = m_screen.origin();
    bool const many = m_screen.displays().size() > 1;
    for (auto* window : m_windows) {
        if (window->level != Window::LevelNormal || window->full_screen)
            continue;
        if (m_desktop_reveal == 0) {
            window->slide_x = 0;
            window->slide_clip = {};
            continue;
        }
        auto const area = m_screen.display_at(window->frame.center()).points;
        // To whichever side it's nearer, leaving a little of it there.
        bool const leftwards = window->frame.center().x < area.center().x;
        int const to = leftwards ? area.x + desktop_sliver - window->frame.width : area.right() - desktop_sliver;
        window->slide_x = static_cast<int>(static_cast<float>((to - window->frame.x) * scale) * m_desktop_reveal);
        window->slide_clip = many ? IntRect { (area.x + origin.x) * scale, (area.y + origin.y) * scale, area.width * scale, area.height * scale } : IntRect {};
    }
    m_compositor.invalidate_all();
    return m_desktop_reveal != target;
}

void Server::press_on_wallpaper(IntPoint point, bool activating)
{
    bool const was_uncovered = m_desktop_revealed;
    auto const& where = spaces_of(m_screen.display_at(point));
    bool const full = where.spaces[static_cast<size_t>(where.current)].full != nullptr;
    if (was_uncovered)
        reveal_desktop(false, true);
    else if ((m_click_wallpaper_reveals || m_stage) && !full)
        reveal_desktop(true, true);
    if (!activating)
        return;
    if (was_uncovered)
        activate_frontmost();
    else
        activate(nullptr);
}

void Server::wallpaper_clicked(IPC::Sender const& sender)
{
    // Only the desktop's own says so.
    auto* client = client_of(sender);
    bool from_desktop = false;
    for (auto const* window : m_windows) {
        if (window->client == client && window->level == Window::LevelDesktop)
            from_desktop = true;
    }
    if (from_desktop)
        press_on_wallpaper(m_pointer, false);
}

void Server::set_click_wallpaper(IPC::Sender const&, bool reveals)
{
    m_click_wallpaper_reveals = reveals;
    if (!reveals && !m_stage)
        reveal_desktop(false, true);
}

// ---- Hot corners

void Server::set_hot_corners(IPC::Sender const&, i32 top_left, i32 top_right, i32 bottom_left, i32 bottom_right)
{
    m_hot_corners[0] = top_left;
    m_hot_corners[1] = top_right;
    m_hot_corners[2] = bottom_left;
    m_hot_corners[3] = bottom_right;
}

void Server::check_hot_corner(IntPoint point)
{
    // Not at the login window or over a locked screen.
    if (m_sessions_managed && (m_session_locked || m_session_uid == 0))
        return;
    auto const area = m_screen.display_at(point).points;
    bool const left = point.x <= area.x + 1;
    bool const right = point.x >= area.right() - 2;
    bool const top = point.y <= area.y + 1;
    bool const bottom = point.y >= area.bottom() - 2;
    int corner = -1;
    if (m_buttons == 0) {
        if (top && left)
            corner = 0;
        else if (top && right)
            corner = 1;
        else if (bottom && left)
            corner = 2;
        else if (bottom && right)
            corner = 3;
    }
    if (corner == m_hot_corner)
        return;
    m_hot_corner = corner;
    if (m_hot_corner_timer >= 0)
        m_loop.cancel_timer(m_hot_corner_timer);
    m_hot_corner_timer = -1;
    if (corner < 0 || m_hot_corners[corner] == static_cast<i32>(Window::HotCornerAction::None))
        return;
    m_hot_corner_timer = m_loop.add_timer(hot_corner_milliseconds, false, [this, corner] {
        m_hot_corner_timer = -1;
        fire_hot_corner(m_hot_corners[corner]);
    });
}

void Server::fire_hot_corner(int action)
{
    using Window::HotCornerAction;
    switch (static_cast<HotCornerAction>(action)) {
    case HotCornerAction::Overview:
        if (m_overview)
            leave_overview(nullptr);
        else
            enter_overview();
        return;
    case HotCornerAction::ApplicationWindows:
        if (m_overview)
            leave_overview(nullptr);
        else
            enter_overview(OverviewMode::OneApplication);
        return;
    case HotCornerAction::Desktop:
        reveal_desktop(!m_desktop_revealed, true);
        return;
    default:
        break;
    }
    for (auto& client : m_clients) {
        if (!client->shows_window_menus || !is_in_session(client.ptr()))
            continue;
        IPC::Encoder encoder(WindowClientEndpoint::magic, static_cast<u32>(WindowClientEndpoint::Method::HotCornerFired));
        encoder.encode(action);
        send_to(*client, encoder);
        return;
    }
}

// ---- Between applications

Vector<Client*> Server::applications_most_recent_first()
{
    Vector<Client*> applications;
    for (auto* client : m_recent_applications) {
        if (!client->application_name.is_empty())
            applications.append(client);
    }
    for (auto& client : m_clients) {
        if (client->application_name.is_empty())
            continue;
        bool seen = false;
        for (auto* one : applications)
            seen = seen || one == client.ptr();
        if (!seen)
            applications.append(client.ptr());
    }
    return applications;
}

void Server::cycle_applications(bool backward)
{
    auto applications = applications_most_recent_first();
    if (applications.size() < 2)
        return;
    if (!m_switching) {
        m_switching = true;
        m_switch_selected = 0;
        m_switch_asked_at = m_event_time ? m_event_time : static_cast<u64>(now_seconds() * 1'000'000.0);
    }
    auto const count = static_cast<int>(applications.size());
    m_switch_selected = (m_switch_selected + (backward ? count - 1 : 1)) % count;
    shell_state_changed();
}

void Server::finish_switching(bool go)
{
    if (!m_switching)
        return;
    m_switching = false;
    auto applications = applications_most_recent_first();
    if (go && m_switch_selected < static_cast<int>(applications.size())) {
        auto* client = applications[static_cast<size_t>(m_switch_selected)];
        if (client->hidden) {
            client->hidden = false;
            update_all_visibility();
        }
        activate_client(*client);
    }
    shell_state_changed();
}

void Server::application_became_active(Client* previous)
{
    if (!m_active_application || previous == m_active_application)
        return;
    m_recent_applications.remove_all_matching([&](auto* one) { return one == m_active_application; });
    m_recent_applications.insert(0, m_active_application);
    if (!m_stage)
        return;
    // Afterwards: this comes from inside of what activates windows.
    m_loop.deferred_invoke([this] { stage_application_changed(); });
}

// ---- Stage

void Server::add_to_stage_strip(Client& client)
{
    ServerWindow* front = nullptr;
    for (auto* window : m_windows) {
        if (window->client == &client && window->visible && window->level == Window::LevelNormal && window->is_document_window())
            front = window;
    }
    if (!front)
        return;
    if (front->chrome_is_stale)
        render_chrome(*front);
    auto picture = m_compositor.snapshot(*front);
    if (picture.is_error())
        return;
    m_stage_strip.remove_all_matching([&](auto const& item) { return item.client == &client; });
    StageItem item;
    item.client = &client;
    item.picture = picture.release_value();
    m_stage_strip.insert(0, move(item));
    if (m_stage_strip.size() > 5)
        m_stage_strip.remove(m_stage_strip.size() - 1);
}

void Server::layout_stage_strip()
{
    auto const area = work_area_of(m_screen.main_display());
    float total = 0;
    for (auto& item : m_stage_strip) {
        float const aspect = static_cast<float>(item.picture->height()) / static_cast<float>(item.picture->width());
        item.rect.width = stage_strip_width;
        item.rect.height = min(stage_strip_width * aspect, stage_strip_tallest);
        if (item.rect.height == stage_strip_tallest)
            item.rect.width = stage_strip_tallest / aspect;
        total += item.rect.height + stage_strip_gap;
    }
    float y = static_cast<float>(area.y) + (static_cast<float>(area.height) - (total - stage_strip_gap)) / 2;
    for (auto& item : m_stage_strip) {
        item.rect.x = static_cast<float>(area.x) + stage_strip_left + (stage_strip_width - item.rect.width) / 2;
        item.rect.y = y;
        y += item.rect.height + stage_strip_gap;
    }
}

bool Server::stage_strip_is_covered() const
{
    float const right = stage_strip_left + stage_strip_width + stage_strip_left;
    for (auto const* window : m_windows) {
        if (window->visible && window->level == Window::LevelNormal && window->is_document_window()
            && static_cast<float>(window->frame.x) < right && window->frame.right() > 0)
            return true;
    }
    return false;
}

void Server::make_room_for_stage()
{
    int const right = static_cast<int>(stage_strip_left + stage_strip_width + stage_strip_left);
    auto const area = work_area_of(m_screen.main_display());
    for (auto* window : m_windows) {
        if (window->client != m_stage_shown || !window->visible || window->level != Window::LevelNormal || !window->is_document_window() || window->full_screen)
            continue;
        if (window->frame.x >= right || window->frame.width > area.right() - right)
            continue;
        if (window->stage_restore.is_empty())
            window->stage_restore = window->frame;
        auto frame = window->frame;
        frame.x = right;
        move_window_to(*window, frame);
        send_moved(*window);
    }
}

void Server::stage_application_changed()
{
    if (!m_stage || !m_active_application || m_stage_shown == m_active_application)
        return;
    auto* previous = m_stage_shown;
    m_stage_shown = m_active_application;
    if (previous)
        add_to_stage_strip(*previous);
    m_stage_strip.remove_all_matching([&](auto const& item) { return item.client == m_active_application; });
    update_all_visibility();
    make_room_for_stage();
    layout_stage_strip();
    refresh_lasting_overlays();
    m_compositor.invalidate_all();
    schedule_frame();
}

void Server::set_stage(IPC::Sender const&, bool on)
{
    if (m_stage == on)
        return;
    m_stage_strip.clear();
    m_stage_shown = m_active_application;
    if (on) {
        for (auto* client : applications_most_recent_first()) {
            if (client != m_stage_shown)
                add_to_stage_strip(*client);
        }
        // In the order they were last used, as they've each gone in at the front.
        Vector<StageItem> reversed;
        for (size_t i = m_stage_strip.size(); i > 0; --i)
            reversed.append(move(m_stage_strip[i - 1]));
        m_stage_strip = move(reversed);
    }
    m_stage = on;
    update_all_visibility();
    if (on) {
        make_room_for_stage();
        layout_stage_strip();
    } else {
        for (auto* window : m_windows) {
            if (window->stage_restore.is_empty())
                continue;
            move_window_to(*window, window->stage_restore);
            send_moved(*window);
            window->stage_restore = {};
        }
    }
    refresh_lasting_overlays();
    shell_state_changed();
    m_compositor.invalidate_all();
    schedule_frame();
}

bool Server::stage_strip_pointer(IntPoint point)
{
    if (!m_stage || stage_strip_is_covered())
        return false;
    for (auto const& item : m_stage_strip) {
        if (item.rect.contains(point.to<float>())) {
            auto* client = item.client;
            activate_client(*client);
            return true;
        }
    }
    return false;
}

Vector<Compositor::Overlay> Server::lasting_overlays()
{
    Vector<Compositor::Overlay> overlays;
    if (m_stage && !stage_strip_is_covered()) {
        for (auto const& item : m_stage_strip) {
            Compositor::Overlay overlay;
            overlay.kind = Compositor::Overlay::Kind::Thumbnail;
            overlay.picture = item.picture;
            overlay.rect = item.rect;
            overlay.radius = 6;
            overlay.shadow = true;
            overlays.append(move(overlay));
        }
    }
    for (auto const& one : m_display_spaces) {
        auto const& space = one.spaces[static_cast<size_t>(one.current)];
        if (!space.full || (!space.beside && !space.choosing))
            continue;
        auto const area = display_of_spaces(one).points;
        int const left_width = clamp(static_cast<int>(static_cast<float>(area.width - divider_width) * space.divider), smallest_side, area.width - divider_width - smallest_side);
        Compositor::Overlay bar;
        bar.kind = Compositor::Overlay::Kind::Tile;
        bar.rect = { static_cast<float>(area.x + left_width), static_cast<float>(area.y), static_cast<float>(divider_width), static_cast<float>(area.height) };
        bar.color = Color { 0, 0, 0, 255 };
        overlays.append(move(bar));
        Compositor::Overlay grip;
        grip.kind = Compositor::Overlay::Kind::Tile;
        grip.rect = { static_cast<float>(area.x + left_width) + 1, static_cast<float>(area.center().y) - 20, static_cast<float>(divider_width) - 2, 40 };
        grip.radius = 2;
        grip.color = Color { 160, 160, 160, 255 };
        overlays.append(move(grip));
    }
    return overlays;
}

void Server::refresh_lasting_overlays()
{
    if (m_overview || m_boot_screen || !m_genies.is_empty())
        return;
    m_compositor.set_overlays(lasting_overlays());
    m_compositor.invalidate_all();
    schedule_frame();
}

// ---- Overview

String Server::space_label(DisplaySpaces const& where, int index) const
{
    auto const& space = where.spaces[static_cast<size_t>(index)];
    StringBuilder label;
    if (space.full) {
        // The application's: a window's title could be any length.
        label.append(space.full->client ? space.full->client->application_name.view() : space.full->title.view());
        if (space.beside && space.beside->client) {
            label.append(" & "_sv);
            label.append(space.beside->client->application_name.view());
        }
    } else {
        int desktop = 0;
        for (int before = 0; before <= index; ++before)
            desktop += where.spaces[static_cast<size_t>(before)].full ? 0 : 1;
        label.appendff("Desktop {}", desktop);
    }
    return MUST(label.to_string());
}

JSON::Value Server::overview_accessibility(Display const& display) const
{
    auto origin = display.points.origin();
    auto node = [&](StringView role, StringView label, FloatRect rect, StringView value = {}) {
        auto one = JSON::Value::object();
        MUST(one.set("role"_sv, MUST(JSON::Value::string(role))));
        MUST(one.set("label"_sv, MUST(JSON::Value::string(label))));
        MUST(one.set("value"_sv, MUST(JSON::Value::string(value))));
        MUST(one.set("x"_sv, JSON::Value(static_cast<double>(rect.x - static_cast<float>(origin.x)))));
        MUST(one.set("y"_sv, JSON::Value(static_cast<double>(rect.y - static_cast<float>(origin.y)))));
        MUST(one.set("width"_sv, JSON::Value(static_cast<double>(rect.width))));
        MUST(one.set("height"_sv, JSON::Value(static_cast<double>(rect.height))));
        MUST(one.set("enabled"_sv, JSON::Value(true)));
        MUST(one.set("focused"_sv, JSON::Value(false)));
        MUST(one.set("children"_sv, JSON::Value::array()));
        return one;
    };
    auto children = JSON::Value::array();
    if (m_overview_mode == OverviewMode::Everything) {
        auto const& where = spaces_of_const(display);
        int const count = static_cast<int>(where.spaces.size());
        // With the row closed, only their names, and nothing else to press.
        bool const open = where.bar_amount >= 0.5f;
        for (int i = 0; i < count; ++i) {
            auto const tile = open ? overview_tile(display, where, i) : overview_label(display, where, i);
            auto const label = space_label(where, i);
            MUST(children.append(node("button"_sv, label.view(), tile, i == where.current ? "current"_sv : StringView {})));
            if (open && (count > 1 || where.spaces[static_cast<size_t>(i)].full)) {
                StringBuilder remove;
                remove.append("Remove "_sv);
                remove.append(label.view());
                MUST(children.append(node("button"_sv, remove.view(), { tile.x - 7, tile.y - 7, 16, 16 })));
            }
        }
        if (open)
            MUST(children.append(node("button"_sv, "Add Desktop"_sv, overview_tile(display, where, count))));
    }
    for (auto const& thumbnail : m_thumbnails) {
        if (!thumbnail.shown || !display.points.contains(thumbnail.home.center().to<int>()))
            continue;
        auto const* window = thumbnail.window;
        MUST(children.append(node("window"_sv, window->title.view(), thumbnail.spread, window->client ? window->client->application_name.view() : StringView {})));
    }
    auto group = node("group"_sv, "Overview"_sv, display.points.to<float>());
    MUST(group.set("children"_sv, move(children)));
    return group;
}

FloatRect Server::overview_tile(Display const& display, DisplaySpaces const& where, int index) const
{
    float const tile_width = m_theme.overview_tile_width;
    float const tile_height = m_theme.overview_tile_height;
    float const gap = m_theme.overview_padding;
    int const tiles = static_cast<int>(where.spaces.size()) + 1;
    float const total = static_cast<float>(tiles) * tile_width + static_cast<float>(tiles - 1) * gap;
    float const x = static_cast<float>(display.points.x) + (static_cast<float>(display.points.width) - total) / 2;
    // (Over where the menu bar was: that's out of the way while Overview's showing.) Above the top until the row opens.
    float const y = static_cast<float>(display.points.y) + tile_top - (1 - where.bar_amount) * (tile_height + tile_top + tile_label_height);
    return { x + static_cast<float>(index) * (tile_width + gap), y, tile_width, tile_height };
}

FloatRect Server::overview_label(Display const& display, DisplaySpaces const& where, int index) const
{
    auto const tile = overview_tile(display, where, index);
    float const closed = static_cast<float>(display.points.y) + (m_theme.overview_collapsed_bar - tile_label_height) / 2;
    float const open = static_cast<float>(display.points.y) + tile_top + tile.height + 2;
    return { tile.x, closed + (open - closed) * where.bar_amount, tile.width, tile_label_height };
}

void Server::open_overview_bar(DisplaySpaces& where, bool open)
{
    if (where.bar_open == open)
        return;
    where.bar_open = open;
    where.bar_from = where.bar_amount;
    where.bar_started = now_seconds();
    start_animating();
}

bool Server::advance_overview_bar(double now)
{
    bool moved = false;
    bool still_going = false;
    float const duration = m_reduce_motion ? 0.001f : m_theme.overview_bar_seconds;
    for (auto& where : m_display_spaces) {
        float const target = where.bar_open ? 1.0f : 0.0f;
        if (where.bar_amount == target)
            continue;
        float const t = clamp(static_cast<float>((now - where.bar_started) / static_cast<double>(duration)), 0.0f, 1.0f);
        where.bar_amount = where.bar_from + (target - where.bar_from) * ease_out(t);
        if (t >= 1)
            where.bar_amount = target;
        moved = true;
        still_going = still_going || where.bar_amount != target;
    }
    // What's under it makes room.
    if (moved) {
        layout_overview();
        update_overview_overlays();
    }
    return still_going;
}

void Server::slide_menu_bars(float amount)
{
    int const scale = m_screen.scale();
    for (auto* window : m_windows) {
        if (window->level != Window::LevelMenuBar)
            continue;
        int const slide = -static_cast<int>(amount * static_cast<float>(window->frame.height * scale));
        if (window->slide_y != slide) {
            m_compositor.invalidate_window(*window);
            window->slide_y = slide;
            m_compositor.invalidate_window(*window);
        }
    }
}

void Server::collect_overview_windows()
{
    m_thumbnails.clear();
    auto add = [&](ServerWindow& window, bool shown) {
        if (window.chrome_is_stale && window.is_titled())
            render_chrome(window);
        auto picture = m_compositor.snapshot(window);
        if (picture.is_error())
            return;
        Thumbnail thumbnail;
        thumbnail.window = &window;
        thumbnail.picture = picture.release_value();
        thumbnail.home = window.frame.to<float>();
        if (window.full_screen) {
            int const scale = m_screen.scale();
            int const top = hidden_top_of(window);
            int const tile_width = static_cast<int>(m_theme.overview_tile_width);
            IntRect const part { 0, top * scale, window.frame.width * scale, (window.frame.height - top) * scale };
            auto small = m_compositor.miniature_of(*thumbnail.picture, part, { tile_width, static_cast<int>(m_theme.overview_tile_height) }, m_theme.overview_tile_radius);
            if (!small.is_error())
                thumbnail.tile_picture = small.release_value();
            // Its title bar's hidden, so that's not what's shown of it.
            auto cropped = m_compositor.miniature_of(*thumbnail.picture, part, { window.frame.width, window.frame.height - top }, m_theme.corner_radius);
            if (!cropped.is_error())
                thumbnail.picture = cropped.release_value();
            thumbnail.home.y += static_cast<float>(top);
            thumbnail.home.height -= static_cast<float>(top);
        }
        thumbnail.spread = thumbnail.home;
        thumbnail.shown = shown;
        m_thumbnails.append(move(thumbnail));
    };
    for (auto* window : m_windows) {
        if (window->level != Window::LevelNormal || window->is_popup())
            continue;
        switch (m_overview_mode) {
        case OverviewMode::Everything:
            if (window->visible)
                add(*window, true);
            else if (window->shown_by_client && !window->minimized && !window->full_screen && !(window->client && window->client->hidden))
                add(*window, false);
            break;
        case OverviewMode::OneApplication:
            if (window->client == m_active_application && window->is_document_window() && (window->visible || window->minimized))
                add(*window, true);
            break;
        case OverviewMode::ChoosingSplit:
            // Whatever's on the Space it came from.
            if (m_active && window != m_active && window->is_document_window() && window->is_resizable() && !window->minimized
                && window->shown_by_client && window->space == m_active->full_screen_from)
                add(*window, true);
            break;
        }
    }
}

void Server::layout_overview()
{
    for (auto const& display : m_screen.displays()) {
        // Those of its Space, in a grid, in what's free of it, under the row of Spaces.
        auto area = work_area_of(display);
        if (m_overview_mode == OverviewMode::ChoosingSplit) {
            auto const& where = spaces_of(display);
            auto const& space = where.spaces[static_cast<size_t>(where.current)];
            if (!space.choosing)
                continue;
            int const half = (display.points.width - divider_width) / 2;
            area = { space.full_on_left ? display.points.x + half + divider_width : display.points.x, display.points.y + m_work_top, half, display.points.height - m_work_top };
        }
        float const bar = m_theme.overview_collapsed_bar + (m_theme.overview_bar - m_theme.overview_collapsed_bar) * spaces_of(display).bar_amount;
        float const top = m_overview_mode == OverviewMode::Everything ? static_cast<float>(display.points.y) + bar : static_cast<float>(area.y);
        float const bottom = static_cast<float>(area.bottom());
        float const left = static_cast<float>(area.x);
        float const right = static_cast<float>(area.right());
        float const padding = m_theme.overview_padding;
        Vector<Thumbnail*> here;
        Vector<Thumbnail*> small;
        for (auto& thumbnail : m_thumbnails) {
            if (!thumbnail.shown)
                continue;
            if (m_overview_mode == OverviewMode::ChoosingSplit) {
                here.append(&thumbnail);
                continue;
            }
            if (&m_screen.display_at(thumbnail.window->frame.center()) != &display)
                continue;
            if (thumbnail.window->minimized)
                small.append(&thumbnail);
            else
                here.append(&thumbnail);
        }
        // Those in the Dock, in a row of their own along the bottom.
        float grid_bottom = bottom;
        if (!small.is_empty()) {
            float const row_height = (bottom - top) / 5;
            grid_bottom = bottom - row_height;
            float x = left + padding;
            for (auto* thumbnail : small) {
                float const scale = min((row_height - padding * 2) / thumbnail->home.height, 1.0f);
                float const width = thumbnail->home.width * scale;
                thumbnail->spread = { x, grid_bottom + padding, width, thumbnail->home.height * scale };
                x += width + padding;
            }
        }
        size_t const count = here.size();
        if (count == 0)
            continue;
        size_t columns = 1;
        while (columns * columns < count)
            ++columns;
        size_t const rows = (count + columns - 1) / columns;
        float const cell_width = (right - left) / static_cast<float>(columns);
        float const cell_height = (grid_bottom - top) / static_cast<float>(rows);
        for (size_t i = 0; i < count; ++i) {
            auto& thumbnail = *here[i];
            size_t const column = i % columns;
            size_t const row = i / columns;
            float const available_width = cell_width - padding * 2;
            float const available_height = cell_height - padding * 2;
            float const scale = min(min(available_width / thumbnail.home.width, available_height / thumbnail.home.height), 1.0f);
            float const width = thumbnail.home.width * scale;
            float const height = thumbnail.home.height * scale;
            thumbnail.spread = { left + cell_width * static_cast<float>(column) + (cell_width - width) / 2, top + cell_height * static_cast<float>(row) + (cell_height - height) / 2, width, height };
        }
    }
}

void Server::enter_overview(OverviewMode mode)
{
    if (m_overview)
        return;
    if (mode == OverviewMode::OneApplication && !m_active_application)
        return;
    reveal_desktop(false, false);
    m_overview_mode = mode;
    for (auto& one : m_display_spaces) {
        one.bar_open = false;
        one.bar_amount = 0;
    }
    collect_overview_windows();
    if (mode == OverviewMode::Everything) {
        m_overview_wallpapers.clear();
        for (auto const& display : m_screen.displays()) {
            auto picture = m_compositor.miniature_of_wallpaper(display.points, { static_cast<int>(m_theme.overview_tile_width), static_cast<int>(m_theme.overview_tile_height) }, m_theme.overview_tile_radius);
            if (!picture.is_error())
                m_overview_wallpapers.append({ display.output, picture.release_value() });
        }
    }
    layout_overview();
    m_overview = true;
    m_overview_leaving = false;
    m_overview_chosen = nullptr;
    m_overview_started = now_seconds();
    m_overview_hover = -1;
    m_overview_hover_space = -1;
    m_overview_hover_close = false;
    m_overview_press_window = -1;
    m_overview_press_tile = -1;
    m_overview_dragging = false;
    m_compositor.set_hides_normal_windows(mode != OverviewMode::ChoosingSplit);
    start_animating();
    m_compositor.invalidate_all();
    shell_state_changed();
    schedule_frame();
}

void Server::leave_overview(ServerWindow* to_activate)
{
    if (!m_overview || m_overview_leaving)
        return;
    if (m_overview_mode == OverviewMode::ChoosingSplit) {
        // No second window: full screen, by itself.
        finish_overview_at_once();
        if (m_active) {
            DisplaySpaces* where = nullptr;
            if (auto* space = space_with_id(m_active->space, &where); space && space->choosing) {
                space->choosing = false;
                layout_space(*space, display_of_spaces(*where));
                refresh_lasting_overlays();
            }
        }
        return;
    }
    m_overview_leaving = true;
    m_overview_chosen = to_activate;
    m_overview_started = now_seconds();
    start_animating();
}

void Server::finish_overview_at_once()
{
    m_overview = false;
    m_overview_leaving = false;
    slide_menu_bars(0);
    m_compositor.set_hides_normal_windows(false);
    m_compositor.set_dim(Color { 0, 0, 0, 0 });
    m_thumbnails.clear();
    m_compositor.set_overlays(lasting_overlays());
    m_compositor.invalidate_all();
    shell_state_changed();
    schedule_frame();
}

void Server::update_overview_overlays()
{
    // (With motion reduced: there at once.)
    float const duration = m_reduce_motion ? 0.001f : m_theme.overview_seconds;
    auto t = static_cast<float>((now_seconds() - m_overview_started) / static_cast<double>(duration));
    t = clamp(t, 0.0f, 1.0f);
    float amount = ease_out(t);
    if (m_overview_leaving)
        amount = 1 - amount;
    Vector<Compositor::Overlay> overlays = m_overview_mode == OverviewMode::ChoosingSplit ? lasting_overlays() : Vector<Compositor::Overlay> {};
    // The menu bar makes way for the row of Spaces.
    if (m_overview_mode == OverviewMode::Everything)
        slide_menu_bars(amount);

    // Spaces along the top, of each display, and what's on them.
    if (m_overview_mode == OverviewMode::Everything) {
        for (auto const& display : m_screen.displays()) {
            auto const& where = spaces_of(display);
            float const off = (1 - amount) * m_theme.overview_collapsed_bar;
            for (int i = 0; i <= static_cast<int>(where.spaces.size()); ++i) {
                auto rect = overview_tile(display, where, i);
                rect.y -= off;
                bool const adding = i == static_cast<int>(where.spaces.size());
                bool const current = !adding && i == where.current;
                bool const hovered = i == m_overview_hover_space && &display == &m_screen.display_at(m_pointer);
                if (adding) {
                    Compositor::Overlay tile;
                    tile.kind = Compositor::Overlay::Kind::Tile;
                    tile.rect = rect;
                    tile.radius = m_theme.overview_tile_radius;
                    tile.color = hovered ? m_theme.overview_highlight : m_theme.overview_tile;
                    tile.label = text_of("+"_sv);
                    overlays.append(move(tile));
                    continue;
                }
                auto const& space = where.spaces[static_cast<size_t>(i)];
                if (current || hovered) {
                    Compositor::Overlay ring;
                    ring.kind = Compositor::Overlay::Kind::Tile;
                    ring.rect = rect.inflated(2);
                    ring.radius = m_theme.overview_tile_radius + 2;
                    ring.color = current ? m_theme.overview_tile_active : m_theme.overview_highlight;
                    overlays.append(move(ring));
                }
                for (auto const& wallpaper : m_overview_wallpapers) {
                    if (wallpaper.output != display.output || space.full)
                        continue;
                    Compositor::Overlay picture;
                    picture.kind = Compositor::Overlay::Kind::Thumbnail;
                    picture.picture = wallpaper.picture;
                    picture.rect = rect;
                    overlays.append(move(picture));
                }
                float const scale = rect.width / static_cast<float>(display.points.width);
                for (size_t index = 0; index < m_thumbnails.size(); ++index) {
                    auto const& thumbnail = m_thumbnails[index];
                    if (thumbnail.window->space != space.id || (m_overview_dragging && static_cast<int>(index) == m_overview_press_window))
                        continue;
                    Compositor::Overlay small;
                    small.kind = Compositor::Overlay::Kind::Thumbnail;
                    small.picture = thumbnail.tile_picture ? thumbnail.tile_picture : thumbnail.picture;
                    small.rect = { rect.x + (thumbnail.home.x - static_cast<float>(display.points.x)) * scale, rect.y + (thumbnail.home.y - static_cast<float>(display.points.y)) * scale, thumbnail.home.width * scale, thumbnail.home.height * scale };
                    overlays.append(move(small));
                }
                // Its name: of the window, if that's what it's for.
                Compositor::Overlay name;
                name.kind = Compositor::Overlay::Kind::Tile;
                name.rect = overview_label(display, where, i).translated(0, -off);
                name.color = Color { 0, 0, 0, 0 };
                name.small_label = true;
                name.label = space_label(where, i);
                overlays.append(move(name));
                // And over one, the way to get rid of it.
                if (hovered && (where.spaces.size() > 1 || space.full)) {
                    Compositor::Overlay close;
                    close.kind = Compositor::Overlay::Kind::Tile;
                    close.rect = { rect.x - 7, rect.y - 7, 16, 16 };
                    close.radius = 8;
                    close.color = m_overview_hover_close ? Color { 60, 60, 60, 255 } : Color { 30, 30, 30, 230 };
                    close.label = text_of("\xC3\x97"_sv);
                    close.small_label = true;
                    overlays.append(move(close));
                }
            }
        }
    }

    for (size_t i = 0; i < m_thumbnails.size(); ++i) {
        auto const& thumbnail = m_thumbnails[i];
        if (!thumbnail.shown)
            continue;
        auto rect = between(thumbnail.home, thumbnail.spread, m_overview_mode == OverviewMode::ChoosingSplit ? 1 : amount);
        bool const dragged = m_overview_dragging && static_cast<int>(i) == m_overview_press_window;
        if (dragged)
            rect = rect.translated(static_cast<float>(m_pointer.x - m_overview_press_at.x), static_cast<float>(m_pointer.y - m_overview_press_at.y));
        if (static_cast<int>(i) == m_overview_hover && !dragged) {
            Compositor::Overlay ring;
            ring.kind = Compositor::Overlay::Kind::Tile;
            ring.rect = rect.inflated(3);
            ring.radius = m_theme.corner_radius + 3;
            ring.color = m_theme.overview_highlight;
            overlays.append(move(ring));
        }
        Compositor::Overlay overlay;
        overlay.kind = Compositor::Overlay::Kind::Thumbnail;
        overlay.picture = thumbnail.picture;
        overlay.rect = rect;
        overlay.radius = m_theme.corner_radius;
        overlay.shadow = true;
        overlays.append(move(overlay));
    }
    if (m_overview_mode != OverviewMode::ChoosingSplit)
        m_compositor.set_dim(m_theme.overview_dim.with_opacity(amount));
    m_compositor.set_overlays(move(overlays));
    m_compositor.invalidate_all();
}

void Server::move_window_to_space(ServerWindow& window, DisplaySpaces& where, int index)
{
    if (window.full_screen)
        return;
    auto const& space = where.spaces[static_cast<size_t>(index)];
    if (space.full)
        return;
    window.space = space.id;
    for (auto& thumbnail : m_thumbnails) {
        if (thumbnail.window == &window)
            thumbnail.shown = space_is_showing(space.id);
    }
    update_visibility(window);
    layout_overview();
    if (m_overview)
        update_overview_overlays();
    if (m_active == &window && !window.visible) {
        activate(nullptr);
        activate_frontmost();
    }
    shell_state_changed();
}

void Server::remove_space(DisplaySpaces& where, int index)
{
    auto& space = where.spaces[static_cast<size_t>(index)];
    if (space.full) {
        auto* window = space.full;
        finish_overview_at_once();
        leave_full_screen(*window, false);
        return;
    }
    // What's on it goes to the one before, or if it's the first, the one after.
    int fallback = -1;
    for (int i = index - 1; i >= 0 && fallback < 0; --i) {
        if (!where.spaces[static_cast<size_t>(i)].full)
            fallback = i;
    }
    for (int i = index + 1; i < static_cast<int>(where.spaces.size()) && fallback < 0; ++i) {
        if (!where.spaces[static_cast<size_t>(i)].full)
            fallback = i;
    }
    if (fallback < 0)
        return;
    int const id = space.id;
    int const fallback_id = where.spaces[static_cast<size_t>(fallback)].id;
    for (auto* window : m_windows) {
        if (window->space == id)
            window->space = fallback_id;
    }
    for (auto* window : m_windows) {
        if (window->full_screen_from == id)
            window->full_screen_from = fallback_id;
    }
    bool const was_current = where.current == index;
    where.spaces.remove(static_cast<size_t>(index));
    if (was_current) {
        for (size_t i = 0; i < where.spaces.size(); ++i) {
            if (where.spaces[i].id == fallback_id)
                where.current = static_cast<int>(i);
        }
    } else if (index < where.current) {
        --where.current;
    }
    update_all_visibility();
    if (m_overview) {
        collect_overview_windows();
        layout_overview();
        update_overview_overlays();
    }
    shell_state_changed();
}

bool Server::overview_pointer(IntPoint point, u32 pressed, u32 released)
{
    if (m_overview_leaving)
        return true;
    auto const& display = m_screen.display_at(point);
    auto& where = spaces_of(display);
    int const space_count = static_cast<int>(where.spaces.size());
    // The row of Spaces opens at the top, and closes once the pointer's well away from it (or what's carried along it).
    if (m_overview_mode == OverviewMode::Everything) {
        int const from_top = point.y - display.points.y;
        if (from_top < static_cast<int>(m_theme.overview_collapsed_bar))
            open_overview_bar(where, true);
        else if (from_top > static_cast<int>(m_theme.overview_bar) + 8 && !(m_overview_dragging && m_overview_press_tile >= 0))
            open_overview_bar(where, false);
        // Only the one the pointer's on.
        for (auto& other : m_display_spaces) {
            if (&other != &where)
                open_overview_bar(other, false);
        }
    }
    // What it's over: a Space along the top (its picture, or with the row closed its name), or a window.
    int over_space = -1;
    bool over_close = false;
    if (m_overview_mode == OverviewMode::Everything) {
        bool const open = where.bar_amount >= 0.5f;
        for (int i = 0; i <= space_count; ++i) {
            if (!open && i == space_count)
                continue;
            auto const tile = open ? overview_tile(display, where, i) : overview_label(display, where, i);
            if (tile.inflated(4).contains(point.to<float>())) {
                over_space = i;
                FloatRect const close { tile.x - 7, tile.y - 7, 16, 16 };
                over_close = open && i < space_count && close.contains(point.to<float>()) && (space_count > 1 || where.spaces[static_cast<size_t>(i)].full);
            }
        }
    }
    int over_window = -1;
    for (size_t i = 0; i < m_thumbnails.size(); ++i) {
        if (m_thumbnails[i].shown && m_thumbnails[i].spread.contains(point.to<float>()))
            over_window = static_cast<int>(i);
    }
    bool const pressing = m_overview_press_window >= 0 || m_overview_press_tile >= 0;
    if (pressing && !m_overview_dragging) {
        int const dx = point.x - m_overview_press_at.x;
        int const dy = point.y - m_overview_press_at.y;
        m_overview_dragging = (dx * dx + dy * dy) > drag_slop * drag_slop;
    }
    // A Space, taken along the row.
    if (m_overview_dragging && m_overview_press_tile >= 0 && over_space >= 0 && over_space < space_count && over_space != m_overview_press_tile) {
        int const from = m_overview_press_tile;
        auto moving = move(where.spaces[static_cast<size_t>(from)]);
        int const current_id = where.spaces[static_cast<size_t>(where.current == from ? over_space : where.current)].id;
        where.spaces.remove(static_cast<size_t>(from));
        where.spaces.insert(static_cast<size_t>(over_space), move(moving));
        for (size_t i = 0; i < where.spaces.size(); ++i) {
            if (where.spaces[i].id == current_id)
                where.current = static_cast<int>(i);
        }
        if (from == where.current || over_space == where.current) {
            // (The one being carried stays as what's showing, if it was.)
        }
        m_overview_press_tile = over_space;
        shell_state_changed();
    }
    if (over_space != m_overview_hover_space || over_window != m_overview_hover || over_close != m_overview_hover_close || m_overview_dragging) {
        m_overview_hover_space = over_space;
        m_overview_hover = over_window;
        m_overview_hover_close = over_close;
        update_overview_overlays();
    }
    if (pressed) {
        m_overview_press_at = point;
        m_overview_press_window = over_space >= 0 ? -1 : over_window;
        m_overview_press_tile = over_space >= 0 && over_space < space_count && !over_close ? over_space : -1;
        m_overview_dragging = false;
        // Choosing the other side's a click, at once.
        if (m_overview_mode == OverviewMode::ChoosingSplit) {
            if (over_window >= 0) {
                auto* chosen = m_thumbnails[static_cast<size_t>(over_window)].window;
                finish_overview_at_once();
                finish_split_view(*chosen);
            }
            m_overview_press_window = -1;
            return true;
        }
        return true;
    }
    if (!released)
        return true;
    int const pressed_window = m_overview_press_window;
    int const pressed_tile = m_overview_press_tile;
    bool const dragged = m_overview_dragging;
    m_overview_press_window = -1;
    m_overview_press_tile = -1;
    m_overview_dragging = false;
    if (dragged) {
        if (pressed_window >= 0 && over_space >= 0) {
            auto* window = m_thumbnails[static_cast<size_t>(pressed_window)].window;
            if (over_space == space_count) {
                Space space;
                space.id = m_next_space_id++;
                where.spaces.append(move(space));
            }
            move_window_to_space(*window, where, over_space);
        }
        update_overview_overlays();
        return true;
    }
    if (over_close && over_space >= 0) {
        remove_space(where, over_space);
        return true;
    }
    if (over_space == space_count) {
        Space space;
        space.id = m_next_space_id++;
        where.spaces.append(move(space));
        update_overview_overlays();
        shell_state_changed();
        return true;
    }
    if (over_space >= 0 && pressed_tile == over_space) {
        go_to_space(where, over_space, false);
        // Its windows now, on their way back to where they are.
        collect_overview_windows();
        layout_overview();
        leave_overview(nullptr);
        return true;
    }
    if (over_window >= 0 && pressed_window == over_window) {
        auto* window = m_thumbnails[static_cast<size_t>(over_window)].window;
        if (window->minimized)
            restore_window(*window);
        leave_overview(window);
        return true;
    }
    if (over_space < 0)
        leave_overview(nullptr);
    return true;
}

}
