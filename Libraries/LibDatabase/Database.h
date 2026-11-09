/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/OwnPtr.h>
#include <LibBase/Span.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibDatabase/SQL.h>
#include <LibDatabase/Value.h>

// An embedded SQL database in one file, for what an application or service keeps. Statements mean what they do in SQLite,
// and a transaction either happens or doesn't, whatever the power does.
//
//     auto database = TRY(Database::Database::open("/private/var/db/things.db"_sv));
//     TRY(database->execute("CREATE TABLE IF NOT EXISTS things(id INTEGER PRIMARY KEY, name TEXT)"_sv));
//     auto insert = TRY(database->prepare("INSERT INTO things(name) VALUES (?)"_sv));
//     TRY(insert->bind(1, Database::Value::text("one"_sv)));
//     (void)TRY(insert->step());
//
// Not there: subqueries, views, triggers, foreign keys.

namespace Database {

class Pager;
class Database;
class Executor;
struct Catalogue;

using Row = Vector<Value>;

class Statement {
public:
    ~Statement();

    // From 1, as SQL counts them.
    ErrorOr<void> bind(size_t parameter, Value);
    // True for a row, which column() then reads; false when there are no more.
    ErrorOr<bool> step();
    // Ready to run again, with the same parameters unless they're changed.
    void reset();

    [[nodiscard]] size_t column_count() const { return m_column_names.size(); }
    [[nodiscard]] StringView column_name(size_t index) const { return m_column_names[index].view(); }
    [[nodiscard]] Value const& column(size_t index) const { return m_results[m_next_row - 1][index]; }

private:
    friend class Database;
    friend class Executor;
    Statement(Database& database, SQL::Statement statement)
        : m_database(database)
        , m_statement(move(statement))
    {
    }

    Database& m_database;
    SQL::Statement m_statement;
    Vector<Value> m_parameters;
    Vector<String> m_column_names;
    Vector<Row> m_results;
    size_t m_next_row { 0 };
    bool m_executed { false };
};

class Database {
public:
    static ErrorOr<NonnullOwnPtr<Database>> open(StringView path);
    ~Database();

    ErrorOr<NonnullOwnPtr<Statement>> prepare(StringView);
    // Runs one statement to its end.
    ErrorOr<void> execute(StringView, Span<Value const> parameters = {});
    // Runs one statement, and gives every row of it.
    ErrorOr<Vector<Row>> query(StringView, Span<Value const> parameters = {});

    // What went wrong, in words, with the last thing that did.
    [[nodiscard]] StringView last_error() const { return m_last_error.view(); }
    [[nodiscard]] i64 last_insert_rowid() const { return m_last_insert_rowid; }
    [[nodiscard]] u64 changes() const { return m_changes; }
    [[nodiscard]] bool in_transaction() const { return m_explicit_transaction; }

    // How much of the file to keep in memory.
    void set_cache_size(size_t bytes);

    // Tests: see Pager.
    void crash_after_writes_for_testing(i64);

private:
    friend class Statement;
    friend class Executor;
    Database(NonnullOwnPtr<Pager>);

    ErrorOr<void> load_catalogue();
    Error sql_error(StringView message);

    NonnullOwnPtr<Pager> m_pager;
    OwnPtr<Catalogue> m_catalogue;
    String m_last_error;
    i64 m_last_insert_rowid { 0 };
    u64 m_changes { 0 };
    bool m_explicit_transaction { false };
};

}
