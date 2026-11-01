/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibBase/Math.h>
#include <LibUI/Layout.h>

namespace UI {

using namespace Gfx;

namespace {

// A view's preferred size, with nothing but zero where it has no opinion.
FloatSize wanted(View const& view)
{
    auto size = view.preferred_size();
    return { max(size.width, view.minimum_size().width), max(size.height, view.minimum_size().height) };
}

float round_point(float value)
{
    // To half points: every display we draw for has whole pixels there, and rows of
    // controls stay on the grid.
    return static_cast<float>(Base::Math::round(static_cast<double>(value) * 2)) / 2;
}

}

void StackLayout::apply(View& view)
{
    bool horizontal = m_axis == Axis::Horizontal;
    float main_start = horizontal ? m_insets.left : m_insets.top;
    float main_end = horizontal ? view.width() - m_insets.right : view.height() - m_insets.bottom;
    float cross_start = horizontal ? m_insets.top : m_insets.left;
    float cross_end = horizontal ? view.height() - m_insets.bottom : view.width() - m_insets.right;
    float main_room = max(main_end - main_start, 0.0f);
    float cross_room = max(cross_end - cross_start, 0.0f);

    Vector<View*> shown;
    float total_wanted = 0;
    float total_stretch = 0;
    for (auto const& child : view.children()) {
        if (child->is_hidden())
            continue;
        shown.append(child.ptr());
        auto size = wanted(*child);
        total_wanted += horizontal ? max(size.width, 0.0f) : max(size.height, 0.0f);
        total_stretch += horizontal ? child->horizontal_stretch() : child->vertical_stretch();
    }
    if (shown.is_empty())
        return;
    float spacing_total = m_spacing * static_cast<float>(shown.size() - 1);
    float left_over = main_room - total_wanted - spacing_total;

    float spacing = m_spacing;
    if (m_distribution == Distribution::EqualSpacing && shown.size() > 1 && left_over > 0)
        spacing += left_over / static_cast<float>(shown.size() - 1);

    float equal_size = (main_room - spacing_total) / static_cast<float>(shown.size());

    // For lining up on text: how far the deepest baseline sits from the row's top.
    float tallest_baseline = 0;
    if (m_alignment == Alignment::Baseline) {
        for (auto* child : shown)
            tallest_baseline = max(tallest_baseline, child->baseline());
    }

    float at = main_start;
    for (auto* child : shown) {
        auto size = wanted(*child);
        float along = horizontal ? max(size.width, 0.0f) : max(size.height, 0.0f);
        if (m_distribution == Distribution::Equal) {
            along = equal_size;
        } else if (m_distribution == Distribution::Natural) {
            float share = horizontal ? child->horizontal_stretch() : child->vertical_stretch();
            if (total_stretch > 0 && share > 0)
                along = max(along + left_over * share / total_stretch, 0.0f);
            else if (total_stretch <= 0 && left_over < 0)
                along = max(along + left_over * along / max(total_wanted, 1.0f), 0.0f);
        }
        float across = horizontal ? size.height : size.width;
        float cross_at = cross_start;
        switch (m_alignment) {
        case Alignment::Leading:
            break;
        case Alignment::Center:
            cross_at += (cross_room - max(across, 0.0f)) / 2;
            break;
        case Alignment::Trailing:
            cross_at += cross_room - max(across, 0.0f);
            break;
        case Alignment::Fill:
            across = cross_room;
            break;
        case Alignment::Baseline:
            if (child->baseline() >= 0)
                cross_at += tallest_baseline - child->baseline();
            else
                cross_at += (cross_room - max(across, 0.0f)) / 2;
            break;
        }
        if (across < 0)
            across = cross_room;
        float at_rounded = round_point(at);
        float along_rounded = round_point(at + along) - at_rounded;
        if (horizontal)
            child->set_frame({ at_rounded, round_point(cross_at), along_rounded, across });
        else
            child->set_frame({ round_point(cross_at), at_rounded, across, along_rounded });
        at += along + spacing;
    }
}

FloatSize StackLayout::preferred_size(View const& view) const
{
    bool horizontal = m_axis == Axis::Horizontal;
    float along = 0;
    float across = 0;
    size_t shown = 0;
    float largest_along = 0;
    float above = 0, below = 0;
    for (auto const& child : view.children()) {
        if (child->is_hidden())
            continue;
        ++shown;
        auto size = wanted(*child);
        float child_along = max(horizontal ? size.width : size.height, 0.0f);
        float child_across = max(horizontal ? size.height : size.width, 0.0f);
        along += child_along;
        largest_along = max(largest_along, child_along);
        if (m_alignment == Alignment::Baseline && child->baseline() >= 0) {
            above = max(above, child->baseline());
            below = max(below, child_across - child->baseline());
        }
        across = max(across, child_across);
    }
    if (m_alignment == Alignment::Baseline)
        across = max(across, above + below);
    if (m_distribution == Distribution::Equal)
        along = largest_along * static_cast<float>(shown);
    if (shown > 1)
        along += m_spacing * static_cast<float>(shown - 1);
    if (horizontal)
        return { along + m_insets.left + m_insets.right, across + m_insets.top + m_insets.bottom };
    return { across + m_insets.left + m_insets.right, along + m_insets.top + m_insets.bottom };
}

GridLayout::Sizes GridLayout::measure(View const& view) const
{
    Sizes sizes;
    for (size_t i = 0; i < m_columns; ++i)
        sizes.columns.append(0.0f);
    size_t index = 0;
    for (auto const& child : view.children()) {
        if (child->is_hidden())
            continue;
        size_t column = index % m_columns;
        size_t row = index / m_columns;
        if (row >= sizes.rows.size())
            sizes.rows.append(0.0f);
        auto size = wanted(*child);
        sizes.columns[column] = max(sizes.columns[column], max(size.width, 0.0f));
        sizes.rows[row] = max(sizes.rows[row], max(size.height, 0.0f));
        ++index;
    }
    return sizes;
}

void GridLayout::apply(View& view)
{
    auto sizes = measure(view);
    if (sizes.rows.is_empty())
        return;
    float used = m_insets.left + m_insets.right + m_column_spacing * static_cast<float>(m_columns - 1);
    for (size_t i = 0; i + 1 < m_columns; ++i)
        used += sizes.columns[i];
    sizes.columns[m_columns - 1] = max(view.width() - used, sizes.columns[m_columns - 1]);

    float y = m_insets.top;
    size_t index = 0;
    size_t row = 0;
    float x = m_insets.left;
    for (auto const& child : view.children()) {
        if (child->is_hidden())
            continue;
        size_t column = index % m_columns;
        if (column == 0 && index > 0) {
            y += sizes.rows[row] + m_row_spacing;
            ++row;
            x = m_insets.left;
        }
        auto size = wanted(*child);
        float cell_width = sizes.columns[column];
        float child_width = child->horizontal_stretch() > 0 || size.width < 0 ? cell_width : min(size.width, cell_width);
        float child_height = max(size.height, 0.0f);
        float child_x = x;
        if (column == 0 && m_first_column_trailing)
            child_x = x + cell_width - child_width;
        // Lined up on the middle of the row.
        float child_y = y + (sizes.rows[row] - child_height) / 2;
        child->set_frame({ round_point(child_x), round_point(child_y), round_point(child_width), child_height });
        x += cell_width + m_column_spacing;
        ++index;
    }
}

FloatSize GridLayout::preferred_size(View const& view) const
{
    auto sizes = measure(view);
    float width = m_insets.left + m_insets.right;
    for (auto column : sizes.columns)
        width += column;
    if (m_columns > 1)
        width += m_column_spacing * static_cast<float>(m_columns - 1);
    float height = m_insets.top + m_insets.bottom;
    for (auto row : sizes.rows)
        height += row;
    if (sizes.rows.size() > 1)
        height += m_row_spacing * static_cast<float>(sizes.rows.size() - 1);
    return { width, height };
}

void ConstraintLayout::add(Anchor first, ConstraintSolver::Relation relation, Anchor second, double strength)
{
    m_rules.append(Rule { first, relation, second, false, strength });
}

void ConstraintLayout::constant(View& view, Attribute attribute, double value, double strength)
{
    m_rules.append(Rule { Anchor { &view, attribute, 1, 0 }, ConstraintSolver::Relation::Equal, Anchor { nullptr, attribute, 0, value }, true, strength });
}

void ConstraintLayout::apply(View& parent)
{
    // Solved afresh each time: layouts are rare, and small enough that it costs less
    // than remembering what changed.
    ConstraintSolver solver;
    using Term = ConstraintSolver::Term;
    Vector<Variables> variables;
    for (auto const& child : parent.children()) {
        if (child->is_hidden())
            continue;
        variables.append(Variables { child.ptr(), solver.new_variable(), solver.new_variable(), solver.new_variable(), solver.new_variable() });
    }

    auto variables_of = [&](View* view) -> Variables const* {
        for (auto const& entry : variables) {
            if (entry.view == view)
                return &entry;
        }
        return nullptr;
    };

    // terms for `anchor`, added with this sign; what is constant goes in `constant`.
    auto add_terms = [&](Anchor const& anchor, double sign, Vector<Term>& terms, double& constant) {
        constant += sign * anchor.constant;
        if (anchor.factor == 0)
            return;
        double factor = sign * anchor.factor;
        if (!anchor.view || anchor.view == &parent) {
            switch (anchor.attribute) {
            case Attribute::Left:
            case Attribute::Top:
                break;
            case Attribute::Right:
            case Attribute::Width:
                constant += factor * static_cast<double>(parent.width());
                break;
            case Attribute::Bottom:
            case Attribute::Height:
                constant += factor * static_cast<double>(parent.height());
                break;
            case Attribute::CenterX:
                constant += factor * static_cast<double>(parent.width()) / 2;
                break;
            case Attribute::CenterY:
                constant += factor * static_cast<double>(parent.height()) / 2;
                break;
            }
            return;
        }
        auto const* entry = variables_of(anchor.view);
        if (!entry)
            return;
        switch (anchor.attribute) {
        case Attribute::Left:
            terms.append(Term { entry->left, factor });
            break;
        case Attribute::Top:
            terms.append(Term { entry->top, factor });
            break;
        case Attribute::Right:
            terms.append(Term { entry->left, factor });
            terms.append(Term { entry->width, factor });
            break;
        case Attribute::Bottom:
            terms.append(Term { entry->top, factor });
            terms.append(Term { entry->height, factor });
            break;
        case Attribute::Width:
            terms.append(Term { entry->width, factor });
            break;
        case Attribute::Height:
            terms.append(Term { entry->height, factor });
            break;
        case Attribute::CenterX:
            terms.append(Term { entry->left, factor });
            terms.append(Term { entry->width, factor / 2 });
            break;
        case Attribute::CenterY:
            terms.append(Term { entry->top, factor });
            terms.append(Term { entry->height, factor / 2 });
            break;
        }
    };

    for (auto const& entry : variables) {
        // Never negative, and the preferred size unless something says otherwise.
        Term width_term[1] = { { entry.width, 1 } };
        Term height_term[1] = { { entry.height, 1 } };
        (void)solver.add_constraint(Span<Term const>(width_term, 1), 0, ConstraintSolver::Relation::GreaterOrEqual);
        (void)solver.add_constraint(Span<Term const>(height_term, 1), 0, ConstraintSolver::Relation::GreaterOrEqual);
        auto preferred = wanted(*entry.view);
        if (preferred.width >= 0)
            (void)solver.add_constraint(Span<Term const>(width_term, 1), -static_cast<double>(preferred.width), ConstraintSolver::Relation::Equal, ConstraintSolver::medium);
        if (preferred.height >= 0)
            (void)solver.add_constraint(Span<Term const>(height_term, 1), -static_cast<double>(preferred.height), ConstraintSolver::Relation::Equal, ConstraintSolver::medium);
    }

    for (auto const& rule : m_rules) {
        Vector<Term> terms;
        double constant = 0;
        add_terms(rule.first, 1, terms, constant);
        add_terms(rule.second, -1, terms, constant);
        if (terms.is_empty())
            continue;
        (void)solver.add_constraint(terms.span(), constant, rule.relation, rule.strength);
    }

    for (auto const& entry : variables) {
        entry.view->set_frame({ round_point(static_cast<float>(solver.value_of(entry.left))), round_point(static_cast<float>(solver.value_of(entry.top))),
            round_point(static_cast<float>(solver.value_of(entry.width))), round_point(static_cast<float>(solver.value_of(entry.height))) });
    }
}

FloatSize ConstraintLayout::preferred_size(View const& view) const
{
    // The extent of the children as they stand: it is what the rules decide.
    FloatSize size { 0, 0 };
    for (auto const& child : view.children()) {
        if (child->is_hidden())
            continue;
        size.width = max(size.width, child->frame().right());
        size.height = max(size.height, child->frame().bottom());
    }
    return size;
}

}
