/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/Span.h>
#include <LibBase/Vector.h>

// Linear constraints, some stronger than others: the Cassowary algorithm (Badros,
// Borning and Stuckey), an incremental simplex over an always-feasible tableau.
// Required constraints all hold; the others hold as far as those allow, the
// stronger first.

namespace UI {

class ConstraintSolver {
public:
    using Variable = u32;
    using ConstraintId = u32;

    enum class Relation : u8 {
        LessOrEqual,
        Equal,
        GreaterOrEqual,
    };

    struct Term {
        Variable variable;
        double coefficient;
    };

    static constexpr double required = 1'000'000'000.0;
    static constexpr double strong = 1'000'000.0;
    static constexpr double medium = 1'000.0;
    static constexpr double weak = 1.0;

    [[nodiscard]] Variable new_variable();
    // terms + constant RELATION 0. EDOM if it is required and cannot hold.
    [[nodiscard]] ErrorOr<ConstraintId> add_constraint(Span<Term const>, double constant, Relation, double strength = required);
    void remove_constraint(ConstraintId);

    [[nodiscard]] double value_of(Variable) const;

private:
    // Which of the tableau's kinds, in the top bits of a symbol.
    enum class Kind : u32 {
        External = 0,
        Slack = 1u << 28,
        Error = 2u << 28,
        Dummy = 3u << 28,
        Artificial = 4u << 28,
    };
    static constexpr u32 kind_mask = 0xf0000000u;
    using Symbol = u32;

    [[nodiscard]] static Kind kind_of(Symbol symbol) { return static_cast<Kind>(symbol & kind_mask); }
    [[nodiscard]] static bool is_restricted(Symbol symbol) { return kind_of(symbol) != Kind::External; }
    [[nodiscard]] Symbol new_symbol(Kind);

    struct Cell {
        Symbol symbol;
        double coefficient;
    };

    // constant + the sum of the cells, which are kept sorted by symbol.
    struct Row {
        double constant { 0 };
        Vector<Cell> cells;

        void add(double value) { constant += value; }
        void insert(Symbol, double coefficient);
        void insert(Row const&, double factor = 1);
        void remove(Symbol);
        [[nodiscard]] double coefficient_of(Symbol) const;
        void reverse_sign();
        // This row is `0 = ...`; rewrites it as `symbol = ...`, symbol taken out.
        void solve_for(Symbol);
        // symbol = row, into this one.
        void substitute(Symbol, Row const&);
    };

    struct BasicRow {
        Symbol basic;
        Row row;
    };

    struct Tag {
        ConstraintId id;
        Symbol marker;
        Symbol other;
        double strength;
    };

    [[nodiscard]] BasicRow* row_of(Symbol);
    [[nodiscard]] BasicRow const* row_of(Symbol) const;
    void remove_row(Symbol);
    void substitute_everywhere(Symbol, Row const&);
    void optimize(Row& objective);
    [[nodiscard]] Symbol choose_subject(Row const&, Tag const&) const;
    [[nodiscard]] bool add_with_artificial(Row const&);
    [[nodiscard]] static bool all_dummies(Row const&);
    void add_error_to_objective(Symbol, double strength);
    void remove_error_from_objective(Symbol, double strength);
    [[nodiscard]] BasicRow* leaving_row_for_marker(Symbol);

    Vector<BasicRow> m_rows;
    Row m_objective;
    Vector<Tag> m_tags;
    u32 m_next_variable { 1 };
    u32 m_next_symbol { 1 };
    ConstraintId m_next_constraint { 1 };
};

}
