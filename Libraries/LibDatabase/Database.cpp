/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Function.h>
#include <LibBase/HashMap.h>
#include <LibBase/NumericLimits.h>
#include <LibBase/QuickSort.h>
#include <LibDatabase/BTree.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/Pager.h>

namespace Database {

using SQL::Assignment;
using SQL::ColumnDefinition;
using SQL::ConflictAction;
using SQL::CreateIndex;
using SQL::CreateTable;
using SQL::Delete;
using SQL::Expression;
using SQL::ExpressionKind;
using SQL::Insert;
using SQL::Operator;
using SQL::parse;
using SQL::Select;
using SQL::StatementKind;
using SQL::Update;

namespace {

const Error sql_failure = Error::from_string_literal("database: the statement failed");

// The catalogue's own table: what tables and indexes there are, and how they were made.
enum CatalogueColumn {
    CatalogueKind,
    CatalogueName,
    CatalogueTable,
    CatalogueRoot,
    CatalogueText,
};

}

struct ColumnInformation {
    String name;
    String type;
    Affinity affinity { Affinity::Blob };
    bool not_null { false };
    OwnPtr<Expression> default_value;
};

struct IndexInformation {
    String name;
    String table;
    Vector<int> columns;
    bool unique { false };
    bool automatic { false };
    u32 root { 0 };
};

struct TableInformation {
    String name;
    u32 root { 0 };
    i64 catalogue_rowid { 0 };
    String text;
    Vector<ColumnInformation> columns;
    // Which column is the rowid under another name, if any is.
    int rowid_column { -1 };
    Vector<OwnPtr<IndexInformation>> indexes;
};

struct Catalogue {
    Vector<OwnPtr<TableInformation>> tables;

    TableInformation* find_table(StringView name)
    {
        for (auto& table : tables) {
            if (table->name.view().equals_ignoring_ascii_case(name))
                return table.ptr();
        }
        return nullptr;
    }

    IndexInformation* find_index(StringView name, TableInformation** owner = nullptr)
    {
        for (auto& table : tables) {
            for (auto& index : table->indexes) {
                if (index->name.view().equals_ignoring_ascii_case(name)) {
                    if (owner)
                        *owner = table.ptr();
                    return index.ptr();
                }
            }
        }
        return nullptr;
    }
};

static int column_index_in(TableInformation const& table, StringView name)
{
    for (size_t i = 0; i < table.columns.size(); ++i) {
        if (table.columns[i].name.view().equals_ignoring_ascii_case(name))
            return static_cast<int>(i);
    }
    return -1;
}

static bool is_rowid_name(StringView name)
{
    return name.equals_ignoring_ascii_case("rowid"_sv) || name.equals_ignoring_ascii_case("_rowid_"_sv) || name.equals_ignoring_ascii_case("oid"_sv);
}

static constexpr size_t after_value_padding = 9;

static void append_after_value(Vector<u8>& key)
{
    for (size_t i = 0; i < after_value_padding; ++i)
        key.append(0xff);
}

static int compare_key_bytes(ReadonlyBytes a, ReadonlyBytes b)
{
    size_t common = min(a.size(), b.size());
    int result = common ? __builtin_memcmp(a.data(), b.data(), common) : 0;
    if (result != 0)
        return result;
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

// The characters of a text, for what counts them: bytes that don't continue one begin one.
static size_t utf8_length(StringView text)
{
    size_t count = 0;
    for (char c : text) {
        if ((static_cast<u8>(c) & 0xc0) != 0x80)
            ++count;
    }
    return count;
}

static size_t utf8_offset(StringView text, size_t characters)
{
    size_t offset = 0;
    while (offset < text.length() && characters > 0) {
        ++offset;
        while (offset < text.length() && (static_cast<u8>(text[offset]) & 0xc0) == 0x80)
            ++offset;
        --characters;
    }
    return offset;
}

static char ascii_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

static char ascii_upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
}

static bool like_matches(StringView pattern, StringView text)
{
    size_t p = 0;
    size_t t = 0;
    size_t star_p = NumericLimits<size_t>::max();
    size_t star_t = 0;
    while (t < text.length()) {
        if (p < pattern.length() && pattern[p] == '%') {
            star_p = p++;
            star_t = t;
        } else if (p < pattern.length() && pattern[p] == '_') {
            ++p;
            t += utf8_offset(text.substring_view(t), 1);
        } else if (p < pattern.length() && ascii_lower(pattern[p]) == ascii_lower(text[t])) {
            ++p;
            ++t;
        } else if (star_p != NumericLimits<size_t>::max()) {
            p = star_p + 1;
            star_t += utf8_offset(text.substring_view(star_t), 1);
            t = star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.length() && pattern[p] == '%')
        ++p;
    return p == pattern.length();
}

static bool glob_class_matches(StringView pattern, size_t& p, char c)
{
    // At the character after '['; leaves it after the ']'.
    bool invert = false;
    bool matched = false;
    if (p < pattern.length() && pattern[p] == '^') {
        invert = true;
        ++p;
    }
    bool first = true;
    while (p < pattern.length() && (pattern[p] != ']' || first)) {
        first = false;
        char low = pattern[p++];
        char high = low;
        if (p + 1 < pattern.length() && pattern[p] == '-' && pattern[p + 1] != ']') {
            high = pattern[p + 1];
            p += 2;
        }
        if (static_cast<u8>(c) >= static_cast<u8>(low) && static_cast<u8>(c) <= static_cast<u8>(high))
            matched = true;
    }
    if (p < pattern.length())
        ++p;
    return matched != invert;
}

static bool glob_matches(StringView pattern, size_t p, StringView text, size_t t)
{
    while (p < pattern.length()) {
        char c = pattern[p];
        if (c == '*') {
            while (p < pattern.length() && pattern[p] == '*')
                ++p;
            if (p == pattern.length())
                return true;
            for (size_t start = t; start <= text.length(); ++start) {
                if (glob_matches(pattern, p, text, start))
                    return true;
            }
            return false;
        }
        if (t >= text.length())
            return false;
        if (c == '?') {
            ++p;
            t += utf8_offset(text.substring_view(t), 1);
        } else if (c == '[') {
            ++p;
            if (!glob_class_matches(pattern, p, text[t]))
                return false;
            ++t;
        } else {
            if (c != text[t])
                return false;
            ++p;
            ++t;
        }
    }
    return t == text.length();
}

// ---- Running a statement.

namespace {

struct TableContext {
    TableInformation* table { nullptr };
    StringView name;
    Vector<Value> values;
    i64 rowid { 0 };
    bool is_null_row { false };
    bool left_join { false };
};

struct Aggregate {
    Expression const* expression { nullptr };
};

struct AggregateState {
    i64 count { 0 };
    i64 integer_sum { 0 };
    double real_sum { 0 };
    bool sum_is_real { false };
    bool sum_overflowed { false };
    Value best;
    bool has_best { false };
    StringBuilder* text { nullptr };
    bool has_text { false };
    HashMap<String, bool>* distinct { nullptr };
};

struct Group {
    Vector<Vector<Value>> row_values;
    Vector<i64> row_ids;
    Vector<bool> row_nulls;
    Vector<AggregateState> states;
    Vector<u8> key;
};

struct AccessPath {
    enum class Kind : u8 {
        Scan,
        RowidEqual,
        RowidRange,
        Index,
    };
    Kind kind { Kind::Scan };
    IndexInformation* index { nullptr };
    Vector<Expression const*> equals;
    Expression const* in_list { nullptr };
    Expression const* lower { nullptr };
    bool lower_inclusive { false };
    Expression const* upper { nullptr };
    bool upper_inclusive { false };
    bool reverse { false };
};

struct Conjunct {
    Expression const* expression;
    int level;
    // From a LEFT JOIN's ON: it says whether its table's row matched, and not whether the row as a whole is wanted.
    bool is_on { false };
    int on_level { -1 };
};

struct SortableRow {
    Row result;
    Row keys;
};

}

class Executor {
public:
    Executor(Database& database, Statement& statement)
        : m_database(database)
        , m_statement(statement)
        , m_pager(*database.m_pager)
        , m_catalogue(*database.m_catalogue)
    {
    }

    ErrorOr<void> run();

private:
    Error fail(StringView message) { return m_database.sql_error(message); }

    // Everything's in one transaction, the statement's own if nothing else has one going.
    template<typename Body>
    ErrorOr<void> in_transaction(Body body);

    ErrorOr<void> run_select(Select const&, Vector<Row>& results, bool name_columns);
    ErrorOr<void> run_create_table(CreateTable const&, StringView text);
    ErrorOr<void> run_create_index(CreateIndex const&, StringView text);
    ErrorOr<void> run_drop_table(StringView name, bool if_exists);
    ErrorOr<void> run_drop_index(StringView name, bool if_exists);
    ErrorOr<void> run_add_column(StringView table, ColumnDefinition const&, StringView text);
    ErrorOr<void> run_insert(Insert const&);
    ErrorOr<void> run_update(Update const&);
    ErrorOr<void> run_delete(Delete const&);
    ErrorOr<void> run_integrity_check();

    ErrorOr<void> resolve(Expression&, bool allow_aggregates);
    ErrorOr<Value> evaluate(Expression const&);
    ErrorOr<Value> evaluate_binary(Expression const&);
    ErrorOr<Value> evaluate_function(Expression const&);
    ErrorOr<Value> compare_operands(Expression const&, Value& left, Value& right);
    void apply_comparison_affinity(Expression const& left_expression, Expression const& right_expression, Value& left, Value& right);

    ErrorOr<void> collect_aggregates(Expression&);
    void find_conjuncts(Expression const&, Vector<Expression const*>&);
    int highest_level_of(Expression const&);
    ErrorOr<AccessPath> choose_path(size_t level, Select const*);
    ErrorOr<void> walk(size_t level, Function<ErrorOr<bool>()> const& on_row);
    ErrorOr<bool> walk_path(size_t level, AccessPath const&, Function<ErrorOr<bool>()> const& on_row);
    ErrorOr<bool> load_row(size_t level, i64 rowid, ReadonlyBytes record);
    ErrorOr<bool> passes(size_t level, bool on_filters);
    void apply_aggregate(AggregateState&, Value const&, Expression const&);
    ErrorOr<Value> finish_aggregate(AggregateState&, Expression const&);
    bool order_matches_path(Select const&, AccessPath&);

    ErrorOr<void> read_row(TableInformation&, i64 rowid, Vector<Value>& values, bool& found);
    void complete_values(TableInformation&, Vector<Value>& values, i64 rowid);
    void make_index_key(IndexInformation const&, Span<Value const> values, i64 rowid, Vector<u8>& key, bool with_rowid);
    ErrorOr<void> add_to_indexes(TableInformation&, Span<Value const> values, i64 rowid);
    ErrorOr<void> remove_from_indexes(TableInformation&, Span<Value const> values, i64 rowid);
    ErrorOr<bool> insert_row(TableInformation&, Vector<Value>& values, Optional<i64> rowid, ConflictAction, i64* inserted_rowid);
    ErrorOr<void> delete_row(TableInformation&, i64 rowid);
    ErrorOr<Optional<i64>> find_conflict(TableInformation&, IndexInformation const&, Span<Value const> values, i64 own_rowid, bool has_own);
    ErrorOr<i64> next_rowid(TableInformation&);
    ErrorOr<void> collect_rowids(TableInformation&, StringView name, Expression*, Vector<i64>&);
    ErrorOr<void> add_catalogue_row(StringView kind, StringView name, StringView table, u32 root, Value text, i64* rowid);
    ErrorOr<void> update_catalogue_text(TableInformation&, StringView text);
    ErrorOr<void> remove_catalogue_row(StringView kind, StringView name);
    ErrorOr<void> make_automatic_indexes(TableInformation&, CreateTable const&);

    Database& m_database;
    Statement& m_statement;
    Pager& m_pager;
    Catalogue& m_catalogue;

    Vector<TableContext> m_contexts;
    Vector<Conjunct> m_conjuncts;
    Vector<Aggregate> m_aggregates;
    Vector<Value> m_aggregate_values;
    bool m_using_aggregate_values { false };
    bool m_stop { false };
};

template<typename Body>
ErrorOr<void> Executor::in_transaction(Body body)
{
    if (m_database.m_explicit_transaction)
        return body();
    TRY(m_pager.begin());
    auto result = body();
    if (result.is_error()) {
        TRY(m_pager.rollback());
        TRY(m_database.load_catalogue());
        return result;
    }
    auto committed = m_pager.commit();
    if (committed.is_error()) {
        (void)m_pager.rollback();
        (void)m_database.load_catalogue();
    }
    return committed;
}

ErrorOr<void> Executor::resolve(Expression& expression, bool allow_aggregates)
{
    if (expression.kind == ExpressionKind::Column) {
        expression.table_index = -1;
        expression.column_index = -2;
        for (size_t i = 0; i < m_contexts.size(); ++i) {
            auto& context = m_contexts[i];
            if (!expression.table.is_empty() && !context.name.equals_ignoring_ascii_case(expression.table.view()))
                continue;
            int column = column_index_in(*context.table, expression.name.view());
            if (column < 0 && is_rowid_name(expression.name.view()))
                column = -1;
            else if (column < 0)
                continue;
            if (expression.column_index != -2) {
                StringBuilder message;
                message.appendff("ambiguous column name: {}", expression.name.view());
                return fail(message.view());
            }
            expression.table_index = static_cast<int>(i);
            expression.column_index = column;
            expression.has_affinity = true;
            expression.affinity = column >= 0 ? context.table->columns[static_cast<size_t>(column)].affinity : Affinity::Integer;
            if (column >= 0 && column == context.table->rowid_column)
                expression.column_index = -1;
        }
        if (expression.column_index == -2) {
            StringBuilder message;
            message.appendff("no such column: {}", expression.name.view());
            return fail(message.view());
        }
        return {};
    }
    for (auto& child : expression.children) {
        if (child->kind != ExpressionKind::Star)
            TRY(resolve(*child, allow_aggregates));
    }
    return {};
}

static bool is_aggregate_name(StringView name, size_t arguments)
{
    if (name.equals_ignoring_ascii_case("count"_sv) || name.equals_ignoring_ascii_case("sum"_sv) || name.equals_ignoring_ascii_case("total"_sv)
        || name.equals_ignoring_ascii_case("avg"_sv) || name.equals_ignoring_ascii_case("group_concat"_sv))
        return true;
    return arguments == 1 && (name.equals_ignoring_ascii_case("min"_sv) || name.equals_ignoring_ascii_case("max"_sv));
}

ErrorOr<void> Executor::collect_aggregates(Expression& expression)
{
    if (expression.kind == ExpressionKind::Function && is_aggregate_name(expression.name.view(), expression.children.size())) {
        expression.aggregate_index = static_cast<int>(m_aggregates.size());
        m_aggregates.append(Aggregate { &expression });
        return {};
    }
    expression.aggregate_index = -1;
    for (auto& child : expression.children)
        TRY(collect_aggregates(*child));
    return {};
}

void Executor::apply_comparison_affinity(Expression const& left_expression, Expression const& right_expression, Value& left, Value& right)
{
    auto is_numeric = [](Affinity affinity) { return affinity == Affinity::Integer || affinity == Affinity::Real || affinity == Affinity::Numeric; };
    bool left_has = left_expression.has_affinity;
    bool right_has = right_expression.has_affinity;
    Affinity left_affinity = left_expression.affinity;
    Affinity right_affinity = right_expression.affinity;
    if (left_has && is_numeric(left_affinity) && !(right_has && is_numeric(right_affinity))) {
        right = apply_affinity(move(right), Affinity::Numeric);
    } else if (right_has && is_numeric(right_affinity) && !(left_has && is_numeric(left_affinity))) {
        left = apply_affinity(move(left), Affinity::Numeric);
    } else if (left_has && left_affinity == Affinity::Text && !right_has) {
        right = apply_affinity(move(right), Affinity::Text);
    } else if (right_has && right_affinity == Affinity::Text && !left_has) {
        left = apply_affinity(move(left), Affinity::Text);
    }
}

ErrorOr<Value> Executor::evaluate_binary(Expression const& expression)
{
    auto const& left_expression = *expression.children[0];
    auto const& right_expression = *expression.children[1];

    if (expression.op == Operator::And) {
        auto left = TRY(evaluate(left_expression)).truth();
        if (left.has_value() && !left.value())
            return Value::integer(0);
        auto right = TRY(evaluate(right_expression)).truth();
        if (right.has_value() && !right.value())
            return Value::integer(0);
        if (!left.has_value() || !right.has_value())
            return Value {};
        return Value::integer(1);
    }
    if (expression.op == Operator::Or) {
        auto left = TRY(evaluate(left_expression)).truth();
        if (left.has_value() && left.value())
            return Value::integer(1);
        auto right = TRY(evaluate(right_expression)).truth();
        if (right.has_value() && right.value())
            return Value::integer(1);
        if (!left.has_value() || !right.has_value())
            return Value {};
        return Value::integer(0);
    }

    auto left = TRY(evaluate(left_expression));
    auto right = TRY(evaluate(right_expression));

    switch (expression.op) {
    case Operator::Is:
    case Operator::IsNot: {
        apply_comparison_affinity(left_expression, right_expression, left, right);
        bool same = compare(left, right) == 0;
        return Value::integer(same == (expression.op == Operator::Is) ? 1 : 0);
    }
    case Operator::Equal:
    case Operator::NotEqual:
    case Operator::Less:
    case Operator::LessOrEqual:
    case Operator::Greater:
    case Operator::GreaterOrEqual: {
        if (left.is_null() || right.is_null())
            return Value {};
        apply_comparison_affinity(left_expression, right_expression, left, right);
        int order = compare(left, right);
        bool result = false;
        switch (expression.op) {
        case Operator::Equal:
            result = order == 0;
            break;
        case Operator::NotEqual:
            result = order != 0;
            break;
        case Operator::Less:
            result = order < 0;
            break;
        case Operator::LessOrEqual:
            result = order <= 0;
            break;
        case Operator::Greater:
            result = order > 0;
            break;
        default:
            result = order >= 0;
            break;
        }
        return Value::integer(result ? 1 : 0);
    }
    case Operator::Concatenate: {
        if (left.is_null() || right.is_null())
            return Value {};
        StringBuilder joined;
        joined.append(left.to_text().view());
        joined.append(right.to_text().view());
        return Value::text(joined.view());
    }
    case Operator::BitAnd:
    case Operator::BitOr:
    case Operator::ShiftLeft:
    case Operator::ShiftRight: {
        if (left.is_null() || right.is_null())
            return Value {};
        u64 a = static_cast<u64>(left.to_integer());
        i64 b = right.to_integer();
        switch (expression.op) {
        case Operator::BitAnd:
            return Value::integer(static_cast<i64>(a & static_cast<u64>(b)));
        case Operator::BitOr:
            return Value::integer(static_cast<i64>(a | static_cast<u64>(b)));
        default: {
            bool to_left = (expression.op == Operator::ShiftLeft) == (b >= 0);
            u64 distance = static_cast<u64>(b < 0 ? -b : b);
            if (distance >= 64)
                return Value::integer((!to_left && static_cast<i64>(a) < 0) ? -1 : 0);
            if (to_left)
                return Value::integer(static_cast<i64>(a << distance));
            return Value::integer(static_cast<i64>(a) >> distance);
        }
        }
    }
    default:
        break;
    }

    // Arithmetic.
    if (left.is_null() || right.is_null())
        return Value {};
    auto a = left.to_number();
    auto b = right.to_number();
    if (expression.op == Operator::Modulo) {
        i64 x = a.to_integer();
        i64 y = b.to_integer();
        if (y == 0)
            return Value {};
        if (y == -1)
            return Value::integer(0);
        return Value::integer(x % y);
    }
    if (a.type() == Type::Integer && b.type() == Type::Integer) {
        i64 x = a.as_integer();
        i64 y = b.as_integer();
        i64 result = 0;
        bool overflowed = false;
        switch (expression.op) {
        case Operator::Add:
            overflowed = __builtin_add_overflow(x, y, &result);
            break;
        case Operator::Subtract:
            overflowed = __builtin_sub_overflow(x, y, &result);
            break;
        case Operator::Multiply:
            overflowed = __builtin_mul_overflow(x, y, &result);
            break;
        case Operator::Divide:
            if (y == 0)
                return Value {};
            if (x == NumericLimits<i64>::min() && y == -1) {
                overflowed = true;
                break;
            }
            result = x / y;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
        if (!overflowed)
            return Value::integer(result);
    }
    double x = a.to_real();
    double y = b.to_real();
    switch (expression.op) {
    case Operator::Add:
        return Value::real(x + y);
    case Operator::Subtract:
        return Value::real(x - y);
    case Operator::Multiply:
        return Value::real(x * y);
    case Operator::Divide:
        if (y == 0)
            return Value {};
        return Value::real(x / y);
    default:
        VERIFY_NOT_REACHED();
    }
}

static Value substring_of(Value const& input, i64 start, Optional<i64> length)
{
    bool is_blob = input.type() == Type::Blob;
    auto text = input.to_text();
    auto view = text.view();
    i64 total = static_cast<i64>(is_blob ? view.length() : utf8_length(view));
    i64 begin = start;
    i64 count = length.value_or(NumericLimits<i64>::max());
    bool negative_count = count < 0;
    if (negative_count)
        count = -count;
    if (begin < 0) {
        begin = total + begin;
        if (begin < 0) {
            if (!negative_count && count != NumericLimits<i64>::max())
                count += begin;
            begin = 0;
            if (count < 0)
                count = 0;
        }
    } else if (begin > 0) {
        begin -= 1;
    } else if (count > 0 && !negative_count && length.has_value()) {
        // Position zero is just before the first character, and uses one of the count.
        count -= 1;
    }
    if (negative_count) {
        i64 first = begin - count;
        if (first < 0) {
            count = begin;
            first = 0;
        }
        begin = first;
    }
    if (begin >= total || count <= 0)
        return is_blob ? Value::blob({}) : Value::text({});
    if (count > total - begin)
        count = total - begin;
    size_t from, to;
    if (is_blob) {
        from = static_cast<size_t>(begin);
        to = from + static_cast<size_t>(count);
    } else {
        from = utf8_offset(view, static_cast<size_t>(begin));
        to = from + utf8_offset(view.substring_view(from), static_cast<size_t>(count));
    }
    auto piece = view.substring_view(from, to - from);
    return is_blob ? Value::blob(piece.bytes()) : Value::text(piece);
}

static Value trim_text(StringView text, StringView characters, bool left, bool right)
{
    auto is_trimmed = [&](char c) { return characters.contains(c); };
    size_t start = 0;
    size_t end = text.length();
    if (left) {
        while (start < end && is_trimmed(text[start]))
            ++start;
    }
    if (right) {
        while (end > start && is_trimmed(text[end - 1]))
            --end;
    }
    return Value::text(text.substring_view(start, end - start));
}

ErrorOr<Value> Executor::evaluate_function(Expression const& expression)
{
    if (expression.aggregate_index >= 0) {
        if (!m_using_aggregate_values)
            return fail("misuse of aggregate function"_sv);
        return m_aggregate_values[static_cast<size_t>(expression.aggregate_index)];
    }
    auto name = expression.name.view();
    auto is = [&](StringView candidate) { return name.equals_ignoring_ascii_case(candidate); };
    size_t const count = expression.children.size();

    if (is("coalesce"_sv) || is("ifnull"_sv)) {
        for (auto const& child : expression.children) {
            auto value = TRY(evaluate(*child));
            if (!value.is_null())
                return value;
        }
        return Value {};
    }

    Vector<Value> arguments;
    for (auto const& child : expression.children)
        arguments.append(TRY(evaluate(*child)));
    auto wrong_arguments = [&] {
        StringBuilder message;
        message.appendff("wrong number of arguments to function {}()", name);
        return fail(message.view());
    };

    if (is("abs"_sv)) {
        if (count != 1)
            return wrong_arguments();
        if (arguments[0].is_null())
            return Value {};
        auto number = arguments[0].to_number();
        if (number.type() == Type::Integer) {
            if (number.as_integer() == NumericLimits<i64>::min())
                return fail("integer overflow"_sv);
            return Value::integer(number.as_integer() < 0 ? -number.as_integer() : number.as_integer());
        }
        double real = number.to_real();
        return Value::real(real < 0 ? -real : real);
    }
    if (is("length"_sv)) {
        if (count != 1)
            return wrong_arguments();
        if (arguments[0].is_null())
            return Value {};
        if (arguments[0].type() == Type::Blob)
            return Value::integer(static_cast<i64>(arguments[0].as_text().length()));
        return Value::integer(static_cast<i64>(utf8_length(arguments[0].to_text().view())));
    }
    if (is("lower"_sv) || is("upper"_sv)) {
        if (count != 1)
            return wrong_arguments();
        if (arguments[0].is_null())
            return Value {};
        auto text = arguments[0].to_text();
        StringBuilder changed;
        for (char c : text.view())
            changed.append(is("lower"_sv) ? ascii_lower(c) : ascii_upper(c));
        return Value::text(changed.view());
    }
    if (is("substr"_sv) || is("substring"_sv)) {
        if (count != 2 && count != 3)
            return wrong_arguments();
        if (arguments[0].is_null() || arguments[1].is_null() || (count == 3 && arguments[2].is_null()))
            return Value {};
        Optional<i64> length;
        if (count == 3)
            length = arguments[2].to_integer();
        return substring_of(arguments[0], arguments[1].to_integer(), length);
    }
    if (is("trim"_sv) || is("ltrim"_sv) || is("rtrim"_sv)) {
        if (count != 1 && count != 2)
            return wrong_arguments();
        if (arguments[0].is_null() || (count == 2 && arguments[1].is_null()))
            return Value {};
        auto text = arguments[0].to_text();
        String characters = count == 2 ? arguments[1].to_text() : MUST(String::from_view(" "_sv));
        return trim_text(text.view(), characters.view(), !is("rtrim"_sv), !is("ltrim"_sv));
    }
    if (is("replace"_sv)) {
        if (count != 3)
            return wrong_arguments();
        if (arguments[0].is_null() || arguments[1].is_null() || arguments[2].is_null())
            return Value {};
        auto text = arguments[0].to_text();
        auto from = arguments[1].to_text();
        auto to = arguments[2].to_text();
        if (from.is_empty())
            return Value::text(text.view());
        StringBuilder replaced;
        auto view = text.view();
        size_t i = 0;
        while (i < view.length()) {
            if (view.substring_view(i).starts_with(from.view())) {
                replaced.append(to.view());
                i += from.length();
            } else {
                replaced.append(view[i++]);
            }
        }
        return Value::text(replaced.view());
    }
    if (is("instr"_sv)) {
        if (count != 2)
            return wrong_arguments();
        if (arguments[0].is_null() || arguments[1].is_null())
            return Value {};
        auto haystack = arguments[0].to_text();
        auto needle = arguments[1].to_text();
        auto found = haystack.view().find(needle.view());
        if (!found.has_value())
            return Value::integer(0);
        return Value::integer(static_cast<i64>(utf8_length(haystack.view().substring_view(0, found.value()))) + 1);
    }
    if (is("round"_sv)) {
        if (count != 1 && count != 2)
            return wrong_arguments();
        if (arguments[0].is_null() || (count == 2 && arguments[1].is_null()))
            return Value {};
        i64 places = count == 2 ? arguments[1].to_integer() : 0;
        if (places < 0)
            places = 0;
        if (places > 30)
            places = 30;
        double const number = arguments[0].to_real();
        // Halves away from zero, as far as a whole number goes.
        if (places == 0 && number > -4503599627370496.0 && number < 4503599627370496.0)
            return Value::real(static_cast<double>(static_cast<i64>(number + (number < 0 ? -0.5 : 0.5))));
        char buffer[400];
        int length = __builtin_snprintf(buffer, sizeof(buffer), "%.*f", static_cast<int>(places), number);
        return Value::real(Value::text(StringView(buffer, static_cast<size_t>(length))).to_real());
    }
    if (is("typeof"_sv)) {
        if (count != 1)
            return wrong_arguments();
        switch (arguments[0].type()) {
        case Type::Null:
            return Value::text("null"_sv);
        case Type::Integer:
            return Value::text("integer"_sv);
        case Type::Real:
            return Value::text("real"_sv);
        case Type::Text:
            return Value::text("text"_sv);
        case Type::Blob:
            return Value::text("blob"_sv);
        }
    }
    if (is("hex"_sv)) {
        if (count != 1)
            return wrong_arguments();
        auto text = arguments[0].to_text();
        StringBuilder digits;
        static constexpr char hexadecimal[] = "0123456789ABCDEF";
        for (char c : text.view()) {
            digits.append(hexadecimal[static_cast<u8>(c) >> 4]);
            digits.append(hexadecimal[static_cast<u8>(c) & 15]);
        }
        return Value::text(digits.view());
    }
    if (is("nullif"_sv)) {
        if (count != 2)
            return wrong_arguments();
        if (!arguments[0].is_null() && !arguments[1].is_null() && compare(arguments[0], arguments[1]) == 0)
            return Value {};
        return arguments[0];
    }
    if (is("min"_sv) || is("max"_sv)) {
        // As many as two arguments or more: otherwise it was an aggregate.
        if (count < 2)
            return wrong_arguments();
        size_t chosen = 0;
        for (size_t i = 0; i < count; ++i) {
            if (arguments[i].is_null())
                return Value {};
            int order = compare(arguments[i], arguments[chosen]);
            if (is("min"_sv) ? order < 0 : order > 0)
                chosen = i;
        }
        return arguments[chosen];
    }
    if (is("last_insert_rowid"_sv))
        return Value::integer(m_database.m_last_insert_rowid);
    if (is("changes"_sv))
        return Value::integer(static_cast<i64>(m_database.m_changes));

    StringBuilder message;
    message.appendff("no such function: {}", name);
    return fail(message.view());
}

ErrorOr<Value> Executor::evaluate(Expression const& expression)
{
    switch (expression.kind) {
    case ExpressionKind::Literal:
        return expression.literal;
    case ExpressionKind::Parameter:
        if (expression.parameter >= m_statement.m_parameters.size())
            return Value {};
        return m_statement.m_parameters[expression.parameter];
    case ExpressionKind::Column: {
        auto const& context = m_contexts[static_cast<size_t>(expression.table_index)];
        if (context.is_null_row)
            return Value {};
        if (expression.column_index < 0)
            return Value::integer(context.rowid);
        return context.values[static_cast<size_t>(expression.column_index)];
    }
    case ExpressionKind::Negate: {
        auto value = TRY(evaluate(*expression.children[0]));
        if (value.is_null())
            return value;
        auto number = value.to_number();
        if (number.type() == Type::Integer) {
            if (number.as_integer() == NumericLimits<i64>::min())
                return Value::real(-static_cast<double>(number.as_integer()));
            return Value::integer(-number.as_integer());
        }
        return Value::real(-number.to_real());
    }
    case ExpressionKind::Not: {
        auto truth = TRY(evaluate(*expression.children[0])).truth();
        if (!truth.has_value())
            return Value {};
        return Value::integer(truth.value() ? 0 : 1);
    }
    case ExpressionKind::BitwiseNot: {
        auto value = TRY(evaluate(*expression.children[0]));
        if (value.is_null())
            return value;
        return Value::integer(~value.to_integer());
    }
    case ExpressionKind::Binary:
        return evaluate_binary(expression);
    case ExpressionKind::Function:
        return evaluate_function(expression);
    case ExpressionKind::IsNull: {
        auto value = TRY(evaluate(*expression.children[0]));
        return Value::integer(value.is_null() == !expression.negated ? 1 : 0);
    }
    case ExpressionKind::Like: {
        auto text = TRY(evaluate(*expression.children[0]));
        auto pattern = TRY(evaluate(*expression.children[1]));
        if (text.is_null() || pattern.is_null())
            return Value {};
        auto text_string = text.to_text();
        auto pattern_string = pattern.to_text();
        bool matched = expression.glob ? glob_matches(pattern_string.view(), 0, text_string.view(), 0) : like_matches(pattern_string.view(), text_string.view());
        return Value::integer(matched != expression.negated ? 1 : 0);
    }
    case ExpressionKind::Between: {
        auto value = TRY(evaluate(*expression.children[0]));
        auto low = TRY(evaluate(*expression.children[1]));
        auto high = TRY(evaluate(*expression.children[2]));
        Optional<bool> above;
        Optional<bool> below;
        if (!value.is_null() && !low.is_null()) {
            auto v = value;
            apply_comparison_affinity(*expression.children[0], *expression.children[1], v, low);
            above = compare(v, low) >= 0;
        }
        if (!value.is_null() && !high.is_null()) {
            auto v = value;
            apply_comparison_affinity(*expression.children[0], *expression.children[2], v, high);
            below = compare(v, high) <= 0;
        }
        Optional<bool> result;
        if ((above.has_value() && !above.value()) || (below.has_value() && !below.value()))
            result = false;
        else if (above.has_value() && below.has_value())
            result = true;
        if (!result.has_value())
            return Value {};
        return Value::integer(result.value() != expression.negated ? 1 : 0);
    }
    case ExpressionKind::In: {
        auto value = TRY(evaluate(*expression.children[0]));
        if (expression.children.size() == 1)
            return Value::integer(expression.negated ? 1 : 0);
        if (value.is_null())
            return Value {};
        bool saw_null = false;
        for (size_t i = 1; i < expression.children.size(); ++i) {
            auto candidate = TRY(evaluate(*expression.children[i]));
            if (candidate.is_null()) {
                saw_null = true;
                continue;
            }
            auto compared = value;
            apply_comparison_affinity(*expression.children[0], *expression.children[i], compared, candidate);
            if (compare(compared, candidate) == 0)
                return Value::integer(expression.negated ? 0 : 1);
        }
        if (saw_null)
            return Value {};
        return Value::integer(expression.negated ? 1 : 0);
    }
    case ExpressionKind::Case: {
        size_t index = 0;
        Optional<Value> operand;
        if (expression.has_operand)
            operand = TRY(evaluate(*expression.children[index++]));
        size_t const last = expression.children.size() - (expression.has_else ? 1 : 0);
        while (index + 1 < last + (expression.has_else ? 0 : 0) && index + 1 < expression.children.size()) {
            if (index + 1 >= last)
                break;
            auto when = TRY(evaluate(*expression.children[index]));
            bool chosen;
            if (operand.has_value()) {
                chosen = !operand->is_null() && !when.is_null() && compare(operand.value(), when) == 0;
            } else {
                chosen = when.truth().value_or(false);
            }
            if (chosen)
                return evaluate(*expression.children[index + 1]);
            index += 2;
        }
        if (expression.has_else)
            return evaluate(*expression.children.last());
        return Value {};
    }
    case ExpressionKind::Cast: {
        auto value = TRY(evaluate(*expression.children[0]));
        if (value.is_null())
            return value;
        switch (affinity_of_declared_type(expression.name.view())) {
        case Affinity::Text:
            return Value::text(value.to_text().view());
        case Affinity::Blob: {
            auto text = value.to_text();
            return Value::blob(text.view().bytes());
        }
        case Affinity::Integer:
            return Value::integer(value.to_integer());
        case Affinity::Real:
            return Value::real(value.to_real());
        case Affinity::Numeric: {
            auto number = value.to_number();
            return apply_affinity(move(number), Affinity::Numeric);
        }
        }
        VERIFY_NOT_REACHED();
    }
    case ExpressionKind::Star:
        return fail("a * isn't a value"_sv);
    }
    VERIFY_NOT_REACHED();
}

void Executor::find_conjuncts(Expression const& expression, Vector<Expression const*>& out)
{
    if (expression.kind == ExpressionKind::Binary && expression.op == Operator::And) {
        find_conjuncts(*expression.children[0], out);
        find_conjuncts(*expression.children[1], out);
        return;
    }
    out.append(&expression);
}

int Executor::highest_level_of(Expression const& expression)
{
    int highest = -1;
    if (expression.kind == ExpressionKind::Column)
        highest = expression.table_index;
    for (auto const& child : expression.children) {
        if (child->kind != ExpressionKind::Star)
            highest = max(highest, highest_level_of(*child));
    }
    return highest;
}

// `column op value`, in either order, where the value can be worked out before this level.
struct Comparison {
    int column { -2 };
    Operator op { Operator::Equal };
    Expression const* value { nullptr };
};

static Operator mirrored(Operator op)
{
    switch (op) {
    case Operator::Less:
        return Operator::Greater;
    case Operator::LessOrEqual:
        return Operator::GreaterOrEqual;
    case Operator::Greater:
        return Operator::Less;
    case Operator::GreaterOrEqual:
        return Operator::LessOrEqual;
    default:
        return op;
    }
}

bool Executor::order_matches_path(Select const& select, AccessPath& path)
{
    if (select.order_by.is_empty() || m_contexts.size() != 1)
        return false;
    bool descending = select.order_by[0].descending;
    for (auto const& term : select.order_by) {
        if (term.descending != descending)
            return false;
    }
    auto const& context = m_contexts[0];
    if (path.kind == AccessPath::Kind::Scan || path.kind == AccessPath::Kind::RowidRange) {
        if (select.order_by.size() != 1)
            return false;
        auto const& term = *select.order_by[0].expression;
        if (term.kind != ExpressionKind::Column || term.column_index != -1)
            return false;
        path.reverse = descending;
        return true;
    }
    if (path.kind != AccessPath::Kind::Index || path.in_list)
        return false;
    size_t index_column = path.equals.size();
    for (size_t i = 0; i < select.order_by.size(); ++i) {
        auto const& term = *select.order_by[i].expression;
        if (term.kind != ExpressionKind::Column)
            return false;
        // Past the end of the index, only the rowid can come next.
        if (index_column >= path.index->columns.size()) {
            if (term.column_index != -1 || i + 1 != select.order_by.size())
                return false;
            continue;
        }
        if (term.column_index != path.index->columns[index_column])
            return false;
        ++index_column;
    }
    (void)context;
    path.reverse = descending;
    return true;
}

ErrorOr<AccessPath> Executor::choose_path(size_t level, Select const* select)
{
    auto& context = m_contexts[level];
    Vector<Comparison> comparisons;
    Vector<Comparison> memberships;
    for (auto const& conjunct : m_conjuncts) {
        if (conjunct.is_on && conjunct.on_level != static_cast<int>(level))
            continue;
        auto const& expression = *conjunct.expression;
        if (expression.kind == ExpressionKind::In && !expression.negated && expression.children[0]->kind == ExpressionKind::Column
            && expression.children[0]->table_index == static_cast<int>(level)) {
            bool constant = true;
            for (size_t i = 1; i < expression.children.size(); ++i) {
                if (highest_level_of(*expression.children[i]) >= static_cast<int>(level))
                    constant = false;
            }
            if (constant)
                memberships.append(Comparison { expression.children[0]->column_index, Operator::Equal, &expression });
            continue;
        }
        if (expression.kind != ExpressionKind::Binary)
            continue;
        auto op = expression.op;
        if (op != Operator::Equal && op != Operator::Less && op != Operator::LessOrEqual && op != Operator::Greater && op != Operator::GreaterOrEqual)
            continue;
        auto const* left = expression.children[0].ptr();
        auto const* right = expression.children[1].ptr();
        auto consider = [&](Expression const* column, Expression const* value, Operator comparison) {
            if (column->kind != ExpressionKind::Column || column->table_index != static_cast<int>(level))
                return;
            if (highest_level_of(*value) >= static_cast<int>(level))
                return;
            // An index keeps the column's values as the column has them: only a value that'll compare the same way.
            if (value->has_affinity && value->affinity != column->affinity)
                return;
            comparisons.append(Comparison { column->column_index, comparison, value });
        };
        consider(left, right, op);
        consider(right, left, mirrored(op));
    }

    AccessPath best;
    int best_score = 0;

    // By rowid: as good as it gets.
    for (auto const& comparison : comparisons) {
        if (comparison.column == -1 && comparison.op == Operator::Equal) {
            best = AccessPath {};
            best.kind = AccessPath::Kind::RowidEqual;
            best.equals.append(comparison.value);
            return best;
        }
    }
    AccessPath by_rowid_range;
    by_rowid_range.kind = AccessPath::Kind::RowidRange;
    bool has_rowid_range = false;
    for (auto const& comparison : comparisons) {
        if (comparison.column != -1)
            continue;
        if ((comparison.op == Operator::Greater || comparison.op == Operator::GreaterOrEqual) && !by_rowid_range.lower) {
            by_rowid_range.lower = comparison.value;
            by_rowid_range.lower_inclusive = comparison.op == Operator::GreaterOrEqual;
            has_rowid_range = true;
        } else if ((comparison.op == Operator::Less || comparison.op == Operator::LessOrEqual) && !by_rowid_range.upper) {
            by_rowid_range.upper = comparison.value;
            by_rowid_range.upper_inclusive = comparison.op == Operator::LessOrEqual;
            has_rowid_range = true;
        }
    }
    if (has_rowid_range) {
        best = by_rowid_range;
        best_score = 5;
    }

    for (auto& index : context.table->indexes) {
        AccessPath path;
        path.kind = AccessPath::Kind::Index;
        path.index = index.ptr();
        size_t column_position = 0;
        for (; column_position < index->columns.size(); ++column_position) {
            Expression const* found = nullptr;
            for (auto const& comparison : comparisons) {
                if (comparison.column == index->columns[column_position] && comparison.op == Operator::Equal) {
                    found = comparison.value;
                    break;
                }
            }
            if (!found)
                break;
            path.equals.append(found);
        }
        int score = static_cast<int>(path.equals.size()) * 4;
        if (index->unique && path.equals.size() == index->columns.size())
            score += 200;
        if (column_position < index->columns.size()) {
            bool found_membership = false;
            for (auto const& membership : memberships) {
                if (membership.column == index->columns[column_position]) {
                    path.in_list = membership.value;
                    score += 3;
                    found_membership = true;
                    break;
                }
            }
            if (!found_membership) {
                for (auto const& comparison : comparisons) {
                    if (comparison.column != index->columns[column_position])
                        continue;
                    if ((comparison.op == Operator::Greater || comparison.op == Operator::GreaterOrEqual) && !path.lower) {
                        path.lower = comparison.value;
                        path.lower_inclusive = comparison.op == Operator::GreaterOrEqual;
                        score += 2;
                    } else if ((comparison.op == Operator::Less || comparison.op == Operator::LessOrEqual) && !path.upper) {
                        path.upper = comparison.value;
                        path.upper_inclusive = comparison.op == Operator::LessOrEqual;
                        score += 2;
                    }
                }
            }
        }
        if (select && order_matches_path(*select, path))
            score += 1;
        if (score > best_score) {
            best = move(path);
            best_score = score;
        }
    }
    if (select)
        (void)order_matches_path(*select, best);
    return best;
}

ErrorOr<bool> Executor::load_row(size_t level, i64 rowid, ReadonlyBytes record)
{
    auto& context = m_contexts[level];
    TRY(decode_record(record, context.values));
    context.rowid = rowid;
    context.is_null_row = false;
    complete_values(*context.table, context.values, rowid);
    return true;
}

ErrorOr<bool> Executor::passes(size_t level, bool on_filters)
{
    for (auto const& conjunct : m_conjuncts) {
        if (conjunct.is_on != on_filters)
            continue;
        if (on_filters ? conjunct.on_level != static_cast<int>(level) : conjunct.level != static_cast<int>(level))
            continue;
        auto truth = TRY(evaluate(*conjunct.expression)).truth();
        if (!truth.has_value() || !truth.value())
            return false;
    }
    return true;
}

ErrorOr<bool> Executor::walk_path(size_t level, AccessPath const& path, Function<ErrorOr<bool>()> const& on_row)
{
    // Calls for each row a key range finds, in order; says whether the row is to carry on from.
    auto& context = m_contexts[level];
    BTree table(m_pager, context.table->root);
    bool matched = false;
    Vector<u8> record;

    auto visit = [&](i64 rowid) -> ErrorOr<void> {
        TRY(load_row(level, rowid, record.span()));
        if (!TRY(passes(level, true)))
            return {};
        matched = true;
        if (!TRY(passes(level, false)))
            return {};
        TRY(walk(level + 1, on_row));
        return {};
    };

    auto probe_value = [&](Expression const& expression, Affinity affinity) -> ErrorOr<Value> {
        return apply_affinity(TRY(evaluate(expression)), affinity);
    };

    auto scan_range = [&](BTree& tree, Vector<u8> const& lower, Vector<u8> const& upper, bool has_lower, bool has_upper, bool is_index, bool reverse) -> ErrorOr<void> {
        BTree::Cursor cursor = reverse ? (has_upper ? TRY(tree.seek(upper.span())) : TRY(tree.last())) : (has_lower ? TRY(tree.seek(lower.span())) : TRY(tree.first()));
        if (reverse) {
            if (has_upper) {
                if (cursor.is_valid())
                    TRY(cursor.previous());
                else
                    cursor = TRY(tree.last());
            }
        }
        while (cursor.is_valid() && !m_stop) {
            auto key = cursor.key();
            if (reverse) {
                if (has_lower && compare_key_bytes(key, lower.span()) < 0)
                    break;
            } else if (has_upper && compare_key_bytes(key, upper.span()) >= 0) {
                break;
            }
            i64 rowid;
            if (is_index) {
                rowid = decode_rowid(ReadonlyBytes(key.data() + key.size() - 8, 8));
                Vector<u8> row_key;
                encode_rowid(rowid, row_key);
                if (!TRY(table.get(row_key.span(), record)))
                    return fail("an index names a row that isn't there"_sv);
            } else {
                rowid = decode_rowid(key);
                TRY(cursor.value(record));
            }
            TRY(visit(rowid));
            if (m_stop)
                break;
            if (reverse)
                TRY(cursor.previous());
            else
                TRY(cursor.next());
        }
        return {};
    };

    switch (path.kind) {
    case AccessPath::Kind::Scan: {
        Vector<u8> nothing;
        TRY(scan_range(table, nothing, nothing, false, false, false, path.reverse));
        break;
    }
    case AccessPath::Kind::RowidEqual: {
        auto value = TRY(evaluate(*path.equals[0]));
        if (value.is_null())
            break;
        auto number = apply_affinity(move(value), Affinity::Integer);
        if (number.type() != Type::Integer)
            break;
        Vector<u8> key;
        encode_rowid(number.as_integer(), key);
        if (TRY(table.get(key.span(), record)))
            TRY(visit(number.as_integer()));
        break;
    }
    case AccessPath::Kind::RowidRange: {
        Vector<u8> lower;
        Vector<u8> upper;
        bool empty = false;
        auto bound = [&](Expression const* expression, bool inclusive, Vector<u8>& key, bool is_upper) -> ErrorOr<void> {
            if (!expression)
                return {};
            auto value = TRY(evaluate(*expression));
            if (value.is_null()) {
                empty = true;
                return {};
            }
            auto number = value.to_number();
            i64 integer = number.to_integer();
            if (number.type() == Type::Real && static_cast<double>(integer) != number.as_real()) {
                // Between two integers: down for a lower limit that's in, and so on.
                if (number.as_real() < static_cast<double>(integer))
                    --integer;
                encode_rowid(integer + ((is_upper) ? 1 : 1), key);
                return {};
            }
            bool next = is_upper == inclusive;
            if (next && integer == NumericLimits<i64>::max()) {
                if (!is_upper)
                    empty = true;
                else
                    append_after_value(key);
                return {};
            }
            encode_rowid(next ? integer + 1 : integer, key);
            return {};
        };
        TRY(bound(path.lower, path.lower_inclusive, lower, false));
        TRY(bound(path.upper, path.upper_inclusive, upper, true));
        if (!empty)
            TRY(scan_range(table, lower, upper, path.lower != nullptr, path.upper != nullptr, false, path.reverse));
        break;
    }
    case AccessPath::Kind::Index: {
        BTree index(m_pager, path.index->root);
        auto const& table_info = *context.table;
        auto affinity_at = [&](size_t position) { return table_info.columns[static_cast<size_t>(path.index->columns[position])].affinity; };
        Vector<u8> prefix;
        bool empty = false;
        for (size_t i = 0; i < path.equals.size(); ++i) {
            auto value = TRY(probe_value(*path.equals[i], affinity_at(i)));
            if (value.is_null()) {
                empty = true;
                break;
            }
            encode_key(value, prefix);
        }
        if (empty)
            break;
        if (path.in_list) {
            Vector<Vector<u8>> keys;
            for (size_t i = 1; i < path.in_list->children.size(); ++i) {
                auto value = TRY(probe_value(*path.in_list->children[i], affinity_at(path.equals.size())));
                if (value.is_null())
                    continue;
                Vector<u8> key = prefix;
                encode_key(value, key);
                keys.append(move(key));
            }
            quick_sort(keys.span(), [](Vector<u8> const& a, Vector<u8> const& b) { return compare_key_bytes(a.span(), b.span()) < 0; });
            for (size_t i = 0; i < keys.size() && !m_stop; ++i) {
                if (i > 0 && compare_key_bytes(keys[i].span(), keys[i - 1].span()) == 0)
                    continue;
                Vector<u8> upper = keys[i];
                append_after_value(upper);
                TRY(scan_range(index, keys[i], upper, true, true, true, false));
            }
            break;
        }
        Vector<u8> lower = prefix;
        Vector<u8> upper = prefix;
        bool has_lower = !prefix.is_empty();
        bool has_upper = !prefix.is_empty();
        if (path.lower) {
            auto value = TRY(probe_value(*path.lower, affinity_at(path.equals.size())));
            if (value.is_null())
                break;
            encode_key(value, lower);
            if (!path.lower_inclusive)
                append_after_value(lower);
            has_lower = true;
        }
        if (path.upper) {
            auto value = TRY(probe_value(*path.upper, affinity_at(path.equals.size())));
            if (value.is_null())
                break;
            encode_key(value, upper);
            if (path.upper_inclusive)
                append_after_value(upper);
            has_upper = true;
        } else if (!prefix.is_empty()) {
            append_after_value(upper);
        }
        TRY(scan_range(index, lower, upper, has_lower, has_upper, true, path.reverse));
        break;
    }
    }

    return matched;
}

ErrorOr<void> Executor::walk(size_t level, Function<ErrorOr<bool>()> const& on_row)
{
    if (m_stop)
        return {};
    if (level == m_contexts.size()) {
        if (!TRY(on_row()))
            m_stop = true;
        return {};
    }
    auto path = TRY(choose_path(level, nullptr));
    bool matched = TRY(walk_path(level, path, on_row));
    if (!matched && m_contexts[level].left_join && !m_stop) {
        auto& context = m_contexts[level];
        context.is_null_row = true;
        if (TRY(passes(level, false)))
            TRY(walk(level + 1, on_row));
        context.is_null_row = false;
    }
    return {};
}

void Executor::apply_aggregate(AggregateState& state, Value const& value, Expression const& expression)
{
    auto name = expression.name.view();
    bool const is_star = !expression.children.is_empty() && expression.children[0]->kind == ExpressionKind::Star;
    if (name.equals_ignoring_ascii_case("count"_sv) && is_star) {
        ++state.count;
        return;
    }
    if (value.is_null())
        return;
    if (expression.distinct) {
        if (!state.distinct)
            state.distinct = new HashMap<String, bool>;
        Vector<u8> key;
        encode_key(value, key);
        auto key_text = MUST(String::from_view(StringView(reinterpret_cast<char const*>(key.data()), key.size())));
        if (state.distinct->contains(key_text.view()))
            return;
        state.distinct->set(move(key_text), true);
    }
    ++state.count;
    if (name.equals_ignoring_ascii_case("count"_sv))
        return;
    if (name.equals_ignoring_ascii_case("min"_sv) || name.equals_ignoring_ascii_case("max"_sv)) {
        bool better = !state.has_best;
        if (state.has_best) {
            int order = compare(value, state.best);
            better = name.equals_ignoring_ascii_case("min"_sv) ? order < 0 : order > 0;
        }
        if (better) {
            state.best = value;
            state.has_best = true;
        }
        return;
    }
    if (name.equals_ignoring_ascii_case("group_concat"_sv)) {
        if (!state.text)
            state.text = new StringBuilder;
        if (state.has_text) {
            Value separator = Value::text(","_sv);
            if (expression.children.size() > 1)
                separator = MUST(evaluate(*expression.children[1]));
            state.text->append(separator.to_text().view());
        }
        state.has_text = true;
        state.text->append(value.to_text().view());
        return;
    }
    // sum, total and avg.
    auto number = value.to_number();
    if (number.type() == Type::Integer && !state.sum_is_real && !state.sum_overflowed) {
        i64 added;
        if (__builtin_add_overflow(state.integer_sum, number.as_integer(), &added)) {
            state.sum_overflowed = true;
        } else {
            state.integer_sum = added;
        }
    } else {
        state.sum_is_real = true;
    }
    state.real_sum += number.to_real();
}

ErrorOr<Value> Executor::finish_aggregate(AggregateState& state, Expression const& expression)
{
    auto name = expression.name.view();
    Value result;
    if (name.equals_ignoring_ascii_case("count"_sv)) {
        result = Value::integer(state.count);
    } else if (name.equals_ignoring_ascii_case("min"_sv) || name.equals_ignoring_ascii_case("max"_sv)) {
        result = state.best;
    } else if (name.equals_ignoring_ascii_case("group_concat"_sv)) {
        result = state.has_text ? Value::text(state.text->view()) : Value {};
    } else if (name.equals_ignoring_ascii_case("total"_sv)) {
        result = Value::real(state.real_sum);
    } else if (name.equals_ignoring_ascii_case("avg"_sv)) {
        result = state.count ? Value::real(state.real_sum / static_cast<double>(state.count)) : Value {};
    } else {
        if (state.count == 0) {
            result = Value {};
        } else if (state.sum_overflowed && !state.sum_is_real) {
            return fail("integer overflow"_sv);
        } else {
            result = state.sum_is_real ? Value::real(state.real_sum) : Value::integer(state.integer_sum);
        }
    }
    delete state.text;
    delete state.distinct;
    state.text = nullptr;
    state.distinct = nullptr;
    return result;
}

ErrorOr<void> Executor::run_select(Select const& const_select, Vector<Row>& results, bool name_columns)
{
    auto& select = const_cast<Select&>(const_select);
    m_contexts.clear();
    m_conjuncts.clear();
    m_aggregates.clear();
    m_stop = false;

    auto add_table = [&](StringView table_name, StringView alias, bool left) -> ErrorOr<void> {
        auto* table = m_catalogue.find_table(table_name);
        if (!table) {
            StringBuilder message;
            message.appendff("no such table: {}", table_name);
            return fail(message.view());
        }
        TableContext context;
        context.table = table;
        context.name = alias.is_empty() ? table->name.view() : alias;
        context.left_join = left;
        m_contexts.append(move(context));
        return {};
    };
    if (select.has_from) {
        TRY(add_table(select.table.view(), select.alias.view(), false));
        for (auto& join : select.joins)
            TRY(add_table(join.table.view(), join.alias.view(), join.left));
    }

    // Stars become columns; everything gets its table and column.
    struct OutputColumn {
        Expression const* expression { nullptr };
        int table { -1 };
        int column { -2 };
    };
    Vector<OutputColumn> outputs;
    Vector<String> names;
    for (auto& column : select.columns) {
        if (column.star) {
            if (m_contexts.is_empty())
                return fail("no tables specified"_sv);
            bool any = false;
            for (size_t t = 0; t < m_contexts.size(); ++t) {
                if (!column.star_table.is_empty() && !m_contexts[t].name.equals_ignoring_ascii_case(column.star_table.view()))
                    continue;
                any = true;
                for (size_t c = 0; c < m_contexts[t].table->columns.size(); ++c) {
                    int column_number = static_cast<int>(c) == m_contexts[t].table->rowid_column ? -1 : static_cast<int>(c);
                    outputs.append(OutputColumn { nullptr, static_cast<int>(t), column_number });
                    names.append(m_contexts[t].table->columns[c].name);
                }
            }
            if (!any) {
                StringBuilder message;
                message.appendff("no such table: {}", column.star_table.view());
                return fail(message.view());
            }
            continue;
        }
        TRY(resolve(*column.expression, true));
        TRY(collect_aggregates(*column.expression));
        outputs.append(OutputColumn { column.expression.ptr() });
        if (!column.alias.is_empty()) {
            names.append(column.alias);
        } else if (column.expression->kind == ExpressionKind::Column) {
            names.append(column.expression->name);
        } else {
            StringBuilder name;
            name.appendff("column{}", names.size() + 1);
            names.append(MUST(name.to_string()));
        }
    }

    if (select.where) {
        TRY(resolve(*select.where, false));
        Vector<Expression const*> conjuncts;
        find_conjuncts(*select.where, conjuncts);
        for (auto const* conjunct : conjuncts) {
            int level = highest_level_of(*conjunct);
            m_conjuncts.append(Conjunct { conjunct, level < 0 ? 0 : level });
        }
    }
    for (size_t i = 0; i < select.joins.size(); ++i) {
        auto& join = select.joins[i];
        if (!join.on)
            continue;
        TRY(resolve(*join.on, false));
        Vector<Expression const*> conjuncts;
        find_conjuncts(*join.on, conjuncts);
        for (auto const* conjunct : conjuncts) {
            int level = highest_level_of(*conjunct);
            if (join.left)
                m_conjuncts.append(Conjunct { conjunct, static_cast<int>(i) + 1, true, static_cast<int>(i) + 1 });
            else
                m_conjuncts.append(Conjunct { conjunct, level < 0 ? 0 : level });
        }
    }
    // GROUP BY can name a result column too, by position, or by an alias that isn't also a column.
    Vector<Expression const*> group_terms;
    for (auto& expression : select.group_by) {
        if (expression->kind == ExpressionKind::Literal && expression->literal.type() == Type::Integer) {
            i64 position = expression->literal.as_integer();
            if (position < 1 || static_cast<size_t>(position) > outputs.size() || !outputs[static_cast<size_t>(position - 1)].expression)
                return fail("GROUP BY term out of range"_sv);
            group_terms.append(outputs[static_cast<size_t>(position - 1)].expression);
            continue;
        }
        if (expression->kind == ExpressionKind::Column && expression->table.is_empty()) {
            bool is_column = false;
            for (auto const& context : m_contexts) {
                if (column_index_in(*context.table, expression->name.view()) >= 0)
                    is_column = true;
            }
            Expression const* aliased = nullptr;
            for (size_t i = 0; !is_column && i < select.columns.size(); ++i) {
                if (!select.columns[i].star && select.columns[i].alias.view().equals_ignoring_ascii_case(expression->name.view()))
                    aliased = select.columns[i].expression.ptr();
            }
            if (aliased) {
                group_terms.append(aliased);
                continue;
            }
        }
        TRY(resolve(*expression, false));
        group_terms.append(expression.ptr());
    }
    if (select.having) {
        TRY(resolve(*select.having, true));
        TRY(collect_aggregates(*select.having));
    }

    // ORDER BY can name a result column, by its alias or its position.
    struct OrderSource {
        int output { -1 };
        Expression const* expression { nullptr };
        bool descending { false };
    };
    Vector<OrderSource> order;
    for (auto& term : select.order_by) {
        OrderSource source;
        source.descending = term.descending;
        auto& expression = *term.expression;
        if (expression.kind == ExpressionKind::Literal && expression.literal.type() == Type::Integer) {
            i64 position = expression.literal.as_integer();
            if (position < 1 || static_cast<size_t>(position) > outputs.size())
                return fail("ORDER BY term out of range"_sv);
            source.output = static_cast<int>(position - 1);
        } else {
            if (expression.kind == ExpressionKind::Column && expression.table.is_empty()) {
                for (size_t i = 0; i < select.columns.size() && i < outputs.size(); ++i) {
                    if (!select.columns[i].star && select.columns[i].alias.view().equals_ignoring_ascii_case(expression.name.view())) {
                        source.output = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (source.output < 0) {
                TRY(resolve(expression, true));
                TRY(collect_aggregates(expression));
                source.expression = &expression;
            }
        }
        order.append(source);
    }

    bool const aggregated = !m_aggregates.is_empty() || !select.group_by.is_empty();

    i64 limit = -1;
    i64 offset = 0;
    if (select.limit) {
        limit = TRY(evaluate(*select.limit)).to_integer();
        if (select.offset)
            offset = max<i64>(0, TRY(evaluate(*select.offset)).to_integer());
    }

    AccessPath first_path;
    bool sorted_by_path = false;
    bool order_with_outputs = false;
    for (auto const& source : order) {
        if (source.output >= 0)
            order_with_outputs = true;
    }
    if (!m_contexts.is_empty() && !aggregated && !order.is_empty() && !order_with_outputs) {
        first_path = TRY(choose_path(0, &select));
        sorted_by_path = order_matches_path(select, first_path);
    }

    auto compute_output = [&](Row& row) -> ErrorOr<void> {
        MUST(row.try_ensure_capacity(outputs.size()));
        for (auto const& output : outputs) {
            if (output.expression) {
                row.append(TRY(evaluate(*output.expression)));
            } else {
                auto const& context = m_contexts[static_cast<size_t>(output.table)];
                if (context.is_null_row)
                    row.append(Value {});
                else if (output.column < 0)
                    row.append(Value::integer(context.rowid));
                else
                    row.append(context.values[static_cast<size_t>(output.column)]);
            }
        }
        return {};
    };

    auto compute_sort_keys = [&](Row const& output, Row& keys) -> ErrorOr<void> {
        for (auto const& source : order) {
            if (source.output >= 0)
                keys.append(output[static_cast<size_t>(source.output)]);
            else
                keys.append(TRY(evaluate(*source.expression)));
        }
        return {};
    };

    Vector<SortableRow> rows;
    HashMap<String, bool> seen;
    i64 produced = 0;

    auto emit_output = [&](Row output, Row keys) -> ErrorOr<bool> {
        if (select.distinct) {
            Vector<u8> encoded;
            for (auto const& value : output)
                encode_key(value, encoded);
            auto key = MUST(String::from_view(StringView(reinterpret_cast<char const*>(encoded.data()), encoded.size())));
            if (seen.contains(key.view()))
                return true;
            seen.set(move(key), true);
        }
        rows.append(SortableRow { move(output), move(keys) });
        ++produced;
        // With nothing left to sort, enough is enough.
        if ((order.is_empty() || sorted_by_path) && limit >= 0 && produced >= offset + limit)
            return false;
        return true;
    };

    HashMap<String, size_t> group_numbers;
    Vector<Group> groups;

    auto save_group_row = [&](Group& group) {
        group.row_values.clear();
        group.row_ids.clear();
        group.row_nulls.clear();
        for (auto const& context : m_contexts) {
            group.row_values.append(context.values);
            group.row_ids.append(context.rowid);
            group.row_nulls.append(context.is_null_row);
        }
    };
    auto restore_group_row = [&](Group const& group) {
        for (size_t i = 0; i < m_contexts.size() && i < group.row_values.size(); ++i) {
            m_contexts[i].values = group.row_values[i];
            m_contexts[i].rowid = group.row_ids[i];
            m_contexts[i].is_null_row = group.row_nulls[i];
        }
    };

    Function<ErrorOr<bool>()> on_row = [&]() -> ErrorOr<bool> {
        if (!aggregated) {
            Row output;
            TRY(compute_output(output));
            Row keys;
            if (!sorted_by_path)
                TRY(compute_sort_keys(output, keys));
            return emit_output(move(output), move(keys));
        }
        Vector<u8> key;
        for (auto const* expression : group_terms)
            encode_key(TRY(evaluate(*expression)), key);
        auto key_text = MUST(String::from_view(StringView(reinterpret_cast<char const*>(key.data()), key.size())));
        size_t number;
        if (auto* existing = group_numbers.get(key_text.view())) {
            number = *existing;
        } else {
            number = groups.size();
            Group group;
            group.key = key;
            MUST(group.states.try_resize(m_aggregates.size()));
            groups.append(move(group));
            group_numbers.set(move(key_text), number);
            save_group_row(groups[number]);
        }
        for (size_t i = 0; i < m_aggregates.size(); ++i) {
            auto const& expression = *m_aggregates[i].expression;
            bool is_star = !expression.children.is_empty() && expression.children[0]->kind == ExpressionKind::Star;
            Value value;
            if (!is_star && !expression.children.is_empty())
                value = TRY(evaluate(*expression.children[0]));
            apply_aggregate(groups[number].states[i], value, expression);
            // The row min() or max() found is what bare columns then show.
            if ((expression.name.view().equals_ignoring_ascii_case("min"_sv) || expression.name.view().equals_ignoring_ascii_case("max"_sv)) && m_aggregates.size() == 1 && compare(groups[number].states[i].best, value) == 0)
                save_group_row(groups[number]);
        }
        return true;
    };

    // Run it.
    if (m_contexts.is_empty()) {
        if (!select.where || TRY(evaluate(*select.where)).truth().value_or(false))
            (void)TRY(on_row());
    } else if (sorted_by_path) {
        bool matched = TRY(walk_path(0, first_path, on_row));
        (void)matched;
    } else {
        TRY(walk(0, on_row));
    }

    if (aggregated) {
        if (groups.is_empty() && select.group_by.is_empty()) {
            Group group;
            MUST(group.states.try_resize(m_aggregates.size()));
            for (auto& context : m_contexts)
                context.is_null_row = true;
            save_group_row(group);
            groups.append(move(group));
        }
        Vector<size_t> ordered;
        for (size_t i = 0; i < groups.size(); ++i)
            ordered.append(i);
        quick_sort(ordered.span(), [&](size_t a, size_t b) { return compare_key_bytes(groups[a].key.span(), groups[b].key.span()) < 0; });
        m_using_aggregate_values = true;
        for (auto number : ordered) {
            auto& group = groups[number];
            restore_group_row(group);
            m_aggregate_values.clear();
            for (size_t i = 0; i < m_aggregates.size(); ++i)
                m_aggregate_values.append(TRY(finish_aggregate(group.states[i], *m_aggregates[i].expression)));
            if (select.having && !TRY(evaluate(*select.having)).truth().value_or(false))
                continue;
            Row output;
            TRY(compute_output(output));
            Row keys;
            TRY(compute_sort_keys(output, keys));
            (void)TRY(emit_output(move(output), move(keys)));
        }
        m_using_aggregate_values = false;
    }

    if (!order.is_empty() && !sorted_by_path) {
        // Index order, so that rows that compare the same stay as they came.
        Vector<size_t> indices;
        for (size_t i = 0; i < rows.size(); ++i)
            indices.append(i);
        quick_sort(indices.span(), [&](size_t a, size_t b) {
            for (size_t k = 0; k < order.size(); ++k) {
                int result = compare(rows[a].keys[k], rows[b].keys[k]);
                if (result != 0)
                    return order[k].descending ? result > 0 : result < 0;
            }
            return a < b;
        });
        Vector<SortableRow> sorted;
        MUST(sorted.try_ensure_capacity(rows.size()));
        for (auto index : indices)
            sorted.append(move(rows[index]));
        rows = move(sorted);
    }

    results.clear();
    for (size_t i = static_cast<size_t>(min<i64>(offset, static_cast<i64>(rows.size()))); i < rows.size(); ++i) {
        if (limit >= 0 && static_cast<i64>(results.size()) >= limit)
            break;
        results.append(move(rows[i].result));
    }
    if (name_columns)
        m_statement.m_column_names = move(names);
    return {};
}

// ---- Rows.

void Executor::complete_values(TableInformation& table, Vector<Value>& values, i64 rowid)
{
    // A row from before a column was added has the column's default.
    while (values.size() < table.columns.size()) {
        auto const& column = table.columns[values.size()];
        if (column.default_value && column.default_value->kind == ExpressionKind::Literal)
            values.append(column.default_value->literal);
        else
            values.append(Value {});
    }
    if (table.rowid_column >= 0)
        values[static_cast<size_t>(table.rowid_column)] = Value::integer(rowid);
}

ErrorOr<void> Executor::read_row(TableInformation& table, i64 rowid, Vector<Value>& values, bool& found)
{
    Vector<u8> key;
    encode_rowid(rowid, key);
    Vector<u8> record;
    BTree tree(m_pager, table.root);
    found = TRY(tree.get(key.span(), record));
    if (!found)
        return {};
    TRY(decode_record(record.span(), values));
    complete_values(table, values, rowid);
    return {};
}

void Executor::make_index_key(IndexInformation const& index, Span<Value const> values, i64 rowid, Vector<u8>& key, bool with_rowid)
{
    key.clear();
    for (auto column : index.columns)
        encode_key(values[static_cast<size_t>(column)], key);
    if (with_rowid)
        encode_rowid(rowid, key);
}

ErrorOr<void> Executor::add_to_indexes(TableInformation& table, Span<Value const> values, i64 rowid)
{
    Vector<u8> key;
    for (auto& index : table.indexes) {
        make_index_key(*index, values, rowid, key, true);
        BTree tree(m_pager, index->root);
        TRY(tree.insert(key.span(), {}));
    }
    return {};
}

ErrorOr<void> Executor::remove_from_indexes(TableInformation& table, Span<Value const> values, i64 rowid)
{
    Vector<u8> key;
    for (auto& index : table.indexes) {
        make_index_key(*index, values, rowid, key, true);
        BTree tree(m_pager, index->root);
        (void)TRY(tree.remove(key.span()));
    }
    return {};
}

ErrorOr<Optional<i64>> Executor::find_conflict(TableInformation&, IndexInformation const& index, Span<Value const> values, i64 own_rowid, bool has_own)
{
    if (!index.unique)
        return Optional<i64> {};
    for (auto column : index.columns) {
        if (values[static_cast<size_t>(column)].is_null())
            return Optional<i64> {};
    }
    Vector<u8> prefix;
    make_index_key(index, values, 0, prefix, false);
    BTree tree(m_pager, index.root);
    auto cursor = TRY(tree.seek(prefix.span()));
    while (cursor.is_valid()) {
        auto key = cursor.key();
        if (key.size() != prefix.size() + 8 || __builtin_memcmp(key.data(), prefix.data(), prefix.size()) != 0)
            break;
        i64 other = decode_rowid(ReadonlyBytes(key.data() + prefix.size(), 8));
        if (!has_own || other != own_rowid)
            return Optional<i64>(other);
        TRY(cursor.next());
    }
    return Optional<i64> {};
}

ErrorOr<i64> Executor::next_rowid(TableInformation& table)
{
    BTree tree(m_pager, table.root);
    auto cursor = TRY(tree.last());
    if (!cursor.is_valid())
        return 1;
    i64 last = decode_rowid(cursor.key());
    if (last == NumericLimits<i64>::max())
        return fail("database or disk is full"_sv);
    return last + 1;
}

ErrorOr<void> Executor::delete_row(TableInformation& table, i64 rowid)
{
    Vector<Value> values;
    bool found = false;
    TRY(read_row(table, rowid, values, found));
    if (!found)
        return {};
    TRY(remove_from_indexes(table, values.span(), rowid));
    Vector<u8> key;
    encode_rowid(rowid, key);
    BTree tree(m_pager, table.root);
    (void)TRY(tree.remove(key.span()));
    return {};
}

ErrorOr<bool> Executor::insert_row(TableInformation& table, Vector<Value>& values, Optional<i64> rowid, ConflictAction action, i64* inserted_rowid)
{
    // Each value as its column would have it.
    for (size_t i = 0; i < table.columns.size(); ++i)
        values[i] = apply_affinity(move(values[i]), table.columns[i].affinity);

    i64 id = 0;
    bool has_id = false;
    if (table.rowid_column >= 0 && !values[static_cast<size_t>(table.rowid_column)].is_null()) {
        auto const& given = values[static_cast<size_t>(table.rowid_column)];
        if (given.type() != Type::Integer)
            return fail("datatype mismatch"_sv);
        id = given.as_integer();
        has_id = true;
    } else if (rowid.has_value()) {
        id = rowid.value();
        has_id = true;
    }
    if (!has_id)
        id = TRY(next_rowid(table));

    for (size_t i = 0; i < table.columns.size(); ++i) {
        if (table.columns[i].not_null && static_cast<int>(i) != table.rowid_column && values[i].is_null()) {
            if (action == ConflictAction::Ignore)
                return false;
            StringBuilder message;
            message.appendff("NOT NULL constraint failed: {}.{}", table.name.view(), table.columns[i].name.view());
            return fail(message.view());
        }
    }

    Vector<u8> key;
    encode_rowid(id, key);
    BTree tree(m_pager, table.root);

    // Whether something else already has its rowid, or a value that has to be unique.
    if (has_id) {
        Vector<u8> existing;
        if (TRY(tree.get(key.span(), existing))) {
            if (action == ConflictAction::Ignore)
                return false;
            if (action == ConflictAction::Abort) {
                StringBuilder message;
                if (table.rowid_column >= 0)
                    message.appendff("UNIQUE constraint failed: {}.{}", table.name.view(), table.columns[static_cast<size_t>(table.rowid_column)].name.view());
                else
                    message.appendff("UNIQUE constraint failed: {}.rowid", table.name.view());
                return fail(message.view());
            }
            TRY(delete_row(table, id));
        }
    }
    for (auto& index : table.indexes) {
        auto conflict = TRY(find_conflict(table, *index, values.span(), id, false));
        if (!conflict.has_value())
            continue;
        if (action == ConflictAction::Ignore)
            return false;
        if (action == ConflictAction::Abort) {
            StringBuilder message;
            message.appendff("UNIQUE constraint failed: {}", table.name.view());
            for (size_t i = 0; i < index->columns.size(); ++i)
                message.appendff("{}{}", i == 0 ? "."_sv : ", "_sv, table.columns[static_cast<size_t>(index->columns[i])].name.view());
            return fail(message.view());
        }
        TRY(delete_row(table, conflict.value()));
    }

    // The rowid's column is the key, so there's no need to write it down twice.
    Vector<u8> record;
    if (table.rowid_column >= 0) {
        Value saved = values[static_cast<size_t>(table.rowid_column)];
        values[static_cast<size_t>(table.rowid_column)] = Value {};
        encode_record(values.span(), record);
        values[static_cast<size_t>(table.rowid_column)] = Value::integer(id);
    } else {
        encode_record(values.span(), record);
    }
    TRY(tree.insert(key.span(), record.span()));
    TRY(add_to_indexes(table, values.span(), id));
    if (inserted_rowid)
        *inserted_rowid = id;
    return true;
}

ErrorOr<void> Executor::run_insert(Insert const& const_insert)
{
    auto& insert = const_cast<Insert&>(const_insert);
    auto* table = m_catalogue.find_table(insert.table.view());
    if (!table) {
        StringBuilder message;
        message.appendff("no such table: {}", insert.table.view());
        return fail(message.view());
    }
    Vector<int> targets;
    for (auto const& name : insert.columns) {
        int column = column_index_in(*table, name.view());
        if (column < 0) {
            StringBuilder message;
            message.appendff("table {} has no column named {}", table->name.view(), name.view());
            return fail(message.view());
        }
        targets.append(column);
    }

    Vector<Row> selected;
    if (insert.select) {
        // What's selected from is read before anything's written.
        TRY(run_select(*insert.select, selected, false));
    }
    m_contexts.clear();

    size_t const row_count = insert.select ? selected.size() : (insert.default_values ? 1 : insert.rows.size());
    size_t const wanted = targets.is_empty() ? table->columns.size() : targets.size();
    u64 changed = 0;

    return in_transaction([&]() -> ErrorOr<void> {
        for (size_t r = 0; r < row_count; ++r) {
            Vector<Value> values;
            MUST(values.try_resize(table->columns.size()));
            for (size_t c = 0; c < table->columns.size(); ++c) {
                if (table->columns[c].default_value)
                    values[c] = TRY(evaluate(*table->columns[c].default_value));
            }
            if (!insert.default_values) {
                size_t given = insert.select ? selected[r].size() : insert.rows[r].size();
                if (given != wanted) {
                    StringBuilder message;
                    message.appendff("table {} has {} columns but {} values were supplied", table->name.view(), wanted, given);
                    return fail(message.view());
                }
                for (size_t c = 0; c < given; ++c) {
                    size_t target = targets.is_empty() ? c : static_cast<size_t>(targets[c]);
                    values[target] = insert.select ? selected[r][c] : TRY(evaluate(*insert.rows[r][c]));
                }
            }
            i64 inserted = 0;
            if (TRY(insert_row(*table, values, {}, insert.on_conflict, &inserted))) {
                m_database.m_last_insert_rowid = inserted;
                ++changed;
            }
        }
        m_database.m_changes = changed;
        return {};
    });
}

ErrorOr<void> Executor::collect_rowids(TableInformation& table, StringView name, Expression* where, Vector<i64>& rowids)
{
    m_contexts.clear();
    m_conjuncts.clear();
    m_stop = false;
    TableContext context;
    context.table = &table;
    context.name = name;
    m_contexts.append(move(context));
    if (where) {
        TRY(resolve(*where, false));
        Vector<Expression const*> conjuncts;
        find_conjuncts(*where, conjuncts);
        for (auto const* conjunct : conjuncts)
            m_conjuncts.append(Conjunct { conjunct, 0 });
    }
    Function<ErrorOr<bool>()> on_row = [&]() -> ErrorOr<bool> {
        rowids.append(m_contexts[0].rowid);
        return true;
    };
    return walk(0, on_row);
}

ErrorOr<void> Executor::run_update(Update const& const_update)
{
    auto& update = const_cast<Update&>(const_update);
    auto* table = m_catalogue.find_table(update.table.view());
    if (!table) {
        StringBuilder message;
        message.appendff("no such table: {}", update.table.view());
        return fail(message.view());
    }
    Vector<i64> rowids;
    TRY(collect_rowids(*table, table->name.view(), update.where.ptr(), rowids));

    Vector<int> targets;
    bool changes_rowid = false;
    for (auto& assignment : update.assignments) {
        int column = column_index_in(*table, assignment.column.view());
        if (column < 0) {
            StringBuilder message;
            message.appendff("no such column: {}", assignment.column.view());
            return fail(message.view());
        }
        targets.append(column);
        if (column == table->rowid_column)
            changes_rowid = true;
        TRY(resolve(*assignment.value, false));
    }

    u64 changed = 0;
    return in_transaction([&]() -> ErrorOr<void> {
        for (auto rowid : rowids) {
            auto& context = m_contexts[0];
            bool found = false;
            TRY(read_row(*table, rowid, context.values, found));
            if (!found)
                continue;
            context.rowid = rowid;
            context.is_null_row = false;

            Vector<Value> updated = context.values;
            for (size_t i = 0; i < targets.size(); ++i)
                updated[static_cast<size_t>(targets[i])] = apply_affinity(TRY(evaluate(*update.assignments[i].value)), table->columns[static_cast<size_t>(targets[i])].affinity);

            if (changes_rowid) {
                TRY(delete_row(*table, rowid));
                i64 inserted = 0;
                if (TRY(insert_row(*table, updated, {}, update.on_conflict, &inserted)))
                    ++changed;
                continue;
            }

            for (size_t i = 0; i < table->columns.size(); ++i) {
                if (table->columns[i].not_null && static_cast<int>(i) != table->rowid_column && updated[i].is_null()) {
                    if (update.on_conflict == ConflictAction::Ignore)
                        goto next_row;
                    StringBuilder message;
                    message.appendff("NOT NULL constraint failed: {}.{}", table->name.view(), table->columns[i].name.view());
                    return fail(message.view());
                }
            }
            for (auto& index : table->indexes) {
                auto conflict = TRY(find_conflict(*table, *index, updated.span(), rowid, true));
                if (!conflict.has_value())
                    continue;
                if (update.on_conflict == ConflictAction::Ignore)
                    goto next_row;
                if (update.on_conflict == ConflictAction::Abort) {
                    StringBuilder message;
                    message.appendff("UNIQUE constraint failed: {}", table->name.view());
                    for (size_t i = 0; i < index->columns.size(); ++i)
                        message.appendff("{}{}", i == 0 ? "."_sv : ", "_sv, table->columns[static_cast<size_t>(index->columns[i])].name.view());
                    return fail(message.view());
                }
                TRY(delete_row(*table, conflict.value()));
            }

            TRY(remove_from_indexes(*table, context.values.span(), rowid));
            {
                Vector<u8> key;
                encode_rowid(rowid, key);
                Vector<u8> record;
                Vector<Value> stored = updated;
                if (table->rowid_column >= 0)
                    stored[static_cast<size_t>(table->rowid_column)] = Value {};
                encode_record(stored.span(), record);
                BTree tree(m_pager, table->root);
                TRY(tree.insert(key.span(), record.span()));
            }
            TRY(add_to_indexes(*table, updated.span(), rowid));
            ++changed;
        next_row:;
        }
        m_database.m_changes = changed;
        return {};
    });
}

ErrorOr<void> Executor::run_delete(Delete const& const_delete)
{
    auto& remove = const_cast<Delete&>(const_delete);
    auto* table = m_catalogue.find_table(remove.table.view());
    if (!table) {
        StringBuilder message;
        message.appendff("no such table: {}", remove.table.view());
        return fail(message.view());
    }
    Vector<i64> rowids;
    TRY(collect_rowids(*table, table->name.view(), remove.where.ptr(), rowids));
    return in_transaction([&]() -> ErrorOr<void> {
        for (auto rowid : rowids)
            TRY(delete_row(*table, rowid));
        m_database.m_changes = rowids.size();
        return {};
    });
}

// ---- The catalogue.

ErrorOr<void> Executor::add_catalogue_row(StringView kind, StringView name, StringView table, u32 root, Value text, i64* rowid)
{
    BTree catalogue(m_pager, m_pager.schema_root());
    auto cursor = TRY(catalogue.last());
    i64 id = cursor.is_valid() ? decode_rowid(cursor.key()) + 1 : 1;
    Vector<Value> values;
    values.append(Value::text(kind));
    values.append(Value::text(name));
    values.append(Value::text(table));
    values.append(Value::integer(root));
    values.append(move(text));
    Vector<u8> record;
    encode_record(values.span(), record);
    Vector<u8> key;
    encode_rowid(id, key);
    TRY(catalogue.insert(key.span(), record.span()));
    if (rowid)
        *rowid = id;
    return {};
}

ErrorOr<void> Executor::update_catalogue_text(TableInformation& table, StringView text)
{
    BTree catalogue(m_pager, m_pager.schema_root());
    Vector<u8> key;
    encode_rowid(table.catalogue_rowid, key);
    Vector<u8> record;
    if (!TRY(catalogue.get(key.span(), record)))
        return fail("the catalogue has lost a table"_sv);
    Vector<Value> values;
    TRY(decode_record(record.span(), values));
    values[CatalogueText] = Value::text(text);
    Vector<u8> updated;
    encode_record(values.span(), updated);
    TRY(catalogue.insert(key.span(), updated.span()));
    return {};
}

ErrorOr<void> Executor::remove_catalogue_row(StringView kind, StringView name)
{
    BTree catalogue(m_pager, m_pager.schema_root());
    auto cursor = TRY(catalogue.first());
    while (cursor.is_valid()) {
        Vector<u8> record;
        TRY(cursor.value(record));
        Vector<Value> values;
        TRY(decode_record(record.span(), values));
        if (values[CatalogueKind].as_text() == kind && values[CatalogueName].as_text() == name) {
            Vector<u8> key;
            for (auto byte : cursor.key())
                key.append(byte);
            (void)TRY(catalogue.remove(key.span()));
            return {};
        }
        TRY(cursor.next());
    }
    return {};
}

static void fill_table_information(TableInformation& table, CreateTable const& definition)
{
    table.name = definition.name;
    for (auto const& column : definition.columns) {
        ColumnInformation information;
        information.name = column.name;
        information.type = column.type;
        information.affinity = affinity_of_declared_type(column.type.view());
        information.not_null = column.not_null || column.primary_key;
        table.columns.append(move(information));
    }
    // INTEGER PRIMARY KEY, and that word exactly: this column is the rowid.
    for (size_t i = 0; i < definition.columns.size(); ++i) {
        auto const& column = definition.columns[i];
        bool sole_in_constraint = definition.primary_key.size() == 1 && definition.primary_key[0].view().equals_ignoring_ascii_case(column.name.view());
        if ((column.primary_key || sole_in_constraint) && column.type.view().equals_ignoring_ascii_case("INTEGER"_sv))
            table.rowid_column = static_cast<int>(i);
    }
}

static OwnPtr<Expression> clone_literal(Expression const& expression)
{
    auto copy = MUST(try_make<Expression>());
    copy->kind = ExpressionKind::Literal;
    copy->literal = expression.literal;
    return copy;
}

static void take_defaults(TableInformation& table, CreateTable const& definition)
{
    for (size_t i = 0; i < definition.columns.size(); ++i) {
        auto const& given = definition.columns[i].default_value;
        if (!given)
            continue;
        if (given->kind == ExpressionKind::Literal) {
            table.columns[i].default_value = clone_literal(*given);
        } else if (given->kind == ExpressionKind::Negate && given->children[0]->kind == ExpressionKind::Literal) {
            auto copy = MUST(try_make<Expression>());
            copy->kind = ExpressionKind::Literal;
            auto const& inner = given->children[0]->literal;
            copy->literal = inner.type() == Type::Integer ? Value::integer(-inner.as_integer()) : Value::real(-inner.to_real());
            table.columns[i].default_value = move(copy);
        }
    }
}

// Which constraints need an index of their own, in the order they're numbered.
static void for_each_automatic_index(TableInformation const& table, CreateTable const& definition, Function<void(Vector<int>)> callback)
{
    for (size_t i = 0; i < definition.columns.size(); ++i) {
        auto const& column = definition.columns[i];
        if ((column.primary_key && static_cast<int>(i) != table.rowid_column) || column.unique) {
            Vector<int> columns;
            columns.append(static_cast<int>(i));
            callback(move(columns));
        }
    }
    if (!definition.primary_key.is_empty() && !(definition.primary_key.size() == 1 && table.rowid_column >= 0 && definition.primary_key[0].view().equals_ignoring_ascii_case(table.columns[static_cast<size_t>(table.rowid_column)].name.view()))) {
        Vector<int> columns;
        for (auto const& name : definition.primary_key)
            columns.append(column_index_in(table, name.view()));
        callback(move(columns));
    }
    for (auto const& set : definition.unique_sets) {
        Vector<int> columns;
        for (auto const& name : set)
            columns.append(column_index_in(table, name.view()));
        callback(move(columns));
    }
}

ErrorOr<void> Executor::make_automatic_indexes(TableInformation& table, CreateTable const& definition)
{
    size_t number = 0;
    ErrorOr<void> result = {};
    for_each_automatic_index(table, definition, [&](Vector<int> columns) {
        if (result.is_error())
            return;
        auto add = [&]() -> ErrorOr<void> {
            for (auto column : columns) {
                if (column < 0)
                    return fail("a constraint names a column that isn't there"_sv);
            }
            ++number;
            StringBuilder name;
            name.appendff("vulture_autoindex_{}_{}", table.name.view(), number);
            auto index = TRY(try_make<IndexInformation>());
            index->name = TRY(name.to_string());
            index->table = table.name;
            index->columns = move(columns);
            index->unique = true;
            index->automatic = true;
            index->root = TRY(BTree::create(m_pager));
            TRY(add_catalogue_row("index"_sv, index->name.view(), table.name.view(), index->root, Value {}, nullptr));
            table.indexes.append(move(index));
            return {};
        };
        result = add();
    });
    return result;
}

ErrorOr<void> Executor::run_create_table(CreateTable const& definition, StringView text)
{
    if (m_catalogue.find_table(definition.name.view())) {
        if (definition.if_not_exists)
            return {};
        StringBuilder message;
        message.appendff("table {} already exists", definition.name.view());
        return fail(message.view());
    }
    return in_transaction([&]() -> ErrorOr<void> {
        auto table = TRY(try_make<TableInformation>());
        fill_table_information(*table, definition);
        take_defaults(*table, definition);
        table->text = TRY(String::from_view(text));
        table->root = TRY(BTree::create(m_pager));
        TRY(add_catalogue_row("table"_sv, table->name.view(), table->name.view(), table->root, Value::text(text), &table->catalogue_rowid));
        TRY(make_automatic_indexes(*table, definition));
        m_catalogue.tables.append(move(table));
        return {};
    });
}

ErrorOr<void> Executor::run_create_index(CreateIndex const& definition, StringView text)
{
    if (m_catalogue.find_index(definition.name.view())) {
        if (definition.if_not_exists)
            return {};
        StringBuilder message;
        message.appendff("index {} already exists", definition.name.view());
        return fail(message.view());
    }
    auto* table = m_catalogue.find_table(definition.table.view());
    if (!table) {
        StringBuilder message;
        message.appendff("no such table: {}", definition.table.view());
        return fail(message.view());
    }
    auto index = TRY(try_make<IndexInformation>());
    index->name = definition.name;
    index->table = table->name;
    index->unique = definition.unique;
    for (auto const& name : definition.columns) {
        int column = column_index_in(*table, name.view());
        if (column < 0) {
            StringBuilder message;
            message.appendff("no such column: {}", name.view());
            return fail(message.view());
        }
        index->columns.append(column);
    }
    return in_transaction([&]() -> ErrorOr<void> {
        index->root = TRY(BTree::create(m_pager));
        TRY(add_catalogue_row("index"_sv, index->name.view(), table->name.view(), index->root, Value::text(text), nullptr));
        // What's there already goes in.
        BTree rows(m_pager, table->root);
        BTree tree(m_pager, index->root);
        auto cursor = TRY(rows.first());
        Vector<u8> record;
        Vector<u8> key;
        Vector<Value> values;
        while (cursor.is_valid()) {
            i64 rowid = decode_rowid(cursor.key());
            TRY(cursor.value(record));
            TRY(decode_record(record.span(), values));
            complete_values(*table, values, rowid);
            if (index->unique) {
                auto conflict = TRY(find_conflict(*table, *index, values.span(), rowid, true));
                if (conflict.has_value())
                    return fail("UNIQUE constraint failed: the index can't be made"_sv);
            }
            make_index_key(*index, values.span(), rowid, key, true);
            TRY(tree.insert(key.span(), {}));
            TRY(cursor.next());
        }
        table->indexes.append(move(index));
        return {};
    });
}

ErrorOr<void> Executor::run_drop_table(StringView name, bool if_exists)
{
    auto* table = m_catalogue.find_table(name);
    if (!table) {
        if (if_exists)
            return {};
        StringBuilder message;
        message.appendff("no such table: {}", name);
        return fail(message.view());
    }
    return in_transaction([&]() -> ErrorOr<void> {
        for (auto& index : table->indexes) {
            BTree tree(m_pager, index->root);
            TRY(tree.destroy());
            TRY(remove_catalogue_row("index"_sv, index->name.view()));
        }
        BTree tree(m_pager, table->root);
        TRY(tree.destroy());
        TRY(remove_catalogue_row("table"_sv, table->name.view()));
        m_catalogue.tables.remove_all_matching([&](auto const& candidate) { return candidate.ptr() == table; });
        return {};
    });
}

ErrorOr<void> Executor::run_drop_index(StringView name, bool if_exists)
{
    TableInformation* table = nullptr;
    auto* index = m_catalogue.find_index(name, &table);
    if (!index) {
        if (if_exists)
            return {};
        StringBuilder message;
        message.appendff("no such index: {}", name);
        return fail(message.view());
    }
    if (index->automatic)
        return fail("an index that goes with a constraint can't be dropped"_sv);
    return in_transaction([&]() -> ErrorOr<void> {
        BTree tree(m_pager, index->root);
        TRY(tree.destroy());
        TRY(remove_catalogue_row("index"_sv, index->name.view()));
        table->indexes.remove_all_matching([&](auto const& candidate) { return candidate.ptr() == index; });
        return {};
    });
}

ErrorOr<void> Executor::run_add_column(StringView table_name, ColumnDefinition const& definition, StringView text)
{
    auto* table = m_catalogue.find_table(table_name);
    if (!table) {
        StringBuilder message;
        message.appendff("no such table: {}", table_name);
        return fail(message.view());
    }
    if (column_index_in(*table, definition.name.view()) >= 0) {
        StringBuilder message;
        message.appendff("duplicate column name: {}", definition.name.view());
        return fail(message.view());
    }
    if (definition.primary_key || definition.unique)
        return fail("a new column can't be a key"_sv);

    // The catalogue's text becomes what the table would have been made with. From after ADD [COLUMN] is the definition.
    size_t definition_start = 0;
    for (size_t i = 0; i + 5 <= text.length(); ++i) {
        if (text.substring_view(i, 5).equals_ignoring_ascii_case(" ADD "_sv)) {
            definition_start = i + 5;
            break;
        }
    }
    auto rest = text.substring_view(definition_start).trim_whitespace();
    if (rest.length() > 7 && rest.substring_view(0, 7).equals_ignoring_ascii_case("COLUMN "_sv))
        rest = rest.substring_view(7).trim_whitespace();
    auto old_text = table->text.view();
    auto closing = old_text.find_last(')');
    if (!closing.has_value())
        return fail("the catalogue's text isn't right"_sv);
    StringBuilder new_text;
    new_text.append(old_text.substring_view(0, closing.value()));
    new_text.append(", "_sv);
    new_text.append(rest);
    new_text.append(old_text.substring_view(closing.value()));

    return in_transaction([&]() -> ErrorOr<void> {
        TRY(update_catalogue_text(*table, new_text.view()));
        table->text = TRY(new_text.to_string());
        ColumnInformation column;
        column.name = definition.name;
        column.type = definition.type;
        column.affinity = affinity_of_declared_type(definition.type.view());
        column.not_null = definition.not_null;
        if (definition.default_value && definition.default_value->kind == ExpressionKind::Literal)
            column.default_value = clone_literal(*definition.default_value);
        table->columns.append(move(column));
        return {};
    });
}

ErrorOr<void> Executor::run_integrity_check()
{
    Vector<Row> results;
    auto report = [&](StringView problem) {
        Row row;
        row.append(Value::text(problem));
        results.append(move(row));
    };
    for (auto& table : m_catalogue.tables) {
        BTree tree(m_pager, table->root);
        auto counted = tree.check();
        if (counted.is_error()) {
            report(counted.error().string_literal());
            continue;
        }
        for (auto& index : table->indexes) {
            BTree index_tree(m_pager, index->root);
            auto index_counted = index_tree.check();
            if (index_counted.is_error()) {
                report(index_counted.error().string_literal());
                continue;
            }
            if (index_counted.value() != counted.value()) {
                StringBuilder message;
                message.appendff("index {} has {} entries for {} rows", index->name.view(), index_counted.value(), counted.value());
                report(message.view());
            }
        }
        auto cursor = TRY(tree.first());
        Vector<u8> record;
        Vector<Value> values;
        Vector<u8> key;
        Vector<u8> found_value;
        while (cursor.is_valid()) {
            i64 rowid = decode_rowid(cursor.key());
            TRY(cursor.value(record));
            TRY(decode_record(record.span(), values));
            complete_values(*table, values, rowid);
            for (auto& index : table->indexes) {
                make_index_key(*index, values.span(), rowid, key, true);
                BTree index_tree(m_pager, index->root);
                if (!TRY(index_tree.get(key.span(), found_value))) {
                    StringBuilder message;
                    message.appendff("row {} of {} is missing from index {}", rowid, table->name.view(), index->name.view());
                    report(message.view());
                }
            }
            TRY(cursor.next());
        }
    }
    u32 free_pages = 0;
    (void)free_pages;
    if (results.is_empty())
        report("ok"_sv);
    m_statement.m_results = move(results);
    m_statement.m_column_names.clear();
    m_statement.m_column_names.append(MUST(String::from_view("integrity_check"_sv)));
    return {};
}

ErrorOr<void> Executor::run()
{
    auto& statement = m_statement.m_statement;
    switch (statement.kind) {
    case StatementKind::Select:
        return run_select(*statement.select, m_statement.m_results, true);
    case StatementKind::CreateTable:
        return run_create_table(*statement.create_table, statement.text.view());
    case StatementKind::CreateIndex:
        return run_create_index(*statement.create_index, statement.text.view());
    case StatementKind::DropTable:
        return run_drop_table(statement.name.view(), statement.if_exists);
    case StatementKind::DropIndex:
        return run_drop_index(statement.name.view(), statement.if_exists);
    case StatementKind::AddColumn:
        return run_add_column(statement.name.view(), *statement.added_column, statement.text.view());
    case StatementKind::Insert:
        return run_insert(*statement.insert);
    case StatementKind::Update:
        return run_update(*statement.update);
    case StatementKind::Delete:
        return run_delete(*statement.remove);
    case StatementKind::Begin:
        if (m_database.m_explicit_transaction)
            return fail("cannot start a transaction within a transaction"_sv);
        TRY(m_pager.begin());
        m_database.m_explicit_transaction = true;
        return {};
    case StatementKind::Commit: {
        if (!m_database.m_explicit_transaction)
            return fail("cannot commit - no transaction is active"_sv);
        auto committed = m_pager.commit();
        if (committed.is_error()) {
            (void)m_pager.rollback();
            (void)m_database.load_catalogue();
            m_database.m_explicit_transaction = false;
            return committed;
        }
        m_database.m_explicit_transaction = false;
        return {};
    }
    case StatementKind::Rollback:
        if (!m_database.m_explicit_transaction)
            return fail("cannot rollback - no transaction is active"_sv);
        m_database.m_explicit_transaction = false;
        TRY(m_pager.rollback());
        return m_database.load_catalogue();
    case StatementKind::IntegrityCheck:
        return run_integrity_check();
    }
    VERIFY_NOT_REACHED();
}

// ---- The public face of it.

Statement::~Statement() = default;

ErrorOr<void> Statement::bind(size_t parameter, Value value)
{
    if (parameter == 0 || parameter > m_statement.parameter_count)
        return Error::from_string_literal("database: no such parameter");
    while (m_parameters.size() < m_statement.parameter_count)
        m_parameters.append(Value {});
    m_parameters[parameter - 1] = move(value);
    return {};
}

ErrorOr<bool> Statement::step()
{
    if (!m_executed) {
        Executor executor(m_database, *this);
        TRY(executor.run());
        m_executed = true;
        m_next_row = 0;
    }
    if (m_next_row >= m_results.size())
        return false;
    ++m_next_row;
    return true;
}

void Statement::reset()
{
    m_results.clear();
    m_next_row = 0;
    m_executed = false;
}

Database::Database(NonnullOwnPtr<Pager> pager)
    : m_pager(move(pager))
{
}

Database::~Database()
{
    if (m_explicit_transaction)
        (void)m_pager->rollback();
}

Error Database::sql_error(StringView message)
{
    m_last_error = MUST(String::from_view(message));
    return sql_failure;
}

void Database::set_cache_size(size_t bytes)
{
    m_pager->set_cache_pages(max<size_t>(16, bytes / page_size));
}

void Database::crash_after_writes_for_testing(i64 writes)
{
    m_pager->crash_after_writes_for_testing(writes);
}

ErrorOr<void> Database::load_catalogue()
{
    auto catalogue = TRY(try_make<Catalogue>());
    if (m_pager->schema_root() == 0) {
        m_catalogue = move(catalogue);
        return {};
    }
    BTree tree(*m_pager, m_pager->schema_root());

    struct IndexRow {
        i64 rowid;
        String name;
        String table;
        u32 root;
        String text;
        bool has_text;
    };
    Vector<IndexRow> index_rows;
    HashMap<String, u32> automatic_roots;
    Vector<u8> record;
    Vector<Value> values;
    auto cursor = TRY(tree.first());
    while (cursor.is_valid()) {
        i64 rowid = decode_rowid(cursor.key());
        TRY(cursor.value(record));
        TRY(decode_record(record.span(), values));
        if (values[CatalogueKind].as_text() == "table"_sv) {
            String message;
            auto parsed = TRY(parse(values[CatalogueText].as_text(), message));
            if (parsed.kind != StatementKind::CreateTable)
                return Error::from_string_literal("database: the catalogue is damaged");
            auto table = TRY(try_make<TableInformation>());
            fill_table_information(*table, *parsed.create_table);
            take_defaults(*table, *parsed.create_table);
            table->root = static_cast<u32>(values[CatalogueRoot].to_integer());
            table->catalogue_rowid = rowid;
            table->text = TRY(String::from_view(values[CatalogueText].as_text()));
            catalogue->tables.append(move(table));
        } else {
            index_rows.append(IndexRow { rowid, TRY(String::from_view(values[CatalogueName].as_text())), TRY(String::from_view(values[CatalogueTable].as_text())),
                static_cast<u32>(values[CatalogueRoot].to_integer()), TRY(String::from_view(values[CatalogueText].as_text())), !values[CatalogueText].is_null() });
        }
        TRY(cursor.next());
    }

    for (auto const& row : index_rows) {
        if (row.has_text) {
            auto* table = catalogue->find_table(row.table.view());
            if (!table)
                return Error::from_string_literal("database: the catalogue is damaged");
            String message;
            auto parsed = TRY(parse(row.text.view(), message));
            if (parsed.kind != StatementKind::CreateIndex)
                return Error::from_string_literal("database: the catalogue is damaged");
            auto index = TRY(try_make<IndexInformation>());
            index->name = row.name;
            index->table = row.table;
            index->unique = parsed.create_index->unique;
            index->root = row.root;
            for (auto const& name : parsed.create_index->columns)
                index->columns.append(column_index_in(*table, name.view()));
            table->indexes.append(move(index));
        } else {
            automatic_roots.set(row.name, row.root);
        }
    }

    // Which constraint each automatic index belongs to is how many came before it.
    for (auto& table : catalogue->tables) {
        String message;
        auto parsed = TRY(parse(table->text.view(), message));
        size_t number = 0;
        auto& info = *table;
        for_each_automatic_index(info, *parsed.create_table, [&](Vector<int> columns) {
            ++number;
            StringBuilder name;
            name.appendff("vulture_autoindex_{}_{}", info.name.view(), number);
            auto const* root = automatic_roots.get(name.view());
            if (!root)
                return;
            auto index = MUST(try_make<IndexInformation>());
            index->name = MUST(name.to_string());
            index->table = info.name;
            index->columns = move(columns);
            index->unique = true;
            index->automatic = true;
            index->root = *root;
            info.indexes.append(move(index));
        });
    }

    m_catalogue = move(catalogue);
    return {};
}

ErrorOr<NonnullOwnPtr<Database>> Database::open(StringView path)
{
    auto pager = TRY(Pager::open(path));
    auto* raw = new (Base::nothrow) Database(move(pager));
    if (!raw)
        return Error::from_errno(12);
    auto database = adopt_own(*raw);
    if (database->m_pager->schema_root() == 0) {
        TRY(database->m_pager->begin());
        database->m_pager->set_schema_root(TRY(BTree::create(*database->m_pager)));
        TRY(database->m_pager->commit());
    }
    TRY(database->load_catalogue());
    return database;
}

ErrorOr<NonnullOwnPtr<Statement>> Database::prepare(StringView text)
{
    String message;
    auto parsed = parse(text, message);
    if (parsed.is_error()) {
        if (!message.is_empty())
            m_last_error = move(message);
        return parsed.release_error();
    }
    auto* raw = new (Base::nothrow) Statement(*this, parsed.release_value());
    if (!raw)
        return Error::from_errno(12);
    return adopt_own(*raw);
}

ErrorOr<void> Database::execute(StringView text, Span<Value const> parameters)
{
    (void)TRY(query(text, parameters));
    return {};
}

ErrorOr<Vector<Row>> Database::query(StringView text, Span<Value const> parameters)
{
    auto statement = TRY(prepare(text));
    for (size_t i = 0; i < parameters.size(); ++i)
        TRY(statement->bind(i + 1, parameters[i]));
    (void)TRY(statement->step());
    return move(statement->m_results);
}

}
