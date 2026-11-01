/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

// /dev/gpu0: the display. mmap() it for the framebuffer (BGRX, 32 bits); what
// is drawn there shows after VULTURE_GPU_FLUSH.

struct vulture_gpu_rect {
    unsigned x;
    unsigned y;
    unsigned width;
    unsigned height;
};

struct vulture_gpu_mode {
    unsigned width;
    unsigned height;
    unsigned pitch; // In bytes; out.
    unsigned long size; // Of the framebuffer, in bytes; out.
};

struct vulture_gpu_cursor {
    void const* pixels; // 64 x 64 premultiplied BGRA.
    unsigned hot_x;
    unsigned hot_y;
};

struct vulture_gpu_position {
    unsigned x;
    unsigned y;
};

// The display's preferred size (`vulture_gpu_mode`; pitch and size for it).
#define VULTURE_GPU_GET_DISPLAY 0x4701
// A new framebuffer of this size, shown at once. Earlier mappings stay valid
// but no longer show.
#define VULTURE_GPU_SET_MODE 0x4702
// Shows what changed in a rectangle (`vulture_gpu_rect`).
#define VULTURE_GPU_FLUSH 0x4703
#define VULTURE_GPU_SET_CURSOR 0x4704
#define VULTURE_GPU_MOVE_CURSOR 0x4705

#define VULTURE_GPU_CURSOR_SIZE 64
