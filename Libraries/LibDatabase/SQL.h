/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibDatabase/Value.h>

// The SQL this database speaks, as what's parsed out of the text: the language of SQLite, as far as an application's own
// data needs, with the same answers.

namespace Database::SQL {

enum class ExpressionKind : u8 {
    Literal,
    Parameter,
    Column,
    Negate,
    Not,
    BitwiseNot,
    Binary,
    Function,
    IsNull,
    Like,
    Between,
    In,
    Case,
    Cast,
    Star,
};

enum class Operator : u8 {
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Concatenate,
    Equal,
    NotEqual,
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
    And,
    Or,
    BitAnd,
    BitOr,
    ShiftLeft,
    ShiftRight,
    Is,
    IsNot,
};

struct Expression {
    ExpressionKind kind { ExpressionKind::Literal };
    Value literal;
    size_t parameter { 0 };
    String table;
    String name;
    Operator op { Operator::Add };
    bool negated { false };
    bool distinct { false };
    bool glob { false };
    Vector<NonnullOwnPtr<Expression>> children;
    // For Case: whether there's a value being compared against, first; and an ELSE, last.
    bool has_operand { false };
    bool has_else { false };

    // Worked out once the tables are known.
    int table_index { -1 };
    int column_index { -2 };
    Affinity affinity { Affinity::Blob };
    bool has_affinity { false };
    int aggregate_index { -1 };
};

using ExpressionPointer = NonnullOwnPtr<Expression>;

struct ResultColumn {
    OwnPtr<Expression> expression;
    String alias;
    // `*`, or `t.*`.
    bool star { false };
    String star_table;
};

struct Join {
    String table;
    String alias;
    OwnPtr<Expression> on;
    bool left { false };
};

struct OrderTerm {
    ExpressionPointer expression;
    bool descending { false };
};

struct Select {
    bool distinct { false };
    Vector<ResultColumn> columns;
    String table;
    String alias;
    bool has_from { false };
    Vector<Join> joins;
    OwnPtr<Expression> where;
    Vector<ExpressionPointer> group_by;
    OwnPtr<Expression> having;
    Vector<OrderTerm> order_by;
    OwnPtr<Expression> limit;
    OwnPtr<Expression> offset;
};

struct ColumnDefinition {
    String name;
    String type;
    bool not_null { false };
    bool unique { false };
    bool primary_key { false };
    bool autoincrement { false };
    OwnPtr<Expression> default_value;
};

struct CreateTable {
    String name;
    bool if_not_exists { false };
    Vector<ColumnDefinition> columns;
    // Across several columns.
    Vector<Vector<String>> unique_sets;
    Vector<String> primary_key;
};

struct CreateIndex {
    String name;
    String table;
    bool unique { false };
    bool if_not_exists { false };
    Vector<String> columns;
};

enum class ConflictAction : u8 {
    Abort,
    Replace,
    Ignore,
};

struct Insert {
    String table;
    ConflictAction on_conflict { ConflictAction::Abort };
    Vector<String> columns;
    Vector<Vector<ExpressionPointer>> rows;
    OwnPtr<Select> select;
    bool default_values { false };
};

struct Assignment {
    String column;
    ExpressionPointer value;
};

struct Update {
    String table;
    ConflictAction on_conflict { ConflictAction::Abort };
    Vector<Assignment> assignments;
    OwnPtr<Expression> where;
};

struct Delete {
    String table;
    OwnPtr<Expression> where;
};

enum class StatementKind : u8 {
    Select,
    CreateTable,
    CreateIndex,
    DropTable,
    DropIndex,
    AddColumn,
    Insert,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback,
    IntegrityCheck,
};

struct Statement {
    StatementKind kind { StatementKind::Select };
    OwnPtr<Select> select;
    OwnPtr<CreateTable> create_table;
    OwnPtr<CreateIndex> create_index;
    OwnPtr<Insert> insert;
    OwnPtr<Update> update;
    OwnPtr<Delete> remove;
    String name;
    bool if_exists { false };
    OwnPtr<ColumnDefinition> added_column;
    size_t parameter_count { 0 };
    // What a table or index was made with, as it's kept in the catalogue.
    String text;
};

// One statement. `message` says what's wrong with it, if it can't be parsed.
ErrorOr<Statement> parse(StringView text, String& message);

}
