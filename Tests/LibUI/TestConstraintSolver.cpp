/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibTest/TestCase.h>
#include <LibUI/ConstraintSolver.h>

using UI::ConstraintSolver;
using Term = ConstraintSolver::Term;
using Relation = ConstraintSolver::Relation;

namespace {

bool near(double a, double b)
{
    double difference = a - b;
    return difference > -0.0001 && difference < 0.0001;
}

ConstraintSolver::ConstraintId add(ConstraintSolver& solver, Term a, double constant, Relation relation, double strength = ConstraintSolver::required)
{
    Term terms[] = { a };
    return MUST(solver.add_constraint({ terms, 1 }, constant, relation, strength));
}

ConstraintSolver::ConstraintId add(ConstraintSolver& solver, Term a, Term b, double constant, Relation relation, double strength = ConstraintSolver::required)
{
    Term terms[] = { a, b };
    return MUST(solver.add_constraint({ terms, 2 }, constant, relation, strength));
}

}

TEST_CASE(an_equality_holds)
{
    ConstraintSolver solver;
    auto x = solver.new_variable();
    add(solver, { x, 1 }, -100, Relation::Equal);
    EXPECT(near(solver.value_of(x), 100));
}

TEST_CASE(sums_and_inequalities)
{
    ConstraintSolver solver;
    auto left = solver.new_variable();
    auto width = solver.new_variable();
    auto right = solver.new_variable();
    Term sum[] = { { right, 1 }, { left, -1 }, { width, -1 } };
    MUST(solver.add_constraint({ sum, 3 }, 0, Relation::Equal));
    add(solver, { left, 1 }, -10, Relation::Equal);
    add(solver, { width, 1 }, -90, Relation::Equal);
    EXPECT(near(solver.value_of(right), 100));
}

TEST_CASE(weak_gives_way_to_required)
{
    ConstraintSolver solver;
    auto x = solver.new_variable();
    add(solver, { x, 1 }, -50, Relation::Equal, ConstraintSolver::weak);
    EXPECT(near(solver.value_of(x), 50));
    add(solver, { x, 1 }, -30, Relation::LessOrEqual);
    EXPECT(near(solver.value_of(x), 30));
}

TEST_CASE(strong_beats_weak)
{
    ConstraintSolver solver;
    auto x = solver.new_variable();
    add(solver, { x, 1 }, -10, Relation::Equal, ConstraintSolver::weak);
    add(solver, { x, 1 }, -20, Relation::Equal, ConstraintSolver::strong);
    EXPECT(near(solver.value_of(x), 20));
    add(solver, { x, 1 }, -30, Relation::Equal, ConstraintSolver::medium);
    EXPECT(near(solver.value_of(x), 20));
}

TEST_CASE(equal_strengths_both_held_as_far_as_can_be)
{
    ConstraintSolver solver;
    auto a = solver.new_variable();
    auto b = solver.new_variable();
    // a + b = 100 is required; a wants 80, b wants 80.
    add(solver, { a, 1 }, { b, 1 }, -100, Relation::Equal);
    add(solver, { a, 1 }, -80, Relation::Equal, ConstraintSolver::strong);
    add(solver, { b, 1 }, -80, Relation::Equal, ConstraintSolver::strong);
    EXPECT(near(solver.value_of(a) + solver.value_of(b), 100));
    EXPECT(solver.value_of(a) <= 80.0001 && solver.value_of(b) <= 80.0001);
}

TEST_CASE(impossible_required_constraints_are_refused)
{
    ConstraintSolver solver;
    auto x = solver.new_variable();
    add(solver, { x, 1 }, -10, Relation::Equal);
    Term term[] = { { x, 1 } };
    EXPECT(solver.add_constraint({ term, 1 }, -20, Relation::Equal).is_error());
    // And nothing is broken by the attempt.
    EXPECT(near(solver.value_of(x), 10));
    auto y = solver.new_variable();
    add(solver, { x, 1 }, { y, 1 }, -30, Relation::Equal);
    EXPECT(near(solver.value_of(y), 20));
}

TEST_CASE(impossible_inequalities_are_refused)
{
    ConstraintSolver solver;
    auto x = solver.new_variable();
    add(solver, { x, 1 }, -100, Relation::GreaterOrEqual);
    Term term[] = { { x, 1 } };
    EXPECT(solver.add_constraint({ term, 1 }, -50, Relation::LessOrEqual).is_error());
    EXPECT(solver.value_of(x) >= 99.9999);
}

TEST_CASE(removing_a_constraint_lets_others_take_over)
{
    ConstraintSolver solver;
    auto x = solver.new_variable();
    add(solver, { x, 1 }, -10, Relation::Equal, ConstraintSolver::weak);
    auto limit = add(solver, { x, 1 }, -100, Relation::Equal, ConstraintSolver::strong);
    EXPECT(near(solver.value_of(x), 100));
    solver.remove_constraint(limit);
    EXPECT(near(solver.value_of(x), 10));
    auto required = add(solver, { x, 1 }, -40, Relation::Equal);
    EXPECT(near(solver.value_of(x), 40));
    solver.remove_constraint(required);
    EXPECT(near(solver.value_of(x), 10));
}

TEST_CASE(a_window_of_views)
{
    // |--[button]--8--[field >= 100, as wide as it can]--|, in a width of 400, and
    // the button at its natural 80 unless that cannot be.
    ConstraintSolver solver;
    auto button_left = solver.new_variable();
    auto button_width = solver.new_variable();
    auto field_left = solver.new_variable();
    auto field_width = solver.new_variable();
    auto window_width = solver.new_variable();
    auto window = add(solver, { window_width, 1 }, -400, Relation::Equal);
    add(solver, { button_left, 1 }, -20, Relation::Equal);
    Term gap[] = { { field_left, 1 }, { button_left, -1 }, { button_width, -1 } };
    MUST(solver.add_constraint({ gap, 3 }, -8, Relation::Equal));
    Term trailing[] = { { window_width, 1 }, { field_left, -1 }, { field_width, -1 } };
    MUST(solver.add_constraint({ trailing, 3 }, -20, Relation::Equal));
    add(solver, { field_width, 1 }, -100, Relation::GreaterOrEqual);
    add(solver, { button_width, 1 }, -80, Relation::Equal, ConstraintSolver::medium);
    EXPECT(near(solver.value_of(button_width), 80));
    EXPECT(near(solver.value_of(field_left), 108));
    EXPECT(near(solver.value_of(field_width), 272));

    // Narrow: the field is at its floor, and the button gives.
    solver.remove_constraint(window);
    add(solver, { window_width, 1 }, -200, Relation::Equal);
    EXPECT(near(solver.value_of(field_width), 100));
    EXPECT(near(solver.value_of(button_width), 52));
}

TEST_CASE(many_constraints)
{
    // A row of a hundred, each next to the last: required chain, weak widths.
    ConstraintSolver solver;
    Vector<ConstraintSolver::Variable> lefts;
    Vector<ConstraintSolver::Variable> widths;
    for (int i = 0; i < 100; ++i) {
        lefts.append(solver.new_variable());
        widths.append(solver.new_variable());
        add(solver, { widths[static_cast<size_t>(i)], 1 }, -10, Relation::Equal, ConstraintSolver::weak);
    }
    add(solver, { lefts[0], 1 }, 0, Relation::Equal);
    for (size_t i = 1; i < 100; ++i) {
        Term chain[] = { { lefts[i], 1 }, { lefts[i - 1], -1 }, { widths[i - 1], -1 } };
        MUST(solver.add_constraint({ chain, 3 }, 0, Relation::Equal));
    }
    EXPECT(near(solver.value_of(lefts[99]), 990));
}
