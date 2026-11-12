/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>

namespace FilesApp {

extern bool s_still;

// One of the first few of a stack, as it's drawn on top of the pile.
struct StackTop {
    String path;
    String icon;
    i64 modified { 0 };
    bool has_thumbnail { false };
};

struct FileItem {
    String name;
    String path;
    String icon;
    bool is_directory { false };
    bool is_alias { false };
    // One of the disks shown on the desktop, rather than anything in its folder.
    bool is_volume { false };
    u64 size { 0 };
    i64 modified { 0 };
    i64 created { 0 };
    bool selected { false };
    // What it looks like, rather than an icon: pictures, and text, as Finder's are.
    bool has_thumbnail { false };
    Vector<String> tags;
    // On the desktop, in Stacks: what's shown for all of one kind.
    bool is_stack { false };
    int stack_count { 0 };
    Vector<StackTop> stack_tops;
};

// A folder that's really a question: smart:NAME|QUERY, in the language of mdfind.
constexpr StringView smart_prefix = "smart:"_sv;
[[nodiscard]] bool is_smart_path(StringView);
[[nodiscard]] StringView smart_name(StringView);
[[nodiscard]] StringView smart_query(StringView);
[[nodiscard]] String recents_path();
[[nodiscard]] String tag_path(StringView tag);
constexpr StringView saved_search_extension = ".savedSearch"_sv;
// What a Smart Folder's file holds, as a place to go.
[[nodiscard]] Optional<String> smart_path_from_file(StringView path);
ErrorOr<String> save_smart_folder(StringView name, StringView query);

[[nodiscard]] String joined(StringView directory, StringView name);
[[nodiscard]] StringView last_component(StringView path);
[[nodiscard]] String parent_of(StringView path);
[[nodiscard]] bool sorts_before(StringView a, StringView b);
[[nodiscard]] Optional<FileItem> item_at_path(StringView path, bool show_hidden_name = true);
[[nodiscard]] Vector<FileItem> list_folder(StringView path, bool show_hidden = false);
[[nodiscard]] String size_text(u64 bytes);
[[nodiscard]] String date_text(i64 seconds);

ErrorOr<void> copy_file(StringView from, StringView to);
ErrorOr<void> remove_tree(StringView path);
[[nodiscard]] bool exists(StringView path);
// A name not yet used in the folder: "untitled folder", "untitled folder 2", ...
[[nodiscard]] String free_name(StringView directory, StringView wanted);
// "report copy.txt", "report copy 2.txt".
[[nodiscard]] String free_copy_name(StringView directory, StringView original);
ErrorOr<void> move_item(StringView from, StringView to);

}
