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
#include <LibUI/TableView.h>
#include <LibUI/TextField.h>

// A new computer's first questions, as a Mac's Setup Assistant asks them, on a card over the blurred wallpaper: welcome,
// where it is, the first account (an administrator), and light or dark. Then that person's logged in. Each step says
// "setup: <step>" on the console, for tests.
namespace LoginWindow {

class Setup final : public UI::View {
public:
    // The account made: its short name, to log in.
    explicit Setup(Function<void(StringView name)> on_done);
    ~Setup() override;

    // Where the keyboard goes at each step.
    [[nodiscard]] UI::View* first_responder();
    // Continue, as Return does.
    void go_forward();
    void go_back();

    void layout() override;
    void draw(UI::Graphics&) override;

private:
    enum class Step {
        Welcome,
        Region,
        Account,
        Look,
    };
    class Card;
    class LookChoice;
    class Regions;

    void show_step(Step);
    [[nodiscard]] bool account_is_ready();
    void finish();
    [[nodiscard]] Gfx::FloatRect card_rect() const;

    Function<void(StringView)> m_on_done;
    Step m_step { Step::Welcome };
    Card* m_card { nullptr };
    UI::View* m_pages[4] {};
    UI::Button* m_back { nullptr };
    UI::Button* m_continue { nullptr };
    UI::TableView* m_region_table { nullptr };
    OwnPtr<Regions> m_regions;
    UI::TextField* m_full_name { nullptr };
    UI::TextField* m_account_name { nullptr };
    UI::TextField* m_password { nullptr };
    UI::TextField* m_verify { nullptr };
    UI::TextField* m_hint { nullptr };
    UI::Label* m_problem { nullptr };
    bool m_name_edited { false };
    bool m_filling_name { false };
    Vector<LookChoice*> m_looks;
    bool m_dark { false };
};

}
