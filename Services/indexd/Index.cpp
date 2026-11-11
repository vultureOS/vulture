/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Index.h"
#include "Importers.h"
#include "QueryLanguage.h"
#include <Desktop.h>
#include <Tags.h>
#include <LibBase/Function.h>
#include <LibBase/HashMap.h>
#include <LibBase/QuickSort.h>
#include <LibJSON/JSON.h>
#include <sys/stat.h>

using Database::Value;

namespace Index {

namespace {

constexpr StringView schema[] = {
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value)"_sv,
    "CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, name TEXT NOT NULL, lowered_name TEXT NOT NULL, "
    "kind TEXT NOT NULL, content_type TEXT, kind_description TEXT, size INTEGER, created INTEGER, modified INTEGER, "
    "pixel_width INTEGER, pixel_height INTEGER, version TEXT, generation INTEGER)"_sv,
    "CREATE INDEX IF NOT EXISTS items_lowered_name ON items(lowered_name)"_sv,
    "CREATE TABLE IF NOT EXISTS name_words(word TEXT NOT NULL, item INTEGER NOT NULL, PRIMARY KEY(word, item))"_sv,
    "CREATE INDEX IF NOT EXISTS name_words_item ON name_words(item)"_sv,
    "CREATE TABLE IF NOT EXISTS content_words(word TEXT NOT NULL, item INTEGER NOT NULL, PRIMARY KEY(word, item))"_sv,
    "CREATE INDEX IF NOT EXISTS content_words_item ON content_words(item)"_sv,
};

// As much of the query as goes to any one kind of match while typing: it's what keeps a letter from reading everything.
constexpr size_t single_term_limit = 256;
constexpr size_t several_terms_limit = 2048;
constexpr size_t most_candidates = 512;

constexpr int score_name_starts = 100;
constexpr int score_word_in_name = 60;
constexpr int score_whole_word_in_text = 40;
constexpr int score_word_start_in_text = 20;
constexpr int score_application = 50;

// The first string after everything that starts with this.
String after_prefix(StringView prefix)
{
    StringBuilder out;
    out.append(prefix);
    auto text = MUST(out.to_string());
    auto* characters = const_cast<char*>(text.characters());
    size_t length = text.length();
    while (length > 0 && static_cast<u8>(characters[length - 1]) == 0xff)
        --length;
    if (length == 0)
        return MUST(String::from_view("\xff\xff\xff\xff"_sv));
    characters[length - 1] = static_cast<char>(static_cast<u8>(characters[length - 1]) + 1);
    return MUST(String::from_view(text.view().substring_view(0, length)));
}

StringView last_component(StringView path)
{
    auto slash = path.find_last('/');
    return slash.has_value() ? path.substring_view(slash.value() + 1) : path;
}

// What's typed, in words: the last may be only the start of one.
void split_terms(StringView folded, Vector<String>& terms)
{
    size_t index = 0;
    auto is_separator = [](char c) { return static_cast<u8>(c) < 0x80 && !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')); };
    while (index < folded.length()) {
        while (index < folded.length() && is_separator(folded[index]))
            ++index;
        size_t start = index;
        while (index < folded.length() && !is_separator(folded[index]))
            ++index;
        if (index > start)
            terms.append(MUST(String::from_view(folded.substring_view(start, index - start))));
    }
}

template<typename... Values>
ErrorOr<void> bind_all(Database::Statement& statement, Values... values)
{
    statement.reset();
    size_t index = 0;
    ErrorOr<void> result = {};
    ((result.is_error() ? void() : void(result = statement.bind(++index, move(values)))), ...);
    return result;
}

}

Store::Store(NonnullOwnPtr<Database::Database> database)
    : m_database(move(database))
{
}

ErrorOr<NonnullOwnPtr<Store>> Store::open(StringView path)
{
    auto database = TRY(Database::Database::open(path));
    database->set_cache_size(16 * MiB);
    for (auto statement : schema)
        TRY(database->execute(statement));
    auto* raw = new (Base::nothrow) Store(move(database));
    if (!raw)
        return Error::from_errno(12);
    auto store = adopt_own(*raw);
    // (An index from before there were tags.)
    (void)store->m_database->execute("ALTER TABLE items ADD COLUMN tags TEXT"_sv);
    TRY(store->prepare_statements());
    auto files = TRY(store->m_database->query("SELECT count(*) FROM items"_sv));
    store->m_file_count = static_cast<u64>(files[0][0].to_integer());
    store->m_word_count = static_cast<u64>(store->meta_integer("word_count"_sv).value_or(0));
    return store;
}

ErrorOr<void> Store::prepare_statements()
{
    auto& d = *m_database;
    m_find_by_path = TRY(d.prepare("SELECT id, size, modified FROM items WHERE path = ?"_sv));
    m_touch = TRY(d.prepare("UPDATE items SET generation = ? WHERE id = ?"_sv));
    m_insert_item = TRY(d.prepare("INSERT INTO items(path, name, lowered_name, kind, content_type, kind_description, size, created, modified, pixel_width, pixel_height, version, generation, tags) "
                                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"_sv));
    m_remove_item = TRY(d.prepare("DELETE FROM items WHERE id = ?"_sv));
    m_insert_name_word = TRY(d.prepare("INSERT OR IGNORE INTO name_words(word, item) VALUES (?, ?)"_sv));
    m_insert_content_word = TRY(d.prepare("INSERT OR IGNORE INTO content_words(word, item) VALUES (?, ?)"_sv));
    m_remove_name_words = TRY(d.prepare("DELETE FROM name_words WHERE item = ?"_sv));
    m_remove_content_words = TRY(d.prepare("DELETE FROM content_words WHERE item = ?"_sv));
    m_by_name_prefix = TRY(d.prepare("SELECT id FROM items WHERE lowered_name >= ? AND lowered_name < ? LIMIT ?"_sv));
    m_name_word_range = TRY(d.prepare("SELECT item FROM name_words WHERE word >= ? AND word < ? LIMIT ?"_sv));
    m_name_word_exact = TRY(d.prepare("SELECT item FROM name_words WHERE word = ? LIMIT ?"_sv));
    m_content_word_range = TRY(d.prepare("SELECT item, word FROM content_words WHERE word >= ? AND word < ? LIMIT ?"_sv));
    m_content_word_exact = TRY(d.prepare("SELECT item, word FROM content_words WHERE word = ? LIMIT ?"_sv));
    m_item_by_id = TRY(d.prepare("SELECT path, name, kind, modified FROM items WHERE id = ?"_sv));
    m_items_under = TRY(d.prepare("SELECT id FROM items WHERE path = ? OR (path >= ? AND path < ?)"_sv));
    m_get_meta = TRY(d.prepare("SELECT value FROM meta WHERE key = ?"_sv));
    m_set_meta = TRY(d.prepare("INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)"_sv));
    return {};
}

ErrorOr<void> Store::begin()
{
    return m_database->execute("BEGIN"_sv);
}

ErrorOr<void> Store::commit()
{
    TRY(set_meta_integer("word_count"_sv, static_cast<i64>(m_word_count)));
    return m_database->execute("COMMIT"_sv);
}

Optional<i64> Store::meta_integer(StringView key)
{
    if (bind_all(*m_get_meta, Value::text(key)).is_error())
        return {};
    auto stepped = m_get_meta->step();
    if (stepped.is_error() || !stepped.value() || m_get_meta->column(0).is_null())
        return {};
    return m_get_meta->column(0).to_integer();
}

ErrorOr<void> Store::set_meta_integer(StringView key, i64 value)
{
    TRY(bind_all(*m_set_meta, Value::text(key), Value::integer(value)));
    (void)TRY(m_set_meta->step());
    return {};
}

ErrorOr<void> Store::insert_words(Database::Statement& statement, i64 item, Vector<String> const& words)
{
    for (auto const& word : words) {
        TRY(bind_all(statement, Value::text(word.view()), Value::integer(item)));
        (void)TRY(statement.step());
    }
    return {};
}

ErrorOr<void> Store::remove_item(i64 id)
{
    TRY(bind_all(*m_remove_name_words, Value::integer(id)));
    (void)TRY(m_remove_name_words->step());
    TRY(bind_all(*m_remove_content_words, Value::integer(id)));
    (void)TRY(m_remove_content_words->step());
    m_word_count -= min(m_word_count, m_database->changes());
    TRY(bind_all(*m_remove_item, Value::integer(id)));
    (void)TRY(m_remove_item->step());
    if (m_file_count > 0)
        --m_file_count;
    return {};
}

ErrorOr<void> Store::put_path(StringView path, i64 generation, bool even_if_unchanged)
{
    auto terminated = TRY(String::from_view(path));
    struct stat information;
    if (lstat(terminated.characters(), &information) != 0)
        return forget_path(path);

    i64 const size = S_ISDIR(information.st_mode) ? 0 : static_cast<i64>(information.st_size);
    i64 const modified = static_cast<i64>(information.st_mtime);

    TRY(bind_all(*m_find_by_path, Value::text(path)));
    if (TRY(m_find_by_path->step())) {
        i64 const id = m_find_by_path->column(0).to_integer();
        if (!even_if_unchanged && m_find_by_path->column(1).to_integer() == size && m_find_by_path->column(2).to_integer() == modified) {
            TRY(bind_all(*m_touch, Value::integer(generation), Value::integer(id)));
            (void)TRY(m_touch->step());
            return {};
        }
        TRY(remove_item(id));
    }

    auto name = last_component(path);
    bool const is_directory = S_ISDIR(information.st_mode);
    bool const is_application = is_directory && name.ends_with(".app"_sv);

    Metadata metadata;
    import_file(path, metadata);
    Vector<String> name_words;
    words_of_name(name, name_words);
    if (is_application) {
        // An application can be found by what it is, as well as by what it's called.
        if (auto bundle = Desktop::read_bundle(path); !bundle.is_error()) {
            words_of_name(bundle.value().name.view(), name_words);
            words_of_name(bundle.value().kind.view(), name_words);
        }
    }

    StringBuilder tags_builder;
    for (auto const& tag : Desktop::tags_of(path)) {
        if (tags_builder.length())
            tags_builder.append(',');
        tags_builder.append(tag.view());
    }
    auto tags_text = MUST(tags_builder.to_string());
    auto optional_integer = [](Optional<i64> const& number) { return number.has_value() ? Value::integer(number.value()) : Value {}; };
    TRY(bind_all(*m_insert_item,
        Value::text(path), Value::text(name), Value::text(fold(name).view()),
        Value::text(is_application ? "application"_sv : (is_directory ? "folder"_sv : "document"_sv)),
        Value::text(metadata.content_type.view()), Value::text(metadata.kind_description.view()),
        Value::integer(size), Value::integer(static_cast<i64>(information.st_ctime)), Value::integer(modified),
        optional_integer(metadata.pixel_width), optional_integer(metadata.pixel_height),
        metadata.version.is_empty() ? Value {} : Value::text(metadata.version.view()),
        Value::integer(generation), tags_text.is_empty() ? Value {} : Value::text(tags_text.view())));
    (void)TRY(m_insert_item->step());
    i64 const id = m_database->last_insert_rowid();
    ++m_file_count;
    TRY(insert_words(*m_insert_name_word, id, name_words));
    TRY(insert_words(*m_insert_content_word, id, metadata.words));
    m_word_count += metadata.words.size();
    return {};
}

ErrorOr<void> Store::forget_path(StringView path)
{
    StringBuilder under;
    under.append(path);
    under.append('/');
    auto start = TRY(under.to_string());
    auto end = after_prefix(start.view());
    TRY(bind_all(*m_items_under, Value::text(path), Value::text(start.view()), Value::text(end.view())));
    Vector<i64> ids;
    while (TRY(m_items_under->step()))
        ids.append(m_items_under->column(0).to_integer());
    for (auto id : ids)
        TRY(remove_item(id));
    return {};
}

ErrorOr<void> Store::forget_older_than(i64 generation)
{
    Value parameter = Value::integer(generation);
    auto rows = TRY(m_database->query("SELECT id FROM items WHERE generation < ?"_sv, Span<Value const>(&parameter, 1)));
    for (auto const& row : rows)
        TRY(remove_item(row[0].to_integer()));
    return {};
}

void Store::collect_prefix_matches(StringView term, bool is_prefix, size_t tier_limit, Function<void(i64, int)> const& found)
{
    auto end = after_prefix(term);
    auto run_names = [&](Database::Statement& statement, int score) {
        for (;;) {
            auto stepped = statement.step();
            if (stepped.is_error() || !stepped.value())
                return;
            found(statement.column(0).to_integer(), score);
        }
    };
    if (is_prefix) {
        (void)bind_all(*m_name_word_range, Value::text(term), Value::text(end.view()), Value::integer(static_cast<i64>(tier_limit)));
        run_names(*m_name_word_range, score_word_in_name);
    } else {
        (void)bind_all(*m_name_word_exact, Value::text(term), Value::integer(static_cast<i64>(tier_limit)));
        run_names(*m_name_word_exact, score_word_in_name);
    }

    auto& content = is_prefix ? *m_content_word_range : *m_content_word_exact;
    if (is_prefix)
        (void)bind_all(content, Value::text(term), Value::text(end.view()), Value::integer(static_cast<i64>(tier_limit)));
    else
        (void)bind_all(content, Value::text(term), Value::integer(static_cast<i64>(tier_limit)));
    for (;;) {
        auto stepped = content.step();
        if (stepped.is_error() || !stepped.value())
            return;
        found(content.column(0).to_integer(), content.column(1).as_text() == term ? score_whole_word_in_text : score_word_start_in_text);
    }
}

String Store::query(StringView text, size_t limit)
{
    auto results = JSON::Value::array();
    auto folded = fold(text.trim_whitespace());
    if (folded.is_empty())
        return MUST(results.serialize());

    Vector<String> terms;
    split_terms(folded.view(), terms);

    // The best each file has done, in the first term; then only what's in all of them.
    HashMap<i64, int> best;
    size_t const tier_limit = terms.size() > 1 ? several_terms_limit : single_term_limit;
    for (size_t t = 0; t < terms.size(); ++t) {
        bool const is_last = t + 1 == terms.size();
        HashMap<i64, int> these;
        collect_prefix_matches(terms[t].view(), is_last, tier_limit, [&](i64 item, int score) {
            auto* existing = these.get(item);
            if (!existing || *existing < score)
                these.set(item, score);
        });
        if (t == 0) {
            best = move(these);
            continue;
        }
        HashMap<i64, int> both;
        these.for_each([&](i64 item, int score) {
            if (auto const* earlier = best.get(item))
                both.set(item, *earlier + score);
        });
        best = move(both);
    }

    // What starts with all of it, spaces and all.
    auto end = after_prefix(folded.view());
    (void)bind_all(*m_by_name_prefix, Value::text(folded.view()), Value::text(end.view()), Value::integer(64));
    for (;;) {
        auto stepped = m_by_name_prefix->step();
        if (stepped.is_error() || !stepped.value())
            break;
        i64 item = m_by_name_prefix->column(0).to_integer();
        auto* existing = best.get(item);
        if (!existing || *existing < score_name_starts)
            best.set(item, score_name_starts);
    }

    struct Candidate {
        i64 item;
        int score;
    };
    Vector<Candidate> candidates;
    best.for_each([&](i64 item, int score) { candidates.append(Candidate { item, score }); });
    quick_sort(candidates.span(), [](Candidate const& a, Candidate const& b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.item < b.item;
    });

    struct Hit {
        String path;
        String name;
        String kind;
        int score;
        i64 modified;
    };
    Vector<Hit> hits;
    for (size_t i = 0; i < candidates.size() && i < most_candidates; ++i) {
        if (bind_all(*m_item_by_id, Value::integer(candidates[i].item)).is_error())
            continue;
        auto stepped = m_item_by_id->step();
        if (stepped.is_error() || !stepped.value())
            continue;
        Hit hit;
        hit.path = MUST(String::from_view(m_item_by_id->column(0).as_text()));
        hit.name = MUST(String::from_view(m_item_by_id->column(1).as_text()));
        hit.kind = MUST(String::from_view(m_item_by_id->column(2).as_text()));
        hit.score = candidates[i].score + (hit.kind.view() == "application"_sv ? score_application : 0);
        hit.modified = m_item_by_id->column(3).to_integer();
        hits.append(move(hit));
    }
    quick_sort(hits.span(), [](Hit const& a, Hit const& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.modified != b.modified)
            return a.modified > b.modified;
        return a.name.view().compare(b.name.view()) < 0;
    });

    for (size_t i = 0; i < hits.size() && i < limit; ++i) {
        auto one = JSON::Value::object();
        MUST(one.set("path"_sv, MUST(JSON::Value::string(hits[i].path.view()))));
        MUST(one.set("name"_sv, MUST(JSON::Value::string(hits[i].name.view()))));
        MUST(one.set("kind"_sv, MUST(JSON::Value::string(hits[i].kind.view()))));
        MUST(results.append(move(one)));
    }
    return MUST(results.serialize());
}

String Store::find(StringView query_text, StringView only_in, bool name_only, size_t limit, String& error)
{
    auto results = JSON::Value::array();
    StringBuilder sql;
    Vector<Value> parameters;
    sql.append("SELECT path FROM items WHERE "_sv);

    Vector<i64> items;
    bool const by_words = !name_only && !looks_like_query_language(query_text);
    if (name_only) {
        StringBuilder pattern;
        pattern.appendff("*{}*", query_text);
        sql.append("lower(name) GLOB lower(?)"_sv);
        parameters.append(Value::text(pattern.view()));
    } else if (!by_words) {
        auto translated = translate_query(query_text, error);
        if (translated.is_error())
            return MUST(results.serialize());
        sql.append('(');
        sql.append(translated.value().where.view());
        sql.append(')');
        parameters = move(translated.value().parameters);
    }

    if (by_words) {
        Vector<String> terms;
        auto folded = fold(query_text.trim_whitespace());
        split_terms(folded.view(), terms);
        HashMap<i64, int> matching;
        for (size_t t = 0; t < terms.size(); ++t) {
            HashMap<i64, int> these;
            collect_prefix_matches(terms[t].view(), true, 1 << 20, [&](i64 item, int score) { these.set(item, score); });
            if (t == 0) {
                matching = move(these);
                continue;
            }
            HashMap<i64, int> both;
            these.for_each([&](i64 item, int score) {
                if (matching.contains(item))
                    both.set(item, score);
            });
            matching = move(both);
        }
        matching.for_each([&](i64 item, int) { items.append(item); });
        quick_sort(items.span(), [](i64 a, i64 b) { return a < b; });
        for (auto item : items) {
            if (results.size() >= limit)
                break;
            if (bind_all(*m_item_by_id, Value::integer(item)).is_error())
                continue;
            auto stepped = m_item_by_id->step();
            if (stepped.is_error() || !stepped.value())
                continue;
            auto path = m_item_by_id->column(0).as_text();
            if (!only_in.is_empty() && !path.starts_with(only_in))
                continue;
            MUST(results.append(MUST(JSON::Value::string(path))));
        }
        return MUST(results.serialize());
    }

    String after;
    if (!only_in.is_empty()) {
        StringBuilder under;
        under.append(only_in);
        if (!only_in.ends_with('/'))
            under.append('/');
        auto start = MUST(under.to_string());
        after = after_prefix(start.view());
        sql.append(" AND path >= ? AND path < ?"_sv);
        parameters.append(Value::text(start.view()));
        parameters.append(Value::text(after.view()));
    }
    sql.append(" ORDER BY path LIMIT ?"_sv);
    parameters.append(Value::integer(static_cast<i64>(limit)));
    auto rows = m_database->query(sql.view(), parameters.span());
    if (rows.is_error()) {
        error = MUST(String::from_view(m_database->last_error()));
        return MUST(results.serialize());
    }
    for (auto const& row : rows.value())
        MUST(results.append(MUST(JSON::Value::string(row[0].as_text()))));
    return MUST(results.serialize());
}

String Store::attributes(StringView path)
{
    Value parameter = Value::text(path);
    auto rows = m_database->query("SELECT name, kind_description, content_type, size, created, modified, pixel_width, pixel_height, version, path FROM items WHERE path = ?"_sv, Span<Value const>(&parameter, 1));
    if (rows.is_error() || rows.value().is_empty())
        return MUST(String::from_view("null"_sv));
    auto const& row = rows.value()[0];
    auto out = JSON::Value::object();
    auto put_text = [&](StringView key, Value const& value) {
        if (!value.is_null())
            MUST(out.set(key, MUST(JSON::Value::string(value.as_text()))));
    };
    auto put_number = [&](StringView key, Value const& value) {
        if (!value.is_null())
            MUST(out.set(key, JSON::Value(static_cast<double>(value.to_integer()))));
    };
    put_text("kMDItemDisplayName"_sv, row[0]);
    put_text("kMDItemFSName"_sv, row[0]);
    put_text("kMDItemKind"_sv, row[1]);
    put_text("kMDItemContentType"_sv, row[2]);
    put_number("kMDItemFSSize"_sv, row[3]);
    put_number("kMDItemContentCreationDate"_sv, row[4]);
    put_number("kMDItemFSCreationDate"_sv, row[4]);
    put_number("kMDItemContentModificationDate"_sv, row[5]);
    put_number("kMDItemFSContentChangeDate"_sv, row[5]);
    put_number("kMDItemPixelWidth"_sv, row[6]);
    put_number("kMDItemPixelHeight"_sv, row[7]);
    put_text("kMDItemVersion"_sv, row[8]);
    put_text("kMDItemPath"_sv, row[9]);
    return MUST(out.serialize());
}

}
