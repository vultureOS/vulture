/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibUI/Window.h>

// The things done to files that take a while, or ask something, or both.

namespace FilesApp {

enum class TransferKind : u8 {
    Copy,
    Move,
};

// Into that folder. A big one has a window saying how it's going; what's there already is asked about, over `parent`.
void start_transfer(TransferKind, Vector<String> sources, StringView folder, UI::Window& parent, Function<void()> done);

void move_to_trash(Span<String const> paths);
[[nodiscard]] bool can_put_back(StringView trashed);
ErrorOr<void> put_back(StringView trashed);
// It asks first.
void empty_trash(UI::Window& parent, Function<void()> done);

// A copy that shares its blocks with the original, where the disk can do that.
ErrorOr<String> duplicate(StringView path);
ErrorOr<String> make_alias(StringView path);

// All of these are done in a process of their own, that can reach only what it's working on.
void compress(Vector<String> paths, Function<void()> done);
void expand(StringView archive, Function<void()> done);
void rotate_picture(StringView path, bool clockwise, Function<void()> done);

}
