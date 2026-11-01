/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibBase/QuickSort.h>
#include <LibGfx/Rasterizer.h>

namespace Gfx {

namespace Math = Base::Math;

Rasterizer::Rasterizer(IntRect clip)
    : m_clip(clip)
{
}

void Rasterizer::add_path(Path const& path, float tolerance)
{
    FloatPoint first {};
    FloatPoint last {};
    bool open = false;
    auto finish_subpath = [&] {
        if (open && last != first)
            add_line(last, first);
        open = false;
    };
    path.flatten(tolerance, [&](FloatPoint point, bool starts_subpath) {
        if (starts_subpath) {
            finish_subpath();
            first = point;
            open = true;
        } else {
            add_line(last, point);
        }
        last = point; }, [&] { finish_subpath(); last = first; });
    finish_subpath();
}

void Rasterizer::add_line(FloatPoint from, FloatPoint to)
{
    if (from.y == to.y)
        return;
    float direction = 1;
    if (from.y > to.y) {
        swap(from, to);
        direction = -1;
    }
    if (to.y <= static_cast<float>(m_clip.y) || from.y >= static_cast<float>(m_clip.bottom()))
        return;
    m_edges.append(Edge { from.x - static_cast<float>(m_clip.x), from.y, to.y, (to.x - from.x) / (to.y - from.y), direction });
}

// One edge's piece in one row: from x_from to x_to (any order), rising by
// `height`. A pixel's coverage is the height risen to its left plus what the
// piece adds within it; the accumulation buffer holds the difference from the
// previous pixel, so a later running sum recovers it.
void Rasterizer::accumulate(float x_from, float x_to, float height)
{
    float left = min(x_from, x_to);
    float right = max(x_from, x_to);
    auto width = static_cast<float>(m_clip.width);
    auto* cells = m_accumulation.data();
    auto note_touched = [&](int first, int last) {
        m_touched_left = min(m_touched_left, first);
        m_touched_right = max(m_touched_right, last);
    };
    // Left of the clip, a piece counts wholly at the first pixel; right of it,
    // not at all.
    if (right <= 0) {
        cells[0] += height;
        note_touched(0, 0);
        return;
    }
    if (left >= width)
        return;
    if (left < 0) {
        float outside = -left / (right - left);
        cells[0] += height * outside;
        note_touched(0, 0);
        height *= 1 - outside;
        left = 0;
    }
    if (right > width) {
        height *= (width - left) / (right - left);
        right = width;
    }

    int first = static_cast<int>(left);
    int last = static_cast<int>(right);
    if (static_cast<float>(last) == right && last > first)
        --last;
    note_touched(first, last + 1);
    if (first == last) {
        // Within one pixel: the part to its right side is what it covers.
        float middle = (left + right) / 2 - static_cast<float>(first);
        cells[first] += height * (1 - middle);
        cells[first + 1] += height * middle;
        return;
    }
    float rise_per_pixel = height / (right - left);
    // The first pixel: a triangle.
    float into_first = static_cast<float>(first + 1) - left;
    float previous = rise_per_pixel * into_first * into_first / 2;
    cells[first] += previous;
    // Those in between: the height at their middle.
    for (int x = first + 1; x < last; ++x) {
        float covered = rise_per_pixel * (static_cast<float>(x) + 0.5f - left);
        cells[x] += covered - previous;
        previous = covered;
    }
    // The last pixel: what is risen on entering it, plus a trapezoid.
    float risen = rise_per_pixel * (static_cast<float>(last) - left);
    float into_last = right - static_cast<float>(last);
    float covered = (risen + height) / 2 * into_last + height * (1 - into_last);
    cells[last] += covered - previous;
    cells[last + 1] += height - covered;
}

void Rasterizer::run(FillRule rule, RowCallback on_row, void* context)
{
    if (m_edges.is_empty() || m_clip.is_empty())
        return;
    quick_sort(m_edges.span(), [](Edge const& a, Edge const& b) { return a.y0 < b.y0; });
    MUST(m_accumulation.try_resize(static_cast<size_t>(m_clip.width) + 2));
    MUST(m_coverage.try_resize(static_cast<size_t>(m_clip.width) + 1));
    __builtin_memset(m_accumulation.data(), 0, m_accumulation.size() * sizeof(float));

    Vector<u32> active;
    size_t next_edge = 0;
    int y = max(m_clip.y, static_cast<int>(Math::floor(m_edges[0].y0)));
    while (y < m_clip.bottom()) {
        auto row_top = static_cast<float>(y);
        auto row_bottom = static_cast<float>(y + 1);
        while (next_edge < m_edges.size() && m_edges[next_edge].y0 < row_bottom)
            active.append(static_cast<u32>(next_edge++));
        if (active.is_empty()) {
            if (next_edge == m_edges.size())
                return;
            y = static_cast<int>(Math::floor(m_edges[next_edge].y0));
            continue;
        }

        m_touched_left = m_clip.width;
        m_touched_right = -1;
        size_t kept = 0;
        for (size_t i = 0; i < active.size(); ++i) {
            auto const& edge = m_edges[active[i]];
            if (edge.y1 <= row_top)
                continue;
            float top = max(row_top, edge.y0);
            float bottom = min(row_bottom, edge.y1);
            if (bottom > top) {
                float x_top = edge.x0 + (top - edge.y0) * edge.slope;
                float x_bottom = edge.x0 + (bottom - edge.y0) * edge.slope;
                accumulate(x_top, x_bottom, (bottom - top) * edge.direction);
            }
            if (edge.y1 > row_bottom)
                active[kept++] = active[i];
        }
        while (active.size() > kept)
            active.take_last();

        if (m_touched_right >= m_touched_left) {
            int stop = min(m_touched_right, m_clip.width - 1);
            float* cells = m_accumulation.data();
            u8* coverage = m_coverage.data();
            float sum = 0;
            auto covered = [rule](float winding) {
                float amount = Math::absolute(winding);
                if (rule == FillRule::EvenOdd) {
                    amount -= 2 * Math::floor(amount / 2);
                    if (amount > 1)
                        amount = 2 - amount;
                } else if (amount > 1) {
                    amount = 1;
                }
                return static_cast<u8>(amount * 255 + 0.5f);
            };
            for (int x = m_touched_left; x <= stop; ++x) {
                sum += cells[x];
                coverage[x] = covered(sum);
            }
            // Past the last edge the sum only changes by what rounding left.
            int end = stop + 1;
            u8 rest = covered(sum);
            if (rest != 0) {
                for (; end < m_clip.width; ++end)
                    coverage[end] = rest;
            }
            on_row(context, y, m_clip.x + m_touched_left, end - m_touched_left, coverage + m_touched_left);
            __builtin_memset(cells + m_touched_left, 0, static_cast<size_t>(m_touched_right - m_touched_left + 2) * sizeof(float));
        }
        ++y;
    }
}

}
