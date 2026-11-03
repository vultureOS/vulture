/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>
#include <LibGfx/Bitmap.h>

// Do two pictures match, as golden tests count it?
//
// Three rules, each catching what the others can't:
//
// - Scattered pixels: at most a small share of the picture may be off. Rounding differs a little between machines, and
//   that's all this is for. It's far too generous to be the only rule: 0.1% of a 2560x1600 screen is 4,096 pixels, enough
//   to lose a checkmark or a tag's dot and still "match".
// - Clusters: no touching group of off pixels may be larger than a few. Rounding scatters; something that has really
//   changed comes in a lump.
// - Regions: what a golden says about particular parts of itself. In a strict one nothing may be off at all; an ignored
//   one (a clock, say) counts for nothing.

namespace ImgDiff {

enum class RegionKind : u8 {
    Strict,
    Ignore,
};

struct Region {
    RegionKind kind { RegionKind::Strict };
    int x { 0 };
    int y { 0 };
    int width { 0 };
    int height { 0 };
    String name;
};

struct Options {
    // How far a channel may be off before the pixel counts as off.
    int max_channel { 2 };
    double max_percent { 0.1 };
    // The largest touching group of off pixels allowed. Negative for no limit.
    int max_cluster { 8 };
    Vector<Region> regions;
};

struct StrictFailure {
    size_t region_index { 0 };
    u64 off { 0 };
    int first_x { 0 };
    int first_y { 0 };
};

struct Result {
    u64 total { 0 };
    u64 off { 0 };
    u64 allowed { 0 };
    int worst { 0 };
    int worst_x { 0 };
    int worst_y { 0 };
    u64 largest_cluster { 0 };
    int cluster_x { 0 };
    int cluster_y { 0 };
    Vector<StrictFailure> strict_failures;
    bool scatter_ok { true };
    bool cluster_ok { true };

    [[nodiscard]] bool matches() const { return scatter_ok && cluster_ok && strict_failures.is_empty(); }
};

// One to a line, in pixels: "strict X Y WIDTH HEIGHT name of it" or "ignore ...". Blank lines and "#" comments are skipped.
[[nodiscard]] ErrorOr<Vector<Region>> parse_regions(StringView text);

// The pictures must be the same size. `difference`, if given and the same size too, gets what's different, amplified,
// for a person to look at.
[[nodiscard]] Result compare(Gfx::Bitmap const& a, Gfx::Bitmap const& b, Options const&, Gfx::Bitmap* difference = nullptr);

}
