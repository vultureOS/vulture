/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Setup.h"
#include "Views.h"
#include <LibCore/AccountDatabase.h>
#include <LibGfx/Path.h>
#include <LibLocale/Locale.h>
#include <LibUI/Appearance.h>
#include <LibUI/Application.h>
#include <LibUI/Layout.h>
#include <LibUI/Window.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace LoginWindow {

using namespace UI;
using namespace Gfx;

namespace {

void say(StringView what)
{
    printf("setup: %.*s\n", static_cast<int>(what.length()), what.characters_without_null_termination());
    fflush(stdout);
}

StringView region_name_of(Locale::LocaleData const& locale)
{
    auto const& english = Locale::default_locale();
    for (size_t i = 0; i < Locale::region_name_count; ++i) {
        if (StringView(Locale::region_codes[i]) == StringView(locale.region))
            return StringView(english.region_names[i]);
    }
    return StringView(locale.region);
}

// What heads a step: a picture (the system's mark, or a symbol), large, in the accent's colour.
class HeaderPicture final : public View {
public:
    HeaderPicture(bool is_mark, Symbol symbol, float size)
        : m_is_mark(is_mark)
        , m_symbol(symbol)
        , m_size(size)
    {
    }

    [[nodiscard]] FloatSize intrinsic_size() const override { return { m_size, m_size }; }

    void draw(Graphics& graphics) override
    {
        auto const color = Appearance::the().color("accent"_sv);
        float const left = (width() - m_size) / 2;
        if (!m_is_mark) {
            graphics.draw_symbol(m_symbol, { left, 0, m_size, m_size }, color, 1);
            return;
        }
        // A ring round a "V", as the boot screen has it.
        float const r = m_size / 2;
        FloatPoint const center { left + r, r };
        StrokeStyle ring;
        ring.width = r * 0.12f;
        Path circle;
        circle.add_circle(center, r - ring.width / 2);
        graphics.painter().stroke_path(circle, ring, color);
        StrokeStyle stroke;
        stroke.width = r * 0.17f;
        stroke.cap = LineCap::Round;
        stroke.join = LineJoin::Round;
        Path v;
        v.move_to({ center.x - r * 0.42f, center.y - r * 0.32f });
        v.line_to({ center.x, center.y + r * 0.4f });
        v.line_to({ center.x + r * 0.42f, center.y - r * 0.32f });
        graphics.painter().stroke_path(v, stroke, color);
    }

private:
    bool m_is_mark;
    Symbol m_symbol;
    float m_size;
};

// The title and what it means, under the picture.
void add_header(StackView& page, bool is_mark, Symbol symbol, StringView title, StringView subtitle)
{
    auto const& t = Appearance::the();
    (void)page.add<HeaderPicture>(is_mark, symbol, t.metric(is_mark ? "setup.markSize"_sv : "setup.symbolSize"_sv));
    auto& heading = page.add<Label>(title, FontSpec { t.metric(is_mark ? "setup.welcomeSize"_sv : "setup.titleSize"_sv), FontWeight::Bold });
    heading.set_alignment(TextAlignment::Center);
    if (!subtitle.is_empty()) {
        auto& explanation = page.add<Label>(subtitle);
        explanation.set_alignment(TextAlignment::Center);
        explanation.set_style(LabelStyle::Secondary);
        explanation.set_wraps(true);
        explanation.set_fixed_width(t.metric("setup.textWidth"_sv));
    }
}

TextField& add_field(StackView& form, StringView title, StringView placeholder, bool secure)
{
    auto const& t = Appearance::the();
    auto& row = form.add<StackView>(Axis::Horizontal, 10, Alignment::Center);
    auto& label = row.add<Label>(title);
    label.set_alignment(TextAlignment::Right);
    label.set_fixed_width(t.metric("setup.labelWidth"_sv));
    auto& field = row.add<TextField>();
    field.set_fixed_width(t.metric("setup.fieldWidth"_sv));
    field.set_placeholder(placeholder);
    field.set_secure(secure);
    field.set_accessibility_label(title);
    return field;
}

void write_owned(StringView path, StringView text, uid_t uid)
{
    auto const name = MUST(String::from_view(path));
    int fd = open(name.characters(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    (void)write(fd, text.characters_without_null_termination(), text.length());
    (void)fchown(fd, uid, Core::AccountDatabase::staff_gid);
    close(fd);
}

}

class Setup::Regions final : public TableModel {
public:
    struct Entry {
        StringView name;
        StringView identifier;
    };

    Regions()
    {
        for (size_t i = 0; i < Locale::locale_count; ++i) {
            Entry entry { region_name_of(Locale::locales[i]), StringView(Locale::locales[i].identifier) };
            // In the order of their names, as a Mac lists them.
            size_t at = m_entries.size();
            while (at > 0 && compare(entry.name, m_entries[at - 1].name) < 0)
                --at;
            m_entries.insert(at, entry);
        }
    }

    [[nodiscard]] size_t row_count() const override { return m_entries.size(); }
    [[nodiscard]] String text_at(size_t row, size_t) const override { return MUST(String::from_view(m_entries[row].name)); }
    [[nodiscard]] Entry const& at(size_t row) const { return m_entries[row]; }

    [[nodiscard]] size_t row_of(StringView identifier) const
    {
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].identifier == identifier)
                return i;
        }
        return 0;
    }

private:
    static int compare(StringView a, StringView b)
    {
        size_t const shorter = min(a.length(), b.length());
        for (size_t i = 0; i < shorter; ++i) {
            if (a[i] != b[i])
                return static_cast<u8>(a[i]) < static_cast<u8>(b[i]) ? -1 : 1;
        }
        return a.length() == b.length() ? 0 : (a.length() < b.length() ? -1 : 1);
    }

    Vector<Entry> m_entries;
};

// What everything's on.
class Setup::Card final : public View {
public:
    void draw(Graphics& graphics) override
    {
        auto const& t = Appearance::the();
        graphics.fill_continuous({ 0, 0, width(), height() }, t.metric("setup.radius"_sv), t.color("windowBackground"_sv));
        graphics.stroke_rounded({ 0.5f, 0.5f, width() - 1, height() - 1 }, t.metric("setup.radius"_sv), 1, t.color("separator"_sv));
    }
};

// Light or Dark: a small picture of each, its name under it, and a ring round the one chosen.
class Setup::LookChoice final : public Control {
public:
    LookChoice(StringView title, bool dark, bool chosen)
        : m_title(MUST(String::from_view(title)))
        , m_dark(dark)
        , m_chosen(chosen)
    {
    }

    void set_chosen(bool chosen)
    {
        m_chosen = chosen;
        set_needs_display();
    }
    [[nodiscard]] bool is_dark() const { return m_dark; }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        auto const& t = Appearance::the();
        return { t.metric("setup.lookWidth"_sv), t.metric("setup.lookHeight"_sv) + 26 };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = Appearance::the();
        float const w = t.metric("setup.lookWidth"_sv);
        float const h = t.metric("setup.lookHeight"_sv);
        FloatRect const picture { 0, 0, w, h };
        graphics.fill_continuous(picture, 8, Color::from_rgb(m_dark ? 0x1c1c1e : 0xdcdce1));
        // A window on the desktop, with its title bar and a sidebar.
        FloatRect const window { w * 0.16f, h * 0.2f, w * 0.68f, h * 0.62f };
        graphics.fill_continuous(window, 4, Color::from_rgb(m_dark ? 0x323234 : 0xffffff));
        graphics.fill_continuous({ window.x, window.y, window.width * 0.3f, window.height }, 4, Color::from_rgb(m_dark ? 0x3a3a3c : 0xececec));
        Color const lights[] = { Color::from_rgb(0xff5f57), Color::from_rgb(0xfebc2e), Color::from_rgb(0x28c840) };
        for (int i = 0; i < 3; ++i)
            graphics.fill_circle({ window.x + 6 + static_cast<float>(i) * 6, window.y + 6 }, 2, lights[i]);
        graphics.fill_rounded({ window.x + window.width * 0.4f, window.y + window.height * 0.35f, window.width * 0.5f, 5 }, 2.5f, t.color("accent"_sv));
        if (m_chosen)
            graphics.stroke_rounded({ -2, -2, w + 4, h + 4 }, 10, 3, t.color("accent"_sv));
        graphics.draw_text_centered(m_title.view(), FontSpec { 13, m_chosen ? FontWeight::SemiBold : FontWeight::Regular }, { 0, h + 6, w, 18 }, t.color("label"_sv));
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::RadioButton; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_title.view())); }
    [[nodiscard]] String accessibility_value() const override { return MUST(String::from_view(m_chosen ? "on"_sv : "off"_sv)); }

private:
    String m_title;
    bool m_dark;
    bool m_chosen;
};

Setup::Setup(Function<void(StringView)> on_done)
    : m_on_done(move(on_done))
{
    auto const& t = Appearance::the();
    m_dark = t.is_dark();
    m_card = &add<Card>();
    float const spacing = t.metric("setup.spacing"_sv);

    // Welcome.
    auto& welcome = m_card->add<StackView>(Axis::Vertical, spacing, Alignment::Center);
    add_header(welcome, true, Symbol::Globe, "Welcome"_sv, "Let\xE2\x80\x99s set up your computer. It takes a few steps, and you can change everything later in Settings."_sv);
    m_pages[0] = &welcome;

    // Where it is: how dates and numbers are written.
    auto& region = m_card->add<StackView>(Axis::Vertical, spacing, Alignment::Center);
    add_header(region, false, Symbol::Globe, "Select Your Country or Region"_sv, {});
    m_regions = adopt_own(*new Regions);
    m_region_table = &region.add<TableView>(m_regions.ptr());
    // Narrower than the list, by its scroller: nothing to scroll sideways to.
    m_region_table->add_column("Country or Region"_sv, t.metric("setup.listWidth"_sv) - 24);
    m_region_table->set_headers_visible(false);
    m_region_table->set_fixed_size({ t.metric("setup.listWidth"_sv), t.metric("setup.listHeight"_sv) });
    m_region_table->set_accessibility_label("Countries and Regions"_sv);
    m_region_table->reload();
    m_region_table->select_row(m_regions->row_of("en_US"_sv));
    m_pages[1] = &region;

    // The first account: an administrator's.
    auto& account = m_card->add<StackView>(Axis::Vertical, spacing, Alignment::Center);
    add_header(account, false, Symbol::Person, "Create a Computer Account"_sv, "Fill out the following information to create your computer account."_sv);
    auto& form = account.add<StackView>(Axis::Vertical, 10, Alignment::Center);
    m_full_name = &add_field(form, "Full Name"_sv, {}, false);
    m_account_name = &add_field(form, "Account Name"_sv, {}, false);
    m_password = &add_field(form, "Password"_sv, "Required"_sv, true);
    m_verify = &add_field(form, "Verify"_sv, "Verify"_sv, true);
    m_hint = &add_field(form, "Hint"_sv, "Recommended"_sv, false);
    m_problem = &account.add<Label>(" "_sv);
    m_problem->set_color_token("systemRed"_sv);
    m_problem->set_alignment(TextAlignment::Center);
    // The account's name follows the full one, as it's typed, until it's changed by hand.
    m_full_name->on_change = [this] {
        if (m_name_edited)
            return;
        m_filling_name = true;
        m_account_name->set_text(Core::AccountDatabase::name_from_full_name(m_full_name->text()).view());
        m_filling_name = false;
    };
    m_account_name->on_change = [this] {
        if (!m_filling_name)
            m_name_edited = true;
    };
    m_pages[2] = &account;

    // Light or dark: shown as it's chosen.
    auto& look = m_card->add<StackView>(Axis::Vertical, spacing, Alignment::Center);
    add_header(look, false, Symbol::Contrast, "Choose Your Look"_sv, "Select an appearance. You can change this later in Settings."_sv);
    auto& choices = look.add<StackView>(Axis::Horizontal, t.metric("setup.lookSpacing"_sv), Alignment::Center);
    constexpr bool looks[] = { false, true };
    for (bool dark : looks) {
        auto& choice = choices.add<LookChoice>(dark ? "Dark"_sv : "Light"_sv, dark, dark == m_dark);
        m_looks.append(&choice);
        choice.on_action = [this, dark] {
            m_dark = dark;
            for (auto* each : m_looks)
                each->set_chosen(each->is_dark() == dark);
            (void)Application::the().connection().server().set_appearance(dark ? "Dark"_sv : "Light"_sv);
            say(dark ? "look Dark"_sv : "look Light"_sv);
        };
    }
    m_pages[3] = &look;

    m_back = &m_card->add<Button>("Back"_sv);
    m_back->on_action = [this] { go_back(); };
    m_continue = &m_card->add<Button>("Continue"_sv, ButtonStyle::Default);
    m_continue->on_action = [this] { go_forward(); };
    show_step(Step::Welcome);
}

Setup::~Setup() = default;

FloatRect Setup::card_rect() const
{
    auto const& t = Appearance::the();
    float const w = t.metric("setup.width"_sv);
    float const h = t.metric("setup.height"_sv);
    return { (width() - w) / 2, (height() - h) / 2, w, h };
}

void Setup::layout()
{
    auto const& t = Appearance::the();
    auto const card = card_rect();
    m_card->set_frame(card);
    float const padding = t.metric("setup.padding"_sv);
    auto const back = m_back->preferred_size();
    auto const next = m_continue->preferred_size();
    float const buttons_y = card.height - padding - next.height;
    m_back->set_frame({ padding, buttons_y, max(back.width, 90.0f), back.height });
    m_continue->set_frame({ card.width - padding - max(next.width, 90.0f), buttons_y, max(next.width, 90.0f), next.height });
    for (auto* page : m_pages) {
        auto const wanted = page->preferred_size();
        page->set_frame({ (card.width - wanted.width) / 2, t.metric("setup.top"_sv), wanted.width, wanted.height });
    }
}

void Setup::draw(Graphics& graphics)
{
    graphics.fill({ 0, 0, width(), height() }, Appearance::the().color("loginwindow.scrim"_sv));
}

View* Setup::first_responder()
{
    switch (m_step) {
    case Step::Region:
        return m_region_table;
    case Step::Account:
        return m_full_name;
    default:
        return m_continue;
    }
}

void Setup::show_step(Step step)
{
    m_step = step;
    for (size_t i = 0; i < 4; ++i)
        m_pages[i]->set_hidden(i != static_cast<size_t>(step));
    m_back->set_hidden(step == Step::Welcome);
    m_problem->set_text(" "_sv);
    set_needs_layout();
    if (auto* window = this->window()) {
        window->make_first_responder(first_responder());
        // The country chosen, in sight.
        if (step == Step::Region) {
            window->update_layout_now();
            if (m_region_table->selected_row() >= 0)
                m_region_table->scroll_to_row(static_cast<size_t>(m_region_table->selected_row()));
        }
    }
    StringView const names[] = { "welcome"_sv, "region"_sv, "account"_sv, "look"_sv };
    say(names[static_cast<size_t>(step)]);
}

void Setup::go_back()
{
    if (m_step != Step::Welcome)
        show_step(static_cast<Step>(static_cast<int>(m_step) - 1));
}

void Setup::go_forward()
{
    switch (m_step) {
    case Step::Welcome:
        show_step(Step::Region);
        return;
    case Step::Region:
        show_step(Step::Account);
        return;
    case Step::Account:
        if (account_is_ready())
            show_step(Step::Look);
        return;
    case Step::Look:
        finish();
        return;
    }
}

bool Setup::account_is_ready()
{
    auto problem = [&](StringView what) {
        m_problem->set_text(what);
        say("problem"_sv);
        return false;
    };
    if (m_full_name->text().is_empty())
        return problem("Enter your full name."_sv);
    if (!Core::AccountDatabase::is_valid_name(m_account_name->text()))
        return problem("An account name starts with a lower-case letter, and has only those, numbers, \xE2\x80\x9C-\xE2\x80\x9D, \xE2\x80\x9C_\xE2\x80\x9D and \xE2\x80\x9C.\xE2\x80\x9D"_sv);
    if (Core::AccountDatabase::person_named(m_account_name->text()).has_value())
        return problem("That account name is taken."_sv);
    if (m_password->text().is_empty())
        return problem("Enter a password."_sv);
    if (m_password->text() != m_verify->text())
        return problem("The passwords don\xE2\x80\x99t match."_sv);
    return true;
}

void Setup::finish()
{
    auto const name = MUST(String::from_view(m_account_name->text()));
    auto made = Core::AccountDatabase::create({ name.view(), m_full_name->text(), m_password->text(), m_hint->text(), true });
    if (made.is_error()) {
        show_step(Step::Account);
        m_problem->set_text("The account couldn\xE2\x80\x99t be made."_sv);
        return;
    }
    // What was chosen, in their own settings.
    StringBuilder path;
    path.appendff("/Users/{}/Library/Preferences/com.vulture.desktop.json", name.view());
    StringBuilder settings;
    auto const selected = m_region_table->selected_row();
    StringView const region = selected >= 0 ? m_regions->at(static_cast<size_t>(selected)).identifier : "en_US"_sv;
    settings.append("{ \"appearance\" : \""_sv);
    settings.append(m_dark ? "Dark"_sv : "Light"_sv);
    settings.append("\", \"region\" : \""_sv);
    settings.append(region);
    settings.append("\" }\n"_sv);
    write_owned(path.view(), settings.view(), made.value());
    (void)Core::AccountDatabase::mark_setup_done();
    StringBuilder said;
    said.appendff("made {}", name.view());
    say(said.view());
    // Nothing of what was typed stays.
    m_password->set_text({});
    m_verify->set_text({});
    if (m_on_done)
        m_on_done(name.view());
}

}
