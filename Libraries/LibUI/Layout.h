/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Vector.h>
#include <LibGfx/Geometry.h>
#include <LibUI/ConstraintSolver.h>
#include <LibUI/View.h>

namespace UI {

class Layout {
public:
    virtual ~Layout() = default;
    virtual void apply(View&) = 0;
    [[nodiscard]] virtual Gfx::FloatSize preferred_size(View const&) const = 0;
};

enum class Axis : u8 {
    Horizontal,
    Vertical,
};

enum class Alignment : u8 {
    // Across the stack: at the start, centered, at the end, or across it all.
    Leading,
    Center,
    Trailing,
    Fill,
    // On text: in a row, so that labels and controls read as one line.
    Baseline,
};

enum class Distribution : u8 {
    // Each takes what it prefers, and those that stretch share the rest.
    Natural,
    // All the same size along the stack.
    Equal,
    // Room left goes between them.
    EqualSpacing,
};

class StackLayout final : public Layout {
public:
    explicit StackLayout(Axis axis, float spacing = 0, Alignment alignment = Alignment::Fill, Insets insets = {})
        : m_axis(axis)
        , m_spacing(spacing)
        , m_alignment(alignment)
        , m_insets(insets)
    {
    }

    void set_distribution(Distribution distribution) { m_distribution = distribution; }
    void set_alignment(Alignment alignment) { m_alignment = alignment; }
    void set_insets(Insets insets) { m_insets = insets; }
    void set_spacing(float spacing) { m_spacing = spacing; }

    void apply(View&) override;
    [[nodiscard]] Gfx::FloatSize preferred_size(View const&) const override;

private:
    Axis m_axis;
    float m_spacing;
    Alignment m_alignment;
    Distribution m_distribution { Distribution::Natural };
    Insets m_insets;
};

// Rows and columns: each column as wide as its widest, each row as tall as its
// tallest; children in reading order. The last column takes any room left, as forms do.
class GridLayout final : public Layout {
public:
    explicit GridLayout(size_t columns, float column_spacing = 8, float row_spacing = 8, Insets insets = {})
        : m_columns(columns)
        , m_column_spacing(column_spacing)
        , m_row_spacing(row_spacing)
        , m_insets(insets)
    {
    }

    // The first column's text lines up at the right, as labels in a form do.
    void set_first_column_trailing(bool trailing) { m_first_column_trailing = trailing; }

    void apply(View&) override;
    [[nodiscard]] Gfx::FloatSize preferred_size(View const&) const override;

private:
    struct Sizes {
        Vector<float> columns;
        Vector<float> rows;
    };
    [[nodiscard]] Sizes measure(View const&) const;

    size_t m_columns;
    float m_column_spacing;
    float m_row_spacing;
    Insets m_insets;
    bool m_first_column_trailing { false };
};

// Anything that can be said as a linear equation between views' edges.
enum class Attribute : u8 {
    Left,
    Right,
    Top,
    Bottom,
    Width,
    Height,
    CenterX,
    CenterY,
};

// A side of a view (or of the view being laid out: pass nullptr), times a factor,
// plus a constant.
struct Anchor {
    View* view { nullptr };
    Attribute attribute { Attribute::Left };
    double factor { 1 };
    double constant { 0 };

    [[nodiscard]] Anchor operator+(double more) const { return { view, attribute, factor, constant + more }; }
    [[nodiscard]] Anchor operator-(double less) const { return { view, attribute, factor, constant - less }; }
    [[nodiscard]] Anchor operator*(double by) const { return { view, attribute, factor * by, constant * by }; }
};

// Each child has a position and a size, the solver's to find: by default it has its
// preferred size at less than required strength, so other constraints can bend it.
class ConstraintLayout final : public Layout {
public:
    ConstraintLayout() = default;

    // first RELATION second.
    void add(Anchor first, ConstraintSolver::Relation, Anchor second, double strength = ConstraintSolver::required);
    void equal(Anchor first, Anchor second, double strength = ConstraintSolver::required) { add(first, ConstraintSolver::Relation::Equal, second, strength); }
    void at_least(Anchor first, Anchor second, double strength = ConstraintSolver::required) { add(first, ConstraintSolver::Relation::GreaterOrEqual, second, strength); }
    void at_most(Anchor first, Anchor second, double strength = ConstraintSolver::required) { add(first, ConstraintSolver::Relation::LessOrEqual, second, strength); }
    // A size that needs no other view.
    void constant(View& view, Attribute attribute, double value, double strength = ConstraintSolver::required);

    void apply(View&) override;
    [[nodiscard]] Gfx::FloatSize preferred_size(View const&) const override;

private:
    struct Rule {
        Anchor first;
        ConstraintSolver::Relation relation;
        Anchor second;
        bool second_is_constant { false };
        double strength;
    };
    struct Variables {
        View* view;
        ConstraintSolver::Variable left, top, width, height;
    };

    Vector<Rule> m_rules;
};

// A view that only arranges others: a VStack or an HStack, made in a line.
class StackView : public View {
public:
    explicit StackView(Axis axis, float spacing = 0, Alignment alignment = Alignment::Fill, Insets insets = {})
    {
        m_stack = &View::set_layout<StackLayout>(axis, spacing, alignment, insets);
    }

    [[nodiscard]] StackLayout& stack() { return *m_stack; }

private:
    StackLayout* m_stack { nullptr };
};

}
