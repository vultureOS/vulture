/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// What a Quick View plugin is: a library in /System/Library/QuickView, or /Library/QuickView, that says which things it can
// picture, and pictures them. It's loaded into a process of its own that can't see anyone's files: it's given the one it's
// asked about, already open, and if it crashes, that process is all that goes.

struct VultureQuickViewPicture {
    unsigned width;
    unsigned height;
    // malloc()ed; 0xAARRGGBB, a row after another.
    unsigned* pixels;
};

extern "C" {

// Whether this plugin can picture a file of this type, whose name ends in this.
int vulture_quickview_claims(char const* content_type, char const* extension);

// Fits it in this many pixels: 0, and `picture` is filled in, or an errno.
int vulture_quickview_render(int document, char const* content_type, unsigned most_width, unsigned most_height, VultureQuickViewPicture* picture);

}
