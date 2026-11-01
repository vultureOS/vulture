/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibUI/ConstraintSolver.h>
#include <Kernel/API/POSIX/errno.h>

namespace UI {

namespace {

constexpr double epsilon = 1e-8;

bool is_near_zero(double value)
{
    return value > -epsilon && value < epsilon;
}

}

void ConstraintSolver::Row::insert(Symbol symbol, double coefficient)
{
    size_t at = 0;
    while (at < cells.size() && cells[at].symbol < symbol)
        ++at;
    if (at < cells.size() && cells[at].symbol == symbol) {
        cells[at].coefficient += coefficient;
        if (is_near_zero(cells[at].coefficient))
            cells.remove(at);
        return;
    }
    if (!is_near_zero(coefficient))
        cells.insert(at, Cell { symbol, coefficient });
}

void ConstraintSolver::Row::insert(Row const& other, double factor)
{
    constant += other.constant * factor;
    for (auto const& cell : other.cells)
        insert(cell.symbol, cell.coefficient * factor);
}

void ConstraintSolver::Row::remove(Symbol symbol)
{
    cells.remove_all_matching([&](auto const& cell) { return cell.symbol == symbol; });
}

double ConstraintSolver::Row::coefficient_of(Symbol symbol) const
{
    for (auto const& cell : cells) {
        if (cell.symbol == symbol)
            return cell.coefficient;
    }
    return 0;
}

void ConstraintSolver::Row::reverse_sign()
{
    constant = -constant;
    for (auto& cell : cells)
        cell.coefficient = -cell.coefficient;
}

void ConstraintSolver::Row::solve_for(Symbol symbol)
{
    double factor = -1 / coefficient_of(symbol);
    remove(symbol);
    constant *= factor;
    for (auto& cell : cells)
        cell.coefficient *= factor;
}

void ConstraintSolver::Row::substitute(Symbol symbol, Row const& row)
{
    double coefficient = coefficient_of(symbol);
    if (coefficient == 0)
        return;
    remove(symbol);
    insert(row, coefficient);
}

ConstraintSolver::Variable ConstraintSolver::new_variable()
{
    return m_next_variable++;
}

ConstraintSolver::Symbol ConstraintSolver::new_symbol(Kind kind)
{
    return m_next_symbol++ | static_cast<u32>(kind);
}

ConstraintSolver::BasicRow* ConstraintSolver::row_of(Symbol symbol)
{
    for (auto& row : m_rows) {
        if (row.basic == symbol)
            return &row;
    }
    return nullptr;
}

ConstraintSolver::BasicRow const* ConstraintSolver::row_of(Symbol symbol) const
{
    for (auto const& row : m_rows) {
        if (row.basic == symbol)
            return &row;
    }
    return nullptr;
}

void ConstraintSolver::remove_row(Symbol symbol)
{
    m_rows.remove_all_matching([&](auto const& row) { return row.basic == symbol; });
}

void ConstraintSolver::substitute_everywhere(Symbol symbol, Row const& row)
{
    for (auto& other : m_rows) {
        other.row.substitute(symbol, row);
        // A restricted variable never goes below zero: the tableau stays feasible.
    }
    m_objective.substitute(symbol, row);
}

bool ConstraintSolver::all_dummies(Row const& row)
{
    for (auto const& cell : row.cells) {
        if (kind_of(cell.symbol) != Kind::Dummy)
            return false;
    }
    return true;
}

void ConstraintSolver::add_error_to_objective(Symbol symbol, double strength)
{
    if (auto const* row = row_of(symbol))
        m_objective.insert(row->row, strength);
    else
        m_objective.insert(symbol, strength);
}

void ConstraintSolver::remove_error_from_objective(Symbol symbol, double strength)
{
    if (auto const* row = row_of(symbol))
        m_objective.insert(row->row, -strength);
    else
        m_objective.insert(symbol, -strength);
}

ConstraintSolver::Symbol ConstraintSolver::choose_subject(Row const& row, Tag const& tag) const
{
    for (auto const& cell : row.cells) {
        if (kind_of(cell.symbol) == Kind::External)
            return cell.symbol;
    }
    // A new slack or error variable with a negative coefficient can enter as is.
    if (kind_of(tag.marker) == Kind::Slack || kind_of(tag.marker) == Kind::Error) {
        if (row.coefficient_of(tag.marker) < 0)
            return tag.marker;
    }
    if (tag.other && (kind_of(tag.other) == Kind::Slack || kind_of(tag.other) == Kind::Error)) {
        if (row.coefficient_of(tag.other) < 0)
            return tag.other;
    }
    return 0;
}

bool ConstraintSolver::add_with_artificial(Row const& row)
{
    // A second simplex, to find a feasible point: minimize an artificial variable
    // that stands for this row. If it cannot reach zero, the constraint cannot hold.
    Symbol artificial = new_symbol(Kind::Artificial);
    m_rows.append(BasicRow { artificial, row });
    Row artificial_objective = row;
    optimize(artificial_objective);
    bool feasible = is_near_zero(artificial_objective.constant);

    if (auto* basic = row_of(artificial)) {
        if (basic->row.cells.is_empty()) {
            remove_row(artificial);
            return feasible;
        }
        Symbol entering = 0;
        for (auto const& cell : basic->row.cells) {
            if (kind_of(cell.symbol) != Kind::Dummy) {
                entering = cell.symbol;
                break;
            }
        }
        if (!entering) {
            remove_row(artificial);
            return false;
        }
        Row pivoted = basic->row;
        remove_row(artificial);
        pivoted.insert(artificial, -1);
        pivoted.solve_for(entering);
        substitute_everywhere(entering, pivoted);
        artificial_objective.substitute(entering, pivoted);
        m_rows.append(BasicRow { entering, move(pivoted) });
    }
    for (auto& other : m_rows)
        other.row.remove(artificial);
    m_objective.remove(artificial);
    return feasible;
}

ErrorOr<ConstraintSolver::ConstraintId> ConstraintSolver::add_constraint(Span<Term const> terms, double constant, Relation relation, double strength)
{
    if (strength > required)
        strength = required;
    Row row;
    row.constant = constant;
    // In terms of what is not basic now.
    for (size_t i = 0; i < terms.size(); ++i) {
        if (is_near_zero(terms[i].coefficient))
            continue;
        Symbol symbol = terms[i].variable | static_cast<u32>(Kind::External);
        if (auto const* basic = row_of(symbol))
            row.insert(basic->row, terms[i].coefficient);
        else
            row.insert(symbol, terms[i].coefficient);
    }

    Tag tag { m_next_constraint++, 0, 0, strength };
    bool is_required = strength >= required;
    if (relation == Relation::Equal) {
        if (is_required) {
            tag.marker = new_symbol(Kind::Dummy);
            row.insert(tag.marker, 1);
        } else {
            tag.marker = new_symbol(Kind::Error);
            tag.other = new_symbol(Kind::Error);
            row.insert(tag.marker, -1);
            row.insert(tag.other, 1);
            add_error_to_objective(tag.marker, strength);
            add_error_to_objective(tag.other, strength);
        }
    } else {
        double sign = relation == Relation::LessOrEqual ? 1 : -1;
        tag.marker = new_symbol(Kind::Slack);
        row.insert(tag.marker, sign);
        if (!is_required) {
            tag.other = new_symbol(Kind::Error);
            row.insert(tag.other, -sign);
            add_error_to_objective(tag.other, strength);
        }
    }
    if (row.constant < 0)
        row.reverse_sign();

    Symbol subject = choose_subject(row, tag);
    if (!subject && all_dummies(row)) {
        if (!is_near_zero(row.constant))
            return Error::from_errno(EDOM);
        subject = tag.marker;
    }
    if (subject) {
        row.solve_for(subject);
        substitute_everywhere(subject, row);
        m_rows.append(BasicRow { subject, move(row) });
    } else {
        // The search pivots as it goes, and a refused constraint must leave no trace.
        auto rows_before = m_rows;
        auto objective_before = m_objective;
        if (!add_with_artificial(row)) {
            m_rows = move(rows_before);
            m_objective = move(objective_before);
            return Error::from_errno(EDOM);
        }
    }
    m_tags.append(tag);
    optimize(m_objective);
    return tag.id;
}

void ConstraintSolver::optimize(Row& objective)
{
    for (;;) {
        // The first that lowers the objective: first keeps it from cycling.
        Symbol entering = 0;
        for (auto const& cell : objective.cells) {
            if (kind_of(cell.symbol) != Kind::Dummy && kind_of(cell.symbol) != Kind::Artificial && cell.coefficient < -epsilon) {
                entering = cell.symbol;
                break;
            }
        }
        if (!entering)
            return;
        // The row that stops it soonest.
        BasicRow* leaving = nullptr;
        double smallest = 0;
        for (auto& row : m_rows) {
            if (!is_restricted(row.basic))
                continue;
            double coefficient = row.row.coefficient_of(entering);
            if (coefficient >= -epsilon)
                continue;
            double ratio = -row.row.constant / coefficient;
            if (!leaving || ratio < smallest) {
                leaving = &row;
                smallest = ratio;
            }
        }
        // Unbounded: it can only happen to an objective that has no floor.
        if (!leaving)
            return;
        Symbol leaving_symbol = leaving->basic;
        Row row = move(leaving->row);
        remove_row(leaving_symbol);
        row.insert(leaving_symbol, -1);
        row.solve_for(entering);
        substitute_everywhere(entering, row);
        // When it is not the main objective, that needs it too.
        if (&objective != &m_objective)
            objective.substitute(entering, row);
        m_rows.append(BasicRow { entering, move(row) });
    }
}

ConstraintSolver::BasicRow* ConstraintSolver::leaving_row_for_marker(Symbol marker)
{
    BasicRow* first = nullptr;
    BasicRow* second = nullptr;
    BasicRow* third = nullptr;
    double first_ratio = 0, second_ratio = 0;
    for (auto& row : m_rows) {
        double coefficient = row.row.coefficient_of(marker);
        if (coefficient == 0)
            continue;
        if (!is_restricted(row.basic)) {
            third = &row;
        } else if (coefficient < 0) {
            double ratio = -row.row.constant / coefficient;
            if (!first || ratio < first_ratio) {
                first = &row;
                first_ratio = ratio;
            }
        } else {
            double ratio = row.row.constant / coefficient;
            if (!second || ratio < second_ratio) {
                second = &row;
                second_ratio = ratio;
            }
        }
    }
    if (first)
        return first;
    if (second)
        return second;
    return third;
}

void ConstraintSolver::remove_constraint(ConstraintId id)
{
    size_t index = m_tags.size();
    for (size_t i = 0; i < m_tags.size(); ++i) {
        if (m_tags[i].id == id)
            index = i;
    }
    if (index == m_tags.size())
        return;
    Tag tag = m_tags[index];
    m_tags.remove(index);

    if (kind_of(tag.marker) == Kind::Error)
        remove_error_from_objective(tag.marker, tag.strength);
    if (tag.other && kind_of(tag.other) == Kind::Error)
        remove_error_from_objective(tag.other, tag.strength);

    if (row_of(tag.marker)) {
        remove_row(tag.marker);
    } else if (auto* leaving = leaving_row_for_marker(tag.marker)) {
        Symbol leaving_symbol = leaving->basic;
        Row row = move(leaving->row);
        remove_row(leaving_symbol);
        row.insert(leaving_symbol, -1);
        row.solve_for(tag.marker);
        substitute_everywhere(tag.marker, row);
    }
    optimize(m_objective);
}

double ConstraintSolver::value_of(Variable variable) const
{
    Symbol symbol = variable | static_cast<u32>(Kind::External);
    if (auto const* row = row_of(symbol))
        return row->row.constant;
    return 0;
}

}
