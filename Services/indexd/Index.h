/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/Optional.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibDatabase/Database.h>

// What's known of the files on the system, in a database in the person's Library: one row for each, and the words of their
// names and their text, each with the file it's in. Prefixes of those are what makes finding something as fast as typing it.

namespace Index {

class Store {
public:
    static ErrorOr<NonnullOwnPtr<Store>> open(StringView path);

    ErrorOr<void> begin();
    ErrorOr<void> commit();

    // What's at the path now, if it's changed since it was last looked at, or nothing if there's nothing there.
    // `generation` is which pass over the disk has seen it.
    ErrorOr<void> put_path(StringView path, i64 generation);
    // It, and everything under it.
    ErrorOr<void> forget_path(StringView path);
    // What a whole pass over the disk didn't come across.
    ErrorOr<void> forget_older_than(i64 generation);

    [[nodiscard]] String query(StringView text, size_t limit);
    // `error` says what's wrong with the query, if anything is.
    [[nodiscard]] String find(StringView query, StringView only_in, bool name_only, size_t limit, String& error);
    [[nodiscard]] String attributes(StringView path);

    [[nodiscard]] Optional<i64> meta_integer(StringView key);
    ErrorOr<void> set_meta_integer(StringView key, i64);

    [[nodiscard]] u64 file_count() const { return m_file_count; }
    [[nodiscard]] u64 word_count() const { return m_word_count; }

private:
    using StatementPointer = NonnullOwnPtr<Database::Statement>;

    Store(NonnullOwnPtr<Database::Database>);

    ErrorOr<void> prepare_statements();
    ErrorOr<void> insert_words(Database::Statement&, i64 item, Vector<String> const& words);
    ErrorOr<void> remove_item(i64 id);
    void collect_prefix_matches(StringView term, bool is_prefix, size_t tier_limit, Function<void(i64 item, int score)> const&);

    NonnullOwnPtr<Database::Database> m_database;
    OwnPtr<Database::Statement> m_find_by_path;
    OwnPtr<Database::Statement> m_touch;
    OwnPtr<Database::Statement> m_insert_item;
    OwnPtr<Database::Statement> m_remove_item;
    OwnPtr<Database::Statement> m_insert_name_word;
    OwnPtr<Database::Statement> m_insert_content_word;
    OwnPtr<Database::Statement> m_remove_name_words;
    OwnPtr<Database::Statement> m_remove_content_words;
    OwnPtr<Database::Statement> m_by_name_prefix;
    OwnPtr<Database::Statement> m_name_word_range;
    OwnPtr<Database::Statement> m_name_word_exact;
    OwnPtr<Database::Statement> m_content_word_range;
    OwnPtr<Database::Statement> m_content_word_exact;
    OwnPtr<Database::Statement> m_item_by_id;
    OwnPtr<Database::Statement> m_items_under;
    OwnPtr<Database::Statement> m_get_meta;
    OwnPtr<Database::Statement> m_set_meta;
    u64 m_file_count { 0 };
    u64 m_word_count { 0 };
};

}
