/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Compare.h"

namespace ImgDiff {

namespace {

bool is_blank(char character)
{
    return character == ' ' || character == '\t';
}

// The next word of a line, and what's left after it.
StringView take_word(StringView& line)
{
    size_t start = 0;
    while (start < line.length() && is_blank(line[start]))
        ++start;
    size_t end = start;
    while (end < line.length() && !is_blank(line[end]))
        ++end;
    auto word = line.substring_view(start, end - start);
    line = line.substring_view(end);
    return word;
}

bool inside(Region const& region, int x, int y)
{
    return x >= region.x && y >= region.y && x < region.x + region.width && y < region.y + region.height;
}

}

ErrorOr<Vector<Region>> parse_regions(StringView text)
{
    Vector<Region> regions;
    size_t position = 0;
    while (position < text.length()) {
        auto newline = text.find('\n', position);
        size_t const end = newline.has_value() ? newline.value() : text.length();
        auto line = text.substring_view(position, end - position).trim_whitespace();
        position = end + 1;
        if (line.is_empty() || line.starts_with('#'))
            continue;
        Region region;
        auto const kind = take_word(line);
        if (kind == "strict"_sv)
            region.kind = RegionKind::Strict;
        else if (kind == "ignore"_sv)
            region.kind = RegionKind::Ignore;
        else
            return Error::from_string_literal("a region must be \"strict\" or \"ignore\"");
        auto const x = take_word(line).to_number<int>();
        auto const y = take_word(line).to_number<int>();
        auto const width = take_word(line).to_number<int>();
        auto const height = take_word(line).to_number<int>();
        if (!x.has_value() || !y.has_value() || !width.has_value() || !height.has_value())
            return Error::from_string_literal("a region needs X Y WIDTH HEIGHT");
        if (x.value() < 0 || y.value() < 0 || width.value() <= 0 || height.value() <= 0)
            return Error::from_string_literal("a region must be on the picture, and not empty");
        region.x = x.value();
        region.y = y.value();
        region.width = width.value();
        region.height = height.value();
        region.name = TRY(String::from_view(line.trim_whitespace()));
        TRY(regions.try_append(move(region)));
    }
    return regions;
}

Result compare(Gfx::Bitmap const& a, Gfx::Bitmap const& b, Options const& options, Gfx::Bitmap* difference)
{
    VERIFY(a.size() == b.size());
    int const width = a.width();
    int const height = a.height();

    Result result;
    result.total = static_cast<u64>(width) * static_cast<u64>(height);
    MUST(result.strict_failures.try_resize(0));
    Vector<StrictFailure> strict;
    for (size_t i = 0; i < options.regions.size(); ++i) {
        if (options.regions[i].kind == RegionKind::Strict) {
            StrictFailure entry;
            entry.region_index = i;
            strict.append(entry);
        }
    }

    // Off pixels, for finding what they touch. Only ones outside everything ignored.
    Vector<u8> off_here;
    MUST(off_here.try_resize(static_cast<size_t>(result.total)));

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t const index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            off_here[index] = 0;
            Gfx::Pixel const pa = a.pixel_at(x, y), pb = b.pixel_at(x, y);
            auto distance = [](u8 first, u8 second) { return first > second ? first - second : second - first; };
            int const red = distance(Gfx::pixel_red(pa), Gfx::pixel_red(pb));
            int const green = distance(Gfx::pixel_green(pa), Gfx::pixel_green(pb));
            int const blue = distance(Gfx::pixel_blue(pa), Gfx::pixel_blue(pb));
            int const alpha = distance(Gfx::pixel_alpha(pa), Gfx::pixel_alpha(pb));
            int const most = max(max(red, green), max(blue, alpha));

            bool ignored = false;
            for (auto const& region : options.regions) {
                if (region.kind == RegionKind::Ignore && inside(region, x, y)) {
                    ignored = true;
                    break;
                }
            }
            if (difference) {
                auto amplified = [&](int value) { return static_cast<u8>(ignored ? 0 : min(value * 16, 255)); };
                difference->set_pixel_at(x, y, Gfx::make_pixel(amplified(red), amplified(green), amplified(blue), 255));
            }
            if (ignored)
                continue;
            if (most > result.worst) {
                result.worst = most;
                result.worst_x = x;
                result.worst_y = y;
            }
            if (most <= options.max_channel)
                continue;
            ++result.off;
            off_here[index] = 1;
            for (auto& entry : strict) {
                if (inside(options.regions[entry.region_index], x, y)) {
                    if (entry.off == 0) {
                        entry.first_x = x;
                        entry.first_y = y;
                    }
                    ++entry.off;
                }
            }
        }
    }

    // The groups of off pixels, touching at edges or corners. (By a list rather than recursion: one may be the whole picture.)
    Vector<u32> pending;
    for (size_t start = 0; start < off_here.size(); ++start) {
        if (!off_here[start])
            continue;
        u64 size = 0;
        off_here[start] = 0;
        pending.append(static_cast<u32>(start));
        while (!pending.is_empty()) {
            size_t const index = pending.take_last();
            ++size;
            int const x = static_cast<int>(index % static_cast<size_t>(width));
            int const y = static_cast<int>(index / static_cast<size_t>(width));
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int const nx = x + dx;
                    int const ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    size_t const neighbour = static_cast<size_t>(ny) * static_cast<size_t>(width) + static_cast<size_t>(nx);
                    if (off_here[neighbour]) {
                        off_here[neighbour] = 0;
                        pending.append(static_cast<u32>(neighbour));
                    }
                }
            }
        }
        if (size > result.largest_cluster) {
            result.largest_cluster = size;
            result.cluster_x = static_cast<int>(start % static_cast<size_t>(width));
            result.cluster_y = static_cast<int>(start / static_cast<size_t>(width));
        }
    }

    result.allowed = static_cast<u64>(static_cast<double>(result.total) * options.max_percent / 100);
    double const percent = result.total ? 100.0 * static_cast<double>(result.off) / static_cast<double>(result.total) : 0;
    result.scatter_ok = percent <= options.max_percent;
    result.cluster_ok = options.max_cluster < 0 || result.largest_cluster <= static_cast<u64>(options.max_cluster);
    for (auto const& entry : strict) {
        if (entry.off > 0)
            result.strict_failures.append(entry);
    }
    return result;
}

}
