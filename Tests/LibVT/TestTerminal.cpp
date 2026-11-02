/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibTest/TestCase.h>
#include <LibVT/Terminal.h>

using VT::Terminal;

namespace {

bool row_is(Terminal const& terminal, int row, StringView text)
{
    return terminal.row_text(row).view() == text;
}

}

TEST_CASE(text_goes_on_the_screen_and_wraps)
{
    Terminal t(10, 3);
    t.feed("hello"_sv);
    EXPECT(row_is(t, 0, "hello"_sv));
    EXPECT_EQ(t.cursor_column(), 5);
    t.feed(" world!"_sv);
    EXPECT(row_is(t, 0, "hello worl"_sv));
    EXPECT(row_is(t, 1, "d!"_sv));
    EXPECT_EQ(t.cursor_row(), 1);
    EXPECT_EQ(t.cursor_column(), 2);
}

TEST_CASE(the_last_column_waits_to_wrap)
{
    Terminal t(5, 2);
    t.feed("abcde"_sv);
    EXPECT_EQ(t.cursor_row(), 0);
    EXPECT_EQ(t.cursor_column(), 4);
    t.feed("\r\n"_sv);
    EXPECT_EQ(t.cursor_row(), 1);
    EXPECT_EQ(t.cursor_column(), 0);
    EXPECT(row_is(t, 1, ""_sv));
}

TEST_CASE(controls_move_the_cursor)
{
    Terminal t(20, 3);
    t.feed("abc\b\bX"_sv);
    EXPECT(row_is(t, 0, "aXc"_sv));
    t.feed("\rY"_sv);
    EXPECT(row_is(t, 0, "YXc"_sv));
    t.feed("\t"_sv);
    EXPECT_EQ(t.cursor_column(), 8);
    int bells = 0;
    t.on_bell = [&] { ++bells; };
    t.feed("\a"_sv);
    EXPECT_EQ(bells, 1);
}

TEST_CASE(csi_moves_and_erases)
{
    Terminal t(10, 4);
    t.feed("\x1b[2;3Hx"_sv);
    EXPECT_EQ(t.cell(1, 2).codepoint, static_cast<u32>('x'));
    t.feed("\x1b[A\x1b[DZ"_sv);
    EXPECT_EQ(t.cell(0, 2).codepoint, static_cast<u32>('Z'));
    t.feed("\x1b[H0123456789"_sv);
    t.feed("\x1b[1;5H\x1b[K"_sv);
    EXPECT(row_is(t, 0, "0123"_sv));
    t.feed("\x1b[1;1H\x1b[1K"_sv);
    EXPECT(row_is(t, 0, " 123"_sv));
    t.feed("\x1b[2J"_sv);
    EXPECT(row_is(t, 0, ""_sv));
}

TEST_CASE(lines_scroll_into_the_scrollback)
{
    Terminal t(10, 2);
    t.feed("one\r\ntwo\r\nthree"_sv);
    EXPECT(row_is(t, 0, "two"_sv));
    EXPECT(row_is(t, 1, "three"_sv));
    EXPECT_EQ(t.scrollback_size(), static_cast<size_t>(1));
    EXPECT_EQ(t.scrollback_line(0)[0].codepoint, static_cast<u32>('o'));
}

TEST_CASE(scroll_regions_keep_what_is_outside_them)
{
    Terminal t(10, 4);
    t.feed("top\x1b[4;1Hbottom"_sv);
    t.feed("\x1b[2;3r"_sv);
    t.feed("\x1b[2;1Ha\r\nb\r\nc"_sv);
    EXPECT(row_is(t, 0, "top"_sv));
    EXPECT(row_is(t, 1, "b"_sv));
    EXPECT(row_is(t, 2, "c"_sv));
    EXPECT(row_is(t, 3, "bottom"_sv));
    EXPECT_EQ(t.scrollback_size(), static_cast<size_t>(0));
}

TEST_CASE(colors_of_all_kinds)
{
    Terminal t(10, 2);
    t.feed("\x1b[1;31mA\x1b[0m"_sv);
    auto const& red = t.cell(0, 0).attributes;
    EXPECT(red.bold);
    EXPECT_EQ(red.foreground.index, static_cast<u8>(1));
    EXPECT(t.cell(0, 0).attributes.foreground.kind == VT::Color::Kind::Indexed);
    t.feed("\x1b[38;5;200mB"_sv);
    EXPECT_EQ(t.cell(0, 1).attributes.foreground.index, static_cast<u8>(200));
    t.feed("\x1b[48;2;1;2;3mC"_sv);
    EXPECT(t.cell(0, 2).attributes.background.kind == VT::Color::Kind::RGB);
    EXPECT_EQ(t.cell(0, 2).attributes.background.blue, static_cast<u8>(3));
    t.feed("\x1b[0;92mD"_sv);
    EXPECT_EQ(t.cell(0, 3).attributes.foreground.index, static_cast<u8>(10));
    t.feed("\x1b[7;4mE\x1b[27;24m"_sv);
    EXPECT(t.cell(0, 4).attributes.inverse);
    EXPECT(t.cell(0, 4).attributes.underline);
}

TEST_CASE(utf8_and_bad_sequences)
{
    Terminal t(10, 2);
    t.feed("é€😀"_sv);
    EXPECT_EQ(t.cell(0, 0).codepoint, static_cast<u32>(0xE9));
    EXPECT_EQ(t.cell(0, 1).codepoint, static_cast<u32>(0x20AC));
    EXPECT_EQ(t.cell(0, 2).codepoint, static_cast<u32>(0x1F600));
    t.feed("\xC3" "a"_sv);
    EXPECT_EQ(t.cell(0, 3).codepoint, static_cast<u32>(0xFFFD));
    EXPECT_EQ(t.cell(0, 4).codepoint, static_cast<u32>('a'));
    t.feed("\xFF"_sv);
    EXPECT_EQ(t.cell(0, 5).codepoint, static_cast<u32>(0xFFFD));
}

TEST_CASE(a_sequence_can_come_in_pieces)
{
    Terminal t(10, 2);
    t.feed("\x1b"_sv);
    t.feed("["_sv);
    t.feed("3"_sv);
    t.feed("1"_sv);
    t.feed("mX"_sv);
    EXPECT_EQ(t.cell(0, 0).attributes.foreground.index, static_cast<u8>(1));
    EXPECT_EQ(t.cell(0, 0).codepoint, static_cast<u32>('X'));
}

TEST_CASE(the_alternate_screen_leaves_the_main_one_alone)
{
    Terminal t(10, 3);
    t.feed("main\x1b[?1049h"_sv);
    EXPECT(t.using_alternate_screen());
    EXPECT(row_is(t, 0, ""_sv));
    t.feed("other"_sv);
    t.feed("\x1b[?1049l"_sv);
    EXPECT(!t.using_alternate_screen());
    EXPECT(row_is(t, 0, "main"_sv));
    EXPECT_EQ(t.cursor_column(), 4);
}

TEST_CASE(insert_and_delete)
{
    Terminal t(6, 3);
    t.feed("abcdef\x1b[1;2H\x1b[2P"_sv);
    EXPECT(row_is(t, 0, "adef"_sv));
    t.feed("\x1b[1;2H\x1b[2@"_sv);
    EXPECT(row_is(t, 0, "a  def"_sv));
    t.feed("\x1b[2;1Hsecond\x1b[1;1H\x1b[L"_sv);
    EXPECT(row_is(t, 0, ""_sv));
    EXPECT(row_is(t, 1, "a  def"_sv));
    EXPECT(row_is(t, 2, "second"_sv));
    t.feed("\x1b[1;1H\x1b[M"_sv);
    EXPECT(row_is(t, 0, "a  def"_sv));
    t.feed("\x1b[1;2H\x1b[2X"_sv);
    EXPECT(row_is(t, 0, "a  def"_sv));
}

TEST_CASE(it_answers_what_is_asked)
{
    Terminal t(10, 4);
    String reply;
    t.on_reply = [&](StringView text) { reply = MUST(String::from_view(text)); };
    t.feed("\x1b[2;3H\x1b[6n"_sv);
    EXPECT(reply.view() == "\x1b[2;3R"_sv);
    t.feed("\x1b[c"_sv);
    EXPECT(reply.view() == "\x1b[?1;2c"_sv);
}

TEST_CASE(the_title_comes_by_osc)
{
    Terminal t(10, 2);
    t.feed("\x1b]0;my title\x07"_sv);
    EXPECT(t.title() == "my title"_sv);
    t.feed("\x1b]2;other\x1b\\"_sv);
    EXPECT(t.title() == "other"_sv);
}

TEST_CASE(modes_the_program_asks_for)
{
    Terminal t(10, 2);
    t.feed("\x1b[?1h\x1b[?25l\x1b[?2004h"_sv);
    EXPECT(t.application_cursor_keys());
    EXPECT(!t.cursor_visible());
    EXPECT(t.bracketed_paste());
    t.feed("\x1b[?1l\x1b[?25h\x1b[?2004l"_sv);
    EXPECT(!t.application_cursor_keys());
    EXPECT(t.cursor_visible());
    t.feed("\x1b[?7labcdefghijklmn"_sv);
    EXPECT_EQ(t.cursor_row(), 0);
    EXPECT_EQ(t.cell(0, 9).codepoint, static_cast<u32>('n'));
}

TEST_CASE(resizing_keeps_the_text_and_the_cursor_on_screen)
{
    Terminal t(10, 4);
    t.feed("one\r\ntwo\r\nthree\r\nfour"_sv);
    t.resize(5, 2);
    EXPECT_EQ(t.rows(), 2);
    EXPECT_EQ(t.columns(), 5);
    EXPECT(t.cursor_row() < 2);
    EXPECT(row_is(t, 1, "four"_sv));
    EXPECT(t.scrollback_size() >= 1);
    t.resize(20, 6);
    EXPECT(row_is(t, 1, "four"_sv));
    t.feed("!"_sv);
    EXPECT(row_is(t, 1, "four!"_sv));
}

TEST_CASE(junk_never_breaks_it)
{
    Terminal t(8, 3);
    for (int i = 0; i < 10000; ++i) {
        u8 byte = static_cast<u8>(i * 73 + (i >> 3));
        t.feed(ReadonlyBytes(&byte, 1));
    }
    t.feed("\x1b[99999;99999H\x1b[99999A\x1b[99999B\x1b[99999C\x1b[99999D\x1b[99999L\x1b[99999M\x1b[99999P\x1b[99999@\x1b[99999X\x1b[99999S\x1b[99999T"_sv);
    t.feed("ok"_sv);
    EXPECT(t.cursor_row() < 3);
    EXPECT(t.cursor_column() < 8);
}
