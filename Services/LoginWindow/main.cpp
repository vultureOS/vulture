/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <os/log.h>
#include <Desktop.h>
#include <LibCore/Account.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibUI/Application.h>
#include <LibUI/Controls.h>
#include <LibUI/Layout.h>
#include <LibUI/Motion.h>
#include <LibUI/TextField.h>
#include <SessionEndpoint.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// The window everyone starts at. With the right password it becomes that person, as far
// as the session's first programs go; it stays root itself, to take the next person
// when they log out. It also answers com.vulture.loginwindow, for what ends a session.

using namespace UI;
using namespace Gfx;

namespace {

bool s_still;

class Avatar final : public View {
public:
    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        float size = Appearance::the().metric("loginwindow.avatar"_sv);
        return { size, size };
    }

    void draw(Graphics& graphics) override
    {
        int scale = static_cast<int>(graphics.scale());
        auto icon = Desktop::icon_named("User"_sv, static_cast<int>(width()), scale);
        if (icon)
            graphics.painter().draw_bitmap(*icon, FloatRect { 0, 0, width(), height() });
    }
};

class PowerButton final : public Control {
public:
    PowerButton(StringView title, bool is_restart, Function<void()> action)
        : m_title(MUST(String::from_view(title)))
        , m_is_restart(is_restart)
    {
        on_action = move(action);
    }

    [[nodiscard]] FloatSize intrinsic_size() const override
    {
        auto const& t = Appearance::the();
        return { max(t.metric("loginwindow.buttonSize"_sv), Appearance::text_width(t.named_font("small"_sv), m_title.view())), t.metric("loginwindow.buttonSize"_sv) + 18 };
    }

    void draw(Graphics& graphics) override
    {
        auto const& t = Appearance::the();
        float size = t.metric("loginwindow.buttonSize"_sv);
        FloatPoint center { width() / 2, size / 2 };
        graphics.fill_circle(center, size / 2, t.color("loginwindow.fieldFill"_sv).with_opacity(is_pressed() ? 1 : 0.7f));
        auto glyph = t.color("loginwindow.text"_sv);
        StrokeStyle style;
        style.width = 2.5f;
        style.cap = LineCap::Round;
        Path arc;
        if (m_is_restart) {
            // Around, with an arrowhead where it ends.
            arc.arc_to(center, 9, -0.6f, 4.4f);
            graphics.painter().stroke_path(arc, style, glyph);
            FloatPoint tip { center.x + 7.5f, center.y - 8.5f };
            Path head;
            head.move_to({ tip.x - 1, tip.y - 5 });
            head.line_to({ tip.x + 1.5f, tip.y + 2 });
            head.line_to({ tip.x - 6, tip.y + 2.5f });
            head.close();
            graphics.painter().fill_path(head, glyph);
        } else {
            graphics.line({ center.x, center.y - 9 }, { center.x, center.y }, 2.5f, glyph, true);
            arc.arc_to(center, 9, -0.9f, 4.04f);
            graphics.painter().stroke_path(arc, style, glyph);
        }
        auto font = t.named_font("small"_sv);
        graphics.draw_text_centered(m_title.view(), font, { 0, size + 2, width(), 14 }, t.color("loginwindow.text"_sv));
    }

    [[nodiscard]] Accessibility::Role accessibility_role() const override { return Accessibility::Role::Button; }
    [[nodiscard]] String accessibility_label() const override { return MUST(String::from_view(m_title.view())); }

private:
    String m_title;
    bool m_is_restart;
};

os_log_t session_log()
{
    static os_log_t const log = os_log_create("com.vulture.loginwindow", "session");
    return log;
}

// The session's programs, run as the person who logged in.
class Session final : public SessionStub {
public:
    explicit Session(Application& application)
        : m_application(application)
    {
    }

    void set_on_ended(Function<void()> callback) { m_on_ended = move(callback); }

    void start(Core::Account const& account)
    {
        m_uid = account.uid();
        prepare_home(account);
        StringView const programs[] = {
            "/System/Library/CoreServices/DesktopShell"_sv,
            "/System/Library/CoreServices/Dock"_sv,
            "/System/Library/CoreServices/Files.app/Contents/MacOS/Files"_sv,
        };
        for (auto program : programs)
            spawn(account, program);
        m_running = true;
        Platform::the().play_sound("login"_sv);
        printf("loginwindow: session started\n");
        os_log(session_log(), "Session started for %.*s", static_cast<int>(account.name().length()), account.name().characters_without_null_termination());
        fflush(stdout);
    }

    void log_out(IPC::Sender const&) override { end(); }

    void restart(IPC::Sender const&) override
    {
        end();
        (void)reboot(RB_AUTOBOOT);
    }

    void shut_down(IPC::Sender const&) override
    {
        end();
        (void)reboot(RB_POWEROFF);
    }

private:
    void prepare_home(Core::Account const& account)
    {
        auto home = MUST(String::from_view(account.home()));
        mkdir(home.characters(), 0755);
        (void)chown(home.characters(), account.uid(), account.gid());
        StringView const folders[] = { "Desktop"_sv, "Documents"_sv, "Downloads"_sv, "Library"_sv, "Library/Preferences"_sv, "Pictures"_sv, "Music"_sv, ".Trash"_sv };
        for (auto folder : folders) {
            StringBuilder path;
            path.append(home.view());
            path.append("/"_sv);
            path.append(folder);
            auto full = MUST(path.to_string());
            if (mkdir(full.characters(), 0755) == 0)
                (void)chown(full.characters(), account.uid(), account.gid());
        }
    }

    void spawn(Core::Account const& account, StringView program)
    {
        pid_t child = fork();
        if (child < 0)
            return;
        if (child == 0) {
            setsid();
            if (account.become().is_error())
                _exit(126);
            auto home = MUST(String::from_view(account.home()));
            setenv("HOME", home.characters(), 1);
            auto name = MUST(String::from_view(account.name()));
            setenv("USER", name.characters(), 1);
            setenv("LOGNAME", name.characters(), 1);
            chdir(home.characters());
            auto path = MUST(String::from_view(program));
            if (s_still)
                execl(path.characters(), path.characters(), "--still", nullptr);
            else
                execl(path.characters(), path.characters(), nullptr);
            _exit(127);
        }
        m_children.append(child);
    }

    void end()
    {
        if (!m_running)
            return;
        m_running = false;
        // Everything of theirs, as they have it: anything may have started anything.
        pid_t killer = fork();
        if (killer == 0) {
            (void)setuid(m_uid);
            kill(-1, SIGTERM);
            _exit(0);
        }
        if (killer > 0)
            (void)waitpid(killer, nullptr, 0);
        for (auto child : m_children)
            (void)waitpid(child, nullptr, WNOHANG);
        m_children.clear();
        m_application.event_loop().deferred_invoke([this] {
            if (m_on_ended)
                m_on_ended();
        });
    }

    Application& m_application;
    Vector<pid_t> m_children;
    uid_t m_uid { 0 };
    bool m_running { false };
    Function<void()> m_on_ended;
};

class LoginScreen final : public View {
public:
    LoginScreen(Session& session, StringView user_name)
        : m_session(session)
        , m_user(MUST(String::from_view(user_name)))
    {
        auto const& t = Appearance::the();
        m_stack = &add<StackView>(Axis::Vertical, t.metric("loginwindow.spacing"_sv), Alignment::Center);
        (void)m_stack->add<Avatar>();
        auto account = Core::Account::from_name(user_name);
        StringView shown = !account.is_error() && !account.value().full_name().is_empty() ? account.value().full_name() : user_name;
        auto& name = m_stack->add<Label>(shown, FontSpec { t.metric("loginwindow.nameSize"_sv), FontWeight::Medium, false });
        name.set_color_token("loginwindow.text"_sv);
        m_field = &m_stack->add<TextField>(StringView {}, FieldStyle::Rounded);
        m_field->set_secure(true);
        m_field->set_placeholder("Enter Password"_sv);
        m_field->set_fixed_size({ t.metric("loginwindow.fieldWidth"_sv), t.metric("loginwindow.fieldHeight"_sv) });
        m_field->set_accessibility_label("Password"_sv);
        m_message = &m_stack->add<Label>(" "_sv, t.named_font("small"_sv));
        m_message->set_color_token("loginwindow.error"_sv);
        m_buttons = &add<StackView>(Axis::Horizontal, t.metric("loginwindow.buttonSpacing"_sv), Alignment::Center);
        (void)m_buttons->add<PowerButton>("Restart"_sv, true, [] { (void)reboot(RB_AUTOBOOT); });
        (void)m_buttons->add<PowerButton>("Shut Down"_sv, false, [] { (void)reboot(RB_POWEROFF); });
    }

    [[nodiscard]] TextField& field() { return *m_field; }

    void try_logging_in()
    {
        auto account = Core::Account::from_name(m_user.view());
        if (account.is_error() || !account.value().authenticate(m_field->text())) {
            m_message->set_text("Incorrect password"_sv);
            m_field->set_text({});
            printf("loginwindow: incorrect password\n");
            fflush(stdout);
            return;
        }
        m_message->set_text(" "_sv);
        m_field->set_text({});
        if (window())
            window()->hide();
        m_session.start(account.value());
    }

    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, Appearance::the().color("loginwindow.scrim"_sv));
    }

    void layout() override
    {
        auto wanted = m_stack->preferred_size();
        float center_y = height() * 0.42f;
        m_stack->set_frame({ (width() - wanted.width) / 2, center_y - wanted.height / 2, wanted.width, wanted.height });
        auto buttons = m_buttons->preferred_size();
        m_buttons->set_frame({ (width() - buttons.width) / 2, height() - buttons.height - 48, buttons.width, buttons.height });
    }

private:
    Session& m_session;
    String m_user;
    StackView* m_stack { nullptr };
    StackView* m_buttons { nullptr };
    TextField* m_field { nullptr };
    Label* m_message { nullptr };
};

}

int main(int argc, char** argv)
{
    // How long a boot took is measured by this (Meta/budgets.sh).
    setenv("VULTURE_REPORT_LAUNCH", "1", 1);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--still") == 0)
            s_still = true;
    }
    if (s_still)
        setenv("VULTURE_STILL", "1", 1);
    auto application_or_error = Application::create();
    if (application_or_error.is_error()) {
        fprintf(stderr, "LoginWindow: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    if (s_still)
        set_motion_enabled(false);

    Session session(*application);
    auto port = IPC::check_in("com.vulture.loginwindow"_sv);
    OwnPtr<IPC::Listener> listener;
    if (!port.is_error())
        listener = adopt_own(*new IPC::Listener(application->event_loop(), port.release_value(), [&](IPC::ReceivedMessage& message) { (void)session.handle(message); }));

    auto screen = Platform::the().screen_size();
    auto window = adopt_own(*new UI::Window("Login"_sv, { 0, 0, screen.width, screen.height }, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
    window->set_transparent(true);
    MaterialRegion material { { 0, 0, screen.width, screen.height }, WS::Material::UnderWindow };
    window->set_materials(Span<MaterialRegion const>(&material, 1));
    auto& login = window->set_content<LoginScreen>(session, "vulture"_sv);
    window->set_default_action([&login] { login.try_logging_in(); });
    session.set_on_ended([&window, &login] {
        window->show();
        window->make_first_responder(&login.field());
        printf("loginwindow: ready\n");
        fflush(stdout);
    });
    window->show();
    window->update_layout_now();
    window->make_first_responder(&login.field());
    application->event_loop().add_timer(300, false, [] {
        printf("loginwindow: ready\n");
        fflush(stdout);
    });
    return application->exec();
}
