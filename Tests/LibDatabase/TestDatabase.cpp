/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/FastRandom.h>
#include <LibBase/HashMap.h>
#include <LibDatabase/BTree.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/Pager.h>
#include <LibTest/TestCase.h>

extern "C" {
struct FILE;
FILE* fopen(char const* path, char const* mode);
unsigned long fwrite(void const* buffer, unsigned long size, unsigned long count, FILE*);
int fclose(FILE*);
char* getenv(char const*);
int getpid();
int unlink(char const*);
}

using namespace Database;

namespace {

// A file in the temporary directory, gone (with its journal) at the end.
class TemporaryDatabase {
public:
    TemporaryDatabase()
    {
        static int number = 0;
        StringBuilder path;
        char const* directory = getenv("TMPDIR");
        path.appendff("{}/vulture-database-test-{}-{}.db", directory ? StringView(directory, __builtin_strlen(directory)) : "/tmp"_sv, getpid(), number++);
        m_path = MUST(path.to_string());
        StringBuilder journal;
        journal.appendff("{}-journal", m_path.view());
        m_journal = MUST(journal.to_string());
    }
    ~TemporaryDatabase()
    {
        unlink(m_path.characters());
        unlink(m_journal.characters());
    }

    [[nodiscard]] StringView path() const { return m_path.view(); }

private:
    String m_path;
    String m_journal;
};

i64 single_integer(::Database::Database& database, StringView sql)
{
    auto rows = database.query(sql);
    if (rows.is_error() || rows.value().size() != 1) {
        Test::print("query failed: "_sv);
        Test::print(database.last_error());
        Test::print("\n"_sv);
        return -1;
    }
    return rows.value()[0][0].to_integer();
}

bool integrity_is_ok(::Database::Database& database)
{
    auto rows = database.query("PRAGMA integrity_check"_sv);
    if (rows.is_error() || rows.value().size() != 1)
        return false;
    if (rows.value()[0][0].as_text() != "ok"_sv) {
        Test::print(rows.value()[0][0].as_text());
        Test::print("\n"_sv);
        return false;
    }
    return true;
}

}

TEST_CASE(what_keys_sort_as_is_what_values_compare_as)
{
    Vector<Value> values;
    values.append(Value {});
    values.append(Value::integer(NumericLimits<i64>::min()));
    values.append(Value::real(-1e300));
    values.append(Value::integer(-5));
    values.append(Value::real(-4.5));
    values.append(Value::integer(0));
    values.append(Value::real(0.5));
    values.append(Value::integer(1));
    values.append(Value::real(1.5));
    values.append(Value::integer(9007199254740993ll)); // 2^53 + 1: a double can't tell it from the next.
    values.append(Value::real(9007199254740992.0));
    values.append(Value::integer(NumericLimits<i64>::max()));
    values.append(Value::text(""_sv));
    values.append(Value::text("a"_sv));
    values.append(Value::text(StringView("a\0b", 3)));
    values.append(Value::text("ab"_sv));
    values.append(Value::blob({}));
    values.append(Value::blob("z"_sv.bytes()));

    for (size_t i = 0; i < values.size(); ++i) {
        for (size_t j = 0; j < values.size(); ++j) {
            Vector<u8> a;
            Vector<u8> b;
            encode_key(values[i], a);
            encode_key(values[j], b);
            size_t common = min(a.size(), b.size());
            int bytes = __builtin_memcmp(a.data(), b.data(), common);
            if (bytes == 0)
                bytes = a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
            int by_value = compare(values[i], values[j]);
            EXPECT_EQ(bytes < 0, by_value < 0);
            EXPECT_EQ(bytes > 0, by_value > 0);
        }
    }
}

TEST_CASE(records_say_what_was_written)
{
    Vector<Value> values;
    values.append(Value {});
    values.append(Value::integer(0));
    values.append(Value::integer(1));
    values.append(Value::integer(-129));
    values.append(Value::integer(70000));
    values.append(Value::integer(NumericLimits<i64>::min()));
    values.append(Value::real(2.25));
    values.append(Value::text("hello"_sv));
    values.append(Value::blob("\x01\x02"_sv.bytes()));
    Vector<u8> record;
    encode_record(values.span(), record);
    Vector<Value> decoded;
    MUST(decode_record(record.span(), decoded));
    EXPECT_EQ(decoded.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT(decoded[i].type() == values[i].type());
        EXPECT_EQ(compare(decoded[i], values[i]), 0);
    }
}

TEST_CASE(a_tree_agrees_with_a_map_through_a_lot_of_changes)
{
    TemporaryDatabase file;
    auto pager = MUST(Pager::open(file.path()));
    MUST(pager->begin());
    u32 root = MUST(BTree::create(*pager));
    BTree tree(*pager, root);

    // Long and short keys and values, some of them longer than a page.
    HashMap<String, String> model;
    Base::FastRandom random(1234);
    auto key_for = [&](u32 n) {
        StringBuilder key;
        key.appendff("key-{:08}", n);
        for (u32 i = 0; i < n % 37; ++i)
            key.append("padding-"_sv);
        return MUST(key.to_string());
    };
    for (u32 round = 0; round < 40000; ++round) {
        u32 n = static_cast<u32>(random.next()) % 6000;
        auto key = key_for(n);
        if (random.next() % 3 == 0) {
            bool expected = model.remove(key.view());
            EXPECT_EQ(MUST(tree.remove(key.view().bytes())), expected);
        } else {
            StringBuilder value;
            u32 length = random.next() % 100 == 0 ? 20000 : random.next() % 300;
            for (u32 i = 0; i < length; ++i)
                value.append(static_cast<char>('a' + (round + i) % 26));
            auto text = MUST(value.to_string());
            MUST(tree.insert(key.view().bytes(), text.view().bytes()));
            model.set(move(key), move(text));
        }
    }
    EXPECT_EQ(MUST(tree.check()), model.size());

    // Everything can be found, by key and by walking; and what's walked is in order, either way.
    size_t walked = 0;
    Vector<u8> previous_key;
    auto cursor = MUST(tree.first());
    Vector<u8> value;
    while (cursor.is_valid()) {
        auto key = cursor.key();
        auto as_view = StringView(reinterpret_cast<char const*>(key.data()), key.size());
        auto const* expected = model.get(as_view);
        EXPECT(expected != nullptr);
        MUST(cursor.value(value));
        if (expected)
            EXPECT(StringView(reinterpret_cast<char const*>(value.data()), value.size()) == expected->view());
        if (walked > 0)
            EXPECT(StringView(reinterpret_cast<char const*>(previous_key.data()), previous_key.size()).compare(as_view) < 0);
        previous_key.clear();
        for (auto byte : key)
            previous_key.append(byte);
        ++walked;
        MUST(cursor.next());
    }
    EXPECT_EQ(walked, model.size());

    size_t backwards = 0;
    auto reverse = MUST(tree.last());
    while (reverse.is_valid()) {
        ++backwards;
        MUST(reverse.previous());
    }
    EXPECT_EQ(backwards, model.size());

    // And then it's emptied, and every page but the root is given back.
    model.for_each([&](String const& key, String const&) { EXPECT(MUST(tree.remove(key.view().bytes()))); });
    EXPECT_EQ(MUST(tree.check()), 0u);
    EXPECT(pager->free_page_count() + 2 >= pager->page_count() - 1);
    MUST(pager->commit());
}

TEST_CASE(what_was_committed_is_there_when_it_is_opened_again)
{
    TemporaryDatabase file;
    {
        auto database = MUST(::Database::Database::open(file.path()));
        MUST(database->execute("CREATE TABLE things(id INTEGER PRIMARY KEY, name TEXT UNIQUE, size INTEGER)"_sv));
        MUST(database->execute("CREATE INDEX by_size ON things(size)"_sv));
        auto insert = MUST(database->prepare("INSERT INTO things(name, size) VALUES (?, ?)"_sv));
        MUST(database->execute("BEGIN"_sv));
        for (int i = 0; i < 5000; ++i) {
            StringBuilder name;
            name.appendff("thing {}", i);
            MUST(insert->bind(1, Value::text(name.view())));
            MUST(insert->bind(2, Value::integer(i % 100)));
            insert->reset();
            (void)MUST(insert->step());
        }
        MUST(database->execute("COMMIT"_sv));
    }
    auto database = MUST(::Database::Database::open(file.path()));
    EXPECT_EQ(single_integer(*database, "SELECT count(*) FROM things"_sv), 5000);
    EXPECT_EQ(single_integer(*database, "SELECT count(*) FROM things WHERE size = 7"_sv), 50);
    EXPECT_EQ(single_integer(*database, "SELECT size FROM things WHERE name = 'thing 1234'"_sv), 34);
    EXPECT(integrity_is_ok(*database));
}

TEST_CASE(parameters_and_reuse)
{
    TemporaryDatabase file;
    auto database = MUST(::Database::Database::open(file.path()));
    MUST(database->execute("CREATE TABLE t(a, b)"_sv));
    Value values[] = { Value::integer(1), Value::text("x"_sv) };
    MUST(database->execute("INSERT INTO t VALUES (?, ?)"_sv, Span<Value const>(values, 2)));
    auto select = MUST(database->prepare("SELECT a, b FROM t WHERE a = ?"_sv));
    MUST(select->bind(1, Value::integer(1)));
    EXPECT(MUST(select->step()));
    EXPECT_EQ(select->column_count(), 2u);
    EXPECT(select->column_name(1) == "b"_sv);
    EXPECT(select->column(1).as_text() == "x"_sv);
    EXPECT(!MUST(select->step()));
    select->reset();
    MUST(select->bind(1, Value::integer(2)));
    EXPECT(!MUST(select->step()));
    EXPECT(select->bind(2, Value {}).is_error());
}

TEST_CASE(errors_say_what_is_wrong)
{
    TemporaryDatabase file;
    auto database = MUST(::Database::Database::open(file.path()));
    EXPECT(database->execute("SELEC 1"_sv).is_error());
    EXPECT(database->last_error().contains("SELEC"_sv));
    EXPECT(database->execute("SELECT * FROM nothing"_sv).is_error());
    EXPECT(database->last_error() == "no such table: nothing"_sv);
    MUST(database->execute("CREATE TABLE t(a TEXT NOT NULL)"_sv));
    EXPECT(database->execute("INSERT INTO t VALUES (NULL)"_sv).is_error());
    EXPECT(database->last_error() == "NOT NULL constraint failed: t.a"_sv);
    EXPECT(database->execute("SELECT b FROM t"_sv).is_error());
    EXPECT(database->last_error() == "no such column: b"_sv);
    // None of which left anything behind.
    EXPECT_EQ(single_integer(*database, "SELECT count(*) FROM t"_sv), 0);
    EXPECT(integrity_is_ok(*database));
}

TEST_CASE(a_statement_that_fails_part_way_does_nothing)
{
    TemporaryDatabase file;
    auto database = MUST(::Database::Database::open(file.path()));
    MUST(database->execute("CREATE TABLE t(a INTEGER UNIQUE)"_sv));
    EXPECT(database->execute("INSERT INTO t VALUES (1), (2), (1)"_sv).is_error());
    EXPECT_EQ(single_integer(*database, "SELECT count(*) FROM t"_sv), 0);
    EXPECT(integrity_is_ok(*database));
}

TEST_CASE(a_crash_at_any_moment_leaves_the_last_transaction_or_the_one_before)
{
    // The same work again and again, the machine going off one write further along each time. Afterwards there's either
    // all of the transaction or none of it, and a database that's in one piece.
    for (i64 writes = 1; writes < 400; writes += 3) {
        TemporaryDatabase file;
        {
            auto database = MUST(::Database::Database::open(file.path()));
            MUST(database->execute("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT UNIQUE, n INTEGER)"_sv));
            MUST(database->execute("CREATE INDEX t_n ON t(n)"_sv));
            MUST(database->execute("BEGIN"_sv));
            for (int i = 0; i < 300; ++i) {
                StringBuilder sql;
                sql.appendff("INSERT INTO t(name, n) VALUES ('name number {} with some length to it', {})", i, i * 2);
                MUST(database->execute(sql.view()));
            }
            MUST(database->execute("COMMIT"_sv));

            database->crash_after_writes_for_testing(writes);
            bool failed = false;
            StringView const statements[] = { "BEGIN"_sv, "UPDATE t SET n = n + 1"_sv, "DELETE FROM t WHERE id % 3 = 0"_sv, "INSERT INTO t(name, n) VALUES ('after', -1)"_sv, "COMMIT"_sv };
            for (auto statement : statements) {
                if (database->execute(statement).is_error()) {
                    failed = true;
                    break;
                }
            }
            (void)failed;
            // Gone, with nothing tidied up on the way: as far as the disk knows this is the end of it.
            (void)database.leak_ptr();
        }
        auto database = MUST(::Database::Database::open(file.path()));
        EXPECT(integrity_is_ok(*database));
        i64 count = single_integer(*database, "SELECT count(*) FROM t"_sv);
        i64 sum = single_integer(*database, "SELECT sum(n) FROM t"_sv);
        i64 after = single_integer(*database, "SELECT count(*) FROM t WHERE name = 'after'"_sv);
        bool none_of_it = count == 300 && after == 0 && sum == 299 * 300;
        bool all_of_it = count == 201 && after == 1;
        if (!none_of_it && !all_of_it)
            Test::fail(__FILE__, __LINE__, "the transaction half happened"_sv);
    }
}

TEST_CASE(a_torn_journal_from_an_older_transaction_is_not_applied)
{
    TemporaryDatabase file;
    {
        auto database = MUST(::Database::Database::open(file.path()));
        MUST(database->execute("CREATE TABLE t(x)"_sv));
        MUST(database->execute("INSERT INTO t VALUES (1)"_sv));
    }
    // Rubbish where the journal goes is not a journal.
    StringBuilder journal_path;
    journal_path.appendff("{}-journal", file.path());
    auto terminated = MUST(journal_path.to_string());
    FILE* journal = fopen(terminated.characters(), "wb");
    EXPECT(journal != nullptr);
    fwrite("this is not a journal at all, not by any means", 1, 46, journal);
    fclose(journal);
    auto database = MUST(::Database::Database::open(file.path()));
    EXPECT_EQ(single_integer(*database, "SELECT count(*) FROM t"_sv), 1);
    EXPECT(integrity_is_ok(*database));
}

TEST_CASE(not_a_database_is_refused)
{
    TemporaryDatabase file;
    auto terminated = MUST(String::from_view(file.path()));
    FILE* output = fopen(terminated.characters(), "wb");
    EXPECT(output != nullptr);
    char junk[9000];
    __builtin_memset(junk, 'x', sizeof(junk));
    fwrite(junk, 1, sizeof(junk), output);
    fclose(output);
    EXPECT(::Database::Database::open(file.path()).is_error());
}

TEST_CASE(a_tree_with_nothing_left_in_a_page_is_still_a_tree)
{
    TemporaryDatabase file;
    auto database = MUST(::Database::Database::open(file.path()));
    MUST(database->execute("CREATE TABLE t(id INTEGER PRIMARY KEY, x TEXT)"_sv));
    MUST(database->execute("CREATE INDEX t_x ON t(x)"_sv));
    MUST(database->execute("BEGIN"_sv));
    for (int i = 0; i < 3000; ++i) {
        StringBuilder sql;
        sql.appendff("INSERT INTO t(x) VALUES ('value {}')", i);
        MUST(database->execute(sql.view()));
    }
    MUST(database->execute("COMMIT"_sv));
    MUST(database->execute("DELETE FROM t WHERE id > 100 AND id <= 2900"_sv));
    EXPECT(integrity_is_ok(*database));
    EXPECT_EQ(single_integer(*database, "SELECT count(*) FROM t"_sv), 200);
    EXPECT_EQ(single_integer(*database, "SELECT id FROM t WHERE x = 'value 2950'"_sv), 2951);
    MUST(database->execute("DROP TABLE t"_sv));
    MUST(database->execute("CREATE TABLE u(a)"_sv));
    MUST(database->execute("INSERT INTO u VALUES (1)"_sv));
    EXPECT_EQ(single_integer(*database, "SELECT a FROM u"_sv), 1);
    EXPECT(integrity_is_ok(*database));
}
