/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Function.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibUI/Controls.h>
#include <LibUI/ScrollView.h>

namespace UI {

// What a table shows. Rows are asked for as they come into sight.
class TableModel {
public:
    virtual ~TableModel() = default;
    [[nodiscard]] virtual size_t row_count() const = 0;
    [[nodiscard]] virtual String text_at(size_t row, size_t column) const = 0;
    // An icon in the first column, if any.
    [[nodiscard]] virtual bool has_symbol_at(size_t) const { return false; }
    [[nodiscard]] virtual Symbol symbol_at(size_t) const { return Symbol::File; }
    // Numbers line up by their right ends.
    [[nodiscard]] virtual bool is_right_aligned(size_t column) const
    {
        (void)column;
        return false;
    }
    // By a column, ascending or not: the model reorders itself.
    virtual void sort_by(size_t column, bool ascending)
    {
        (void)column;
        (void)ascending;
    }
};

class TableHeaderView final : public View {
public:
    TableHeaderView() = default;

    void add_column(StringView title, float width);
    [[nodiscard]] size_t column_count() const { return m_columns.size(); }
    [[nodiscard]] float column_width(size_t index) const { return m_columns[index].width; }
    [[nodiscard]] float column_x(size_t index) const;
    [[nodiscard]] float total_width() const;
    // The header scrolls with the rows, sideways.
    void set_scroll(float x);

    Function<void()> on_columns_changed;
    Function<void(size_t column, bool ascending)> on_sort;
    // Whether a column's title goes to the right, as its numbers do.
    Function<bool(size_t)> is_right_aligned;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    bool mouse_down(MouseEvent const&) override;
    void mouse_dragged(MouseEvent const&) override;
    void mouse_up(MouseEvent const&) override;
    [[nodiscard]] WS::CursorShape cursor() const override;
    void mouse_moved(MouseEvent const&) override;

private:
    struct Column {
        String title;
        float width;
    };

    [[nodiscard]] int divider_at(float x) const;

    Vector<Column> m_columns;
    float m_scroll { 0 };
    int m_resizing { -1 };
    float m_resize_start_x { 0 };
    float m_resize_start_width { 0 };
    int m_pressed { -1 };
    int m_sort_column { -1 };
    bool m_sort_ascending { true };
    bool m_over_divider { false };
};

class RowsView;

class TableView : public View {
public:
    explicit TableView(TableModel* = nullptr);
    ~TableView() override;

    void set_model(TableModel*);
    [[nodiscard]] TableModel* model() const { return m_model; }
    void add_column(StringView title, float width);
    void set_headers_visible(bool);
    void reload();
    void set_alternating_rows(bool);
    void set_allows_multiple_selection(bool allows) { m_multiple = allows; }

    // Selection.
    [[nodiscard]] Vector<size_t> const& selection() const { return m_selection; }
    [[nodiscard]] int selected_row() const { return m_selection.is_empty() ? -1 : static_cast<int>(m_selection.first()); }
    void select_row(size_t row, bool extend = false);
    void clear_selection();
    void scroll_to_row(size_t row);

    Function<void()> on_selection_change;
    Function<void(size_t row)> on_activate;

    // Drags: what a row's drag carries, if anything.
    Function<bool(size_t row, String& type, Vector<u8>& data)> on_drag_row;

    void layout() override;
    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override { return { -1, -1 }; }
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    bool key_down(KeyEvent const&) override;
    [[nodiscard]] float focus_ring_radius() const override { return 0; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    void did_become_first_responder() override { set_needs_display(); }
    void did_resign_first_responder() override { set_needs_display(); }
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Table; }

    // How much a row's first column sits in (an outline's depth).
    [[nodiscard]] virtual float indent_of(size_t) const { return 0; }
    virtual void draw_row_prefix(Graphics&, size_t, Gfx::FloatRect const&) { }
    virtual bool row_prefix_pressed(size_t, float) { return false; }

    [[nodiscard]] ScrollView& scroll_view() { return *m_scroll; }
    [[nodiscard]] TableHeaderView& header() { return *m_header; }

private:
    friend class RowsView;

    void row_pressed(size_t row, MouseEvent const&);
    void selection_changed();

    TableModel* m_model { nullptr };
    ScrollView* m_scroll { nullptr };
    TableHeaderView* m_header { nullptr };
    RowsView* m_rows { nullptr };
    Vector<size_t> m_selection;
    int m_anchor { -1 };
    bool m_multiple { false };
    bool m_alternating { false };
    bool m_headers_visible { true };
};

// A tree, shown as rows: what children an item has comes from this.
class OutlineModel {
public:
    virtual ~OutlineModel() = default;
    // Of `parent` (nullptr for the top level). Items are the model's own, and stay.
    [[nodiscard]] virtual size_t child_count(void* parent) const = 0;
    [[nodiscard]] virtual void* child_at(void* parent, size_t index) const = 0;
    [[nodiscard]] virtual String text_of(void* item, size_t column) const = 0;
    [[nodiscard]] virtual bool has_symbol(void*) const { return false; }
    [[nodiscard]] virtual Symbol symbol_of(void*) const { return Symbol::File; }
};

class OutlineView final : public TableView
    , private TableModel {
public:
    explicit OutlineView(OutlineModel&);

    void reload_items();
    void set_expanded(void* item, bool expanded);
    [[nodiscard]] bool is_expanded(void* item) const;
    [[nodiscard]] void* item_at_row(size_t row) const { return m_rows_flat[row].item; }

    [[nodiscard]] float indent_of(size_t) const override;
    void draw_row_prefix(Graphics&, size_t, Gfx::FloatRect const&) override;
    bool row_prefix_pressed(size_t, float) override;
    bool key_down(KeyEvent const&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Outline; }

private:
    struct Row {
        void* item;
        int depth;
    };

    void flatten_into(void* parent, int depth);

    [[nodiscard]] size_t row_count() const override { return m_rows_flat.size(); }
    [[nodiscard]] String text_at(size_t row, size_t column) const override { return m_outline.text_of(m_rows_flat[row].item, column); }
    [[nodiscard]] bool has_symbol_at(size_t row) const override { return m_outline.has_symbol(m_rows_flat[row].item); }
    [[nodiscard]] Symbol symbol_at(size_t row) const override { return m_outline.symbol_of(m_rows_flat[row].item); }

    OutlineModel& m_outline;
    Vector<Row> m_rows_flat;
    Vector<void*> m_expanded;
};

class CollectionModel {
public:
    virtual ~CollectionModel() = default;
    [[nodiscard]] virtual size_t item_count() const = 0;
    [[nodiscard]] virtual String title_of(size_t) const = 0;
    [[nodiscard]] virtual Symbol symbol_of(size_t) const { return Symbol::File; }
};

// Items in a grid that flows with its width.
class CollectionView final : public View {
public:
    explicit CollectionView(CollectionModel&);

    [[nodiscard]] int selected_item() const { return m_selected; }
    void select_item(int);

    Function<void(size_t)> on_activate;
    Function<void()> on_selection_change;

    [[nodiscard]] Gfx::FloatSize intrinsic_size() const override;
    void draw(Graphics&) override;
    bool mouse_down(MouseEvent const&) override;
    [[nodiscard]] bool accepts_first_responder() const override { return true; }
    [[nodiscard]] bool shows_focus_ring() const override { return false; }
    bool key_down(KeyEvent const&) override;
    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Table; }
    void accessibility_parts(Vector<Accessibility::Node>&) override;

private:
    [[nodiscard]] int columns() const;
    [[nodiscard]] Gfx::FloatRect item_rect(size_t) const;

    CollectionModel& m_model;
    int m_selected { -1 };
};

}
