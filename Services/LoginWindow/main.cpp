/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Setup.h"
#include "Views.h"
#include <os/log.h>
#include <Desktop.h>
#include <ScreenSaver.h>
#include <LibCore/Account.h>
#include <LibCore/AccountDatabase.h>
#include <LibIPC/Bootstrap.h>
#include <LibIPC/Listener.h>
#include <LibJSON/JSON.h>
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
#include <time.h>
#include <unistd.h>

// The window everyone starts at, and comes back to. It asks who's using the computer and for their password, and starts
// their session as them: several at once, one on the screen (fast user switching), the window server showing only that
// person's windows. Over a session, it's the lock screen and the screen saver. On a new computer, Setup comes first. It
// stays root, to start each session and take the next person. It answers com.vulture.loginwindow (Session.ipc).

using namespace UI;
using namespace Gfx;
using namespace LoginWindow;

namespace {

bool s_still;

os_log_t session_log()
{
    static os_log_t const log = os_log_create("com.vulture.loginwindow", "session");
    return log;
}

void say(char const* format, StringView what = {})
{
    printf("loginwindow: ");
    printf(format, static_cast<int>(what.length()), what.characters_without_null_termination());
    printf("\n");
    fflush(stdout);
}

// What someone's chosen in their own settings: the desktop's, read as root from their home.
JSON::Value preferences_of(StringView home, StringView identifier)
{
    StringBuilder path;
    path.appendff("{}/Library/Preferences/{}.json", home, identifier);
    auto bytes = Platform::the().read_file(path.view());
    if (bytes.is_error())
        return JSON::Value::object();
    auto parsed = JSON::parse(StringView(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
    return parsed.is_error() ? JSON::Value::object() : parsed.release_value();
}

// One person's programs, run as them.
class Session {
public:
    explicit Session(Core::Account account)
        : m_account(move(account))
    {
    }

    [[nodiscard]] uid_t uid() const { return m_account.uid(); }
    [[nodiscard]] StringView name() const { return m_account.name(); }
    [[nodiscard]] Core::Account const& account() const { return m_account; }

    void start()
    {
        prepare_home();
        StringView const programs[] = {
            "/System/Library/CoreServices/DesktopShell"_sv,
            "/System/Library/CoreServices/Dock"_sv,
            "/System/Library/CoreServices/Files.app/Contents/MacOS/Files"_sv,
        };
        for (auto program : programs)
            m_programs.append({ program, spawn(program), 0, 0 });
        os_log(session_log(), "Session started for %.*s", static_cast<int>(name().length()), name().characters_without_null_termination());
    }

    // The Dock, the shell and Files come back if they stop, as a Mac's do: at once, unless one keeps on stopping (more
    // than five times in a minute), which would only go round and round.
    void keep_running()
    {
        if (m_ending)
            return;
        for (auto& program : m_programs) {
            if (program.pid <= 0)
                continue;
            // Gone: collected here, or already (ECHILD), or not there at all.
            pid_t const ended = waitpid(program.pid, nullptr, WNOHANG);
            if (ended == 0 || (ended < 0 && errno != ECHILD && !(kill(program.pid, 0) < 0 && errno == ESRCH)))
                continue;
            struct timespec moment;
            clock_gettime(CLOCK_MONOTONIC, &moment);
            if (moment.tv_sec - program.counting_since > 60) {
                program.counting_since = moment.tv_sec;
                program.restarts = 0;
            }
            auto const name = program.path.substring_view(program.path.find_last('/').value_or(0) + 1);
            if (++program.restarts > 5) {
                os_log_error(session_log(), "%.*s keeps stopping: not started again", static_cast<int>(name.length()), name.characters_without_null_termination());
                program.pid = 0;
                continue;
            }
            program.pid = spawn(program.path);
            os_log(session_log(), "Started %.*s again", static_cast<int>(name.length()), name.characters_without_null_termination());
            say("started %.*s again", name);
        }
    }

    // Everything of theirs, as they have it: anything may have started anything.
    void end()
    {
        m_ending = true;
        pid_t killer = fork();
        if (killer == 0) {
            (void)setuid(uid());
            kill(-1, SIGTERM);
            _exit(0);
        }
        if (killer > 0)
            (void)waitpid(killer, nullptr, 0);
        for (auto const& program : m_programs)
            (void)waitpid(program.pid, nullptr, WNOHANG);
        m_programs.clear();
        os_log(session_log(), "Session ended for %.*s", static_cast<int>(name().length()), name().characters_without_null_termination());
    }

private:
    void prepare_home()
    {
        auto home = MUST(String::from_view(m_account.home()));
        mkdir(home.characters(), 0755);
        (void)chown(home.characters(), uid(), m_account.gid());
        StringView const folders[] = { "Desktop"_sv, "Documents"_sv, "Downloads"_sv, "Library"_sv, "Library/Preferences"_sv, "Pictures"_sv, "Music"_sv, ".Trash"_sv };
        for (auto folder : folders) {
            StringBuilder path;
            path.appendff("{}/{}", home.view(), folder);
            auto full = MUST(path.to_string());
            if (mkdir(full.characters(), 0755) == 0)
                (void)chown(full.characters(), uid(), m_account.gid());
        }
    }

    pid_t spawn(StringView program)
    {
        pid_t child = fork();
        if (child < 0)
            return 0;
        if (child == 0) {
            setsid();
            if (m_account.become().is_error())
                _exit(126);
            auto home = MUST(String::from_view(m_account.home()));
            setenv("HOME", home.characters(), 1);
            auto user = MUST(String::from_view(name()));
            setenv("USER", user.characters(), 1);
            setenv("LOGNAME", user.characters(), 1);
            chdir(home.characters());
            auto path = MUST(String::from_view(program));
            if (s_still)
                execl(path.characters(), path.characters(), "--still", nullptr);
            else
                execl(path.characters(), path.characters(), nullptr);
            _exit(127);
        }
        return child;
    }

    struct Program {
        StringView path;
        pid_t pid { 0 };
        int restarts { 0 };
        i64 counting_since { 0 };
    };

    Core::Account m_account;
    Vector<Program> m_programs;
    bool m_ending { false };
};

class LoginWindowController;

// What's on the screen: the clock at the top; the people to choose from, or the one chosen with the password field; and
// Restart and Shut Down at the foot.
class LoginScreen final : public View {
public:
    enum class Mode {
        Choosing,
        Password,
        Locked,
    };

    explicit LoginScreen(LoginWindowController& controller);

    void show_people(Vector<Core::AccountDatabase::Person> const& people, Vector<String> const& logged_in);
    void choose(Core::AccountDatabase::Person const&, Mode);
    [[nodiscard]] Mode mode() const { return m_mode; }
    [[nodiscard]] StringView chosen() const { return m_chosen.view(); }
    [[nodiscard]] TextField& field() { return *m_field; }
    void tick() { m_clock->tick(); }
    void say_wrong(Optional<String> const& hint);
    void clear_message();

    void draw(Graphics& graphics) override
    {
        graphics.fill({ 0, 0, width(), height() }, Appearance::the().color("loginwindow.scrim"_sv));
    }

    void layout() override;

private:
    LoginWindowController& m_controller;
    Mode m_mode { Mode::Password };
    String m_chosen;
    Clock* m_clock { nullptr };
    StackView* m_people { nullptr };
    StackView* m_person { nullptr };
    Picture* m_picture { nullptr };
    Label* m_name { nullptr };
    StackView* m_field_row { nullptr };
    Button* m_back { nullptr };
    View* m_balance { nullptr };
    TextField* m_field { nullptr };
    Label* m_message { nullptr };
    Button* m_switch_user { nullptr };
    StackView* m_buttons { nullptr };
    size_t m_people_count { 0 };
};

// Over everything, while it's on: anything done ends it.
class SaverView final : public View {
public:
    explicit SaverView(StringView name)
    {
        struct timespec moment;
        clock_gettime(CLOCK_MONOTONIC, &moment);
        m_shown_at = static_cast<double>(moment.tv_sec) + static_cast<double>(moment.tv_nsec) / 1e9;
        m_saver = &add<Desktop::ScreenSaver>(name);
        if (is_still())
            m_saver->set_time(now().seconds, now().offset);
    }

    Function<void()> on_woken;
    [[nodiscard]] Desktop::ScreenSaver& saver() { return *m_saver; }

    void layout() override { m_saver->set_frame({ 0, 0, width(), height() }); }

    View* hit_test(FloatPoint point) override
    {
        return frame().contains(point) ? this : nullptr;
    }
    bool mouse_down(MouseEvent const&) override
    {
        wake();
        return true;
    }
    void mouse_moved(MouseEvent const&) override
    {
        // Not what comes as it appears, with the pointer where it already was: any after that.
        struct timespec moment;
        clock_gettime(CLOCK_MONOTONIC, &moment);
        double const now = static_cast<double>(moment.tv_sec) + static_cast<double>(moment.tv_nsec) / 1e9;
        if (m_shown_at == 0)
            m_shown_at = now;
        if (now - m_shown_at > 0.5)
            wake();
    }
    bool key_down(KeyEvent const&) override
    {
        wake();
        return true;
    }
    [[nodiscard]] bool accepts_first_responder() const override { return true; }

private:
    void wake()
    {
        if (on_woken)
            on_woken();
    }

    Desktop::ScreenSaver* m_saver { nullptr };
    double m_shown_at { 0 };
};

class LoginWindowController final : public SessionStub {
public:
    explicit LoginWindowController(Application& application)
        : m_application(application)
    {
        auto screen = Platform::the().screen_size();
        m_frame = { 0, 0, screen.width, screen.height };
        m_window = adopt_own(*new UI::Window("Login"_sv, m_frame, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
        m_window->set_transparent(true);
        m_window->backend().set_level(WS::LevelShield);
        MaterialRegion material { { 0, 0, screen.width, screen.height }, WS::Material::UnderWindow };
        m_window->set_materials(Span<MaterialRegion const>(&material, 1));
    }

    void begin()
    {
        if (!Core::AccountDatabase::setup_done()) {
            begin_setup();
            return;
        }
        show_login();
        m_application.event_loop().add_timer(300, false, [] { say("ready"); });
        keep_sessions_running();
        // The clock, and the screen saver's wait.
        m_application.event_loop().add_timer(2000, true, [this] { every_little_while(); });
    }

    // ---- Session.ipc: from a session's own programs.
    void log_out(IPC::Sender const& sender) override
    {
        auto* session = session_of(sender.uid);
        if (!session)
            return;
        end_session(*session);
    }

    void restart(IPC::Sender const&) override
    {
        end_all();
        (void)reboot(RB_AUTOBOOT);
    }

    void shut_down(IPC::Sender const&) override
    {
        end_all();
        (void)reboot(RB_POWEROFF);
    }

    void lock_screen(IPC::Sender const& sender) override
    {
        if (sender.uid != 0 && sender.uid != m_console_uid)
            return;
        lock();
    }

    void start_screen_saver(IPC::Sender const& sender) override
    {
        if (sender.uid != 0 && sender.uid != m_console_uid)
            return;
        start_saver();
    }

    void switch_to(IPC::Sender const& sender, String name) override
    {
        if (sender.uid != 0 && sender.uid != m_console_uid)
            return;
        // Theirs stays as it is, locked, under the login window.
        lock_quietly();
        auto person = Core::AccountDatabase::person_named(name.view());
        if (person.has_value()) {
            show_people_list();
            m_screen->choose(person.value(), LoginScreen::Mode::Password);
        } else {
            show_people_list();
        }
        present();
        say("switching");
    }

    String logged_in(IPC::Sender const&) override
    {
        StringBuilder names;
        for (auto const& session : m_sessions) {
            names.append(session->name());
            names.append('\n');
        }
        return MUST(names.to_string());
    }

    // ---- What the screen's buttons do.
    void person_chosen(Core::AccountDatabase::Person const& person)
    {
        m_screen->choose(person, LoginScreen::Mode::Password);
        m_window->make_first_responder(&m_screen->field());
        say("chose %.*s", person.name.view());
    }

    void back_to_people()
    {
        show_people_list();
        m_window->make_first_responder(nullptr);
    }

    void password_entered()
    {
        auto name = MUST(String::from_view(m_screen->chosen()));
        auto account = Core::Account::from_name(name.view());
        bool const right = !account.is_error() && account.value().authenticate(m_screen->field().text());
        m_screen->field().set_text({});
        if (!right) {
            ++m_wrong;
            m_screen->say_wrong(m_wrong >= 3 ? Core::AccountDatabase::hint(name.view()) : Optional<String> {});
            say("incorrect password");
            return;
        }
        m_wrong = 0;
        m_screen->clear_message();
        if (auto* existing = session_of(account.value().uid())) {
            // Theirs is running: back to it.
            bool const was_locked = m_screen->mode() == LoginScreen::Mode::Locked;
            activate(*existing);
            say(was_locked ? "unlocked" : "switched to %.*s", name.view());
            return;
        }
        auto* session = new Session(account.release_value());
        m_sessions.append(adopt_own(*session));
        activate(*session);
        session->start();
        Platform::the().play_sound("login"_sv);
        say("session started");
    }

    void restart_now() { restart({}); }
    void shut_down_now() { shut_down({}); }
    void switch_user()
    {
        show_people_list();
        say("choosing");
    }

private:
    Session* session_of(uid_t uid)
    {
        for (auto& session : m_sessions) {
            if (session->uid() == uid)
                return session.ptr();
        }
        return nullptr;
    }

    Session* console_session() { return m_console_uid ? session_of(m_console_uid) : nullptr; }

    void keep_sessions_running()
    {
        m_application.event_loop().add_timer(500, true, [this] {
            for (auto& session : m_sessions)
                session->keep_running();
        });
    }

    void set_session(uid_t uid, bool locked)
    {
        m_console_uid = uid;
        m_locked = locked;
        (void)m_application.connection().server().set_session(uid, locked);
    }

    void begin_setup()
    {
        set_session(0, false);
        m_setup = &m_window->set_content<LoginWindow::Setup>([this](StringView name) {
            auto account = Core::Account::from_name(name);
            if (account.is_error())
                return;
            m_application.event_loop().deferred_invoke([this, name = MUST(String::from_view(name))] {
                auto made = Core::Account::from_name(name.view());
                if (made.is_error())
                    return;
                m_setup = nullptr;
                build_login_screen();
                auto* session = new Session(made.release_value());
                m_sessions.append(adopt_own(*session));
                activate(*session);
                session->start();
                Platform::the().play_sound("login"_sv);
                say("session started");
                m_application.event_loop().add_timer(2000, true, [this] { every_little_while(); });
                keep_sessions_running();
            });
        });
        m_window->set_default_action([this] {
            if (m_setup)
                m_setup->go_forward();
        });
        present();
        m_window->make_first_responder(m_setup->first_responder());
        say("setup");
    }

    void build_login_screen()
    {
        m_screen = &m_window->set_content<LoginScreen>(*this);
        m_window->set_default_action([this] {
            if (m_screen && m_screen->mode() != LoginScreen::Mode::Choosing)
                password_entered();
        });
    }

    void show_people_list()
    {
        if (!m_screen)
            build_login_screen();
        Vector<String> names;
        for (auto const& session : m_sessions)
            names.append(MUST(String::from_view(session->name())));
        auto people = Core::AccountDatabase::people();
        m_screen->show_people(people, names);
        // Only one: chosen already.
        if (people.size() == 1)
            m_screen->choose(people[0], LoginScreen::Mode::Password);
    }

    void show_login()
    {
        set_session(0, false);
        show_people_list();
        present();
    }

    void present()
    {
        m_window->show();
        m_window->update_layout_now();
        if (m_screen && m_screen->mode() != LoginScreen::Mode::Choosing)
            m_window->make_first_responder(&m_screen->field());
    }

    void activate(Session& session)
    {
        set_session(session.uid(), false);
        m_window->hide();
    }

    // The person at the screen stays chosen; nobody else's windows show, nor theirs.
    void lock_quietly()
    {
        if (!m_console_uid || m_locked)
            return;
        set_session(m_console_uid, true);
    }

    void lock()
    {
        auto* session = console_session();
        if (!session)
            return;
        stop_saver();
        lock_quietly();
        show_people_list();
        auto person = Core::AccountDatabase::person_named(session->name());
        if (person.has_value())
            m_screen->choose(person.value(), LoginScreen::Mode::Locked);
        present();
        say("locked");
    }

    void end_session(Session& session)
    {
        bool const was_console = session.uid() == m_console_uid;
        session.end();
        m_sessions.remove_all_matching([&](auto const& each) { return each.ptr() == &session; });
        if (!was_console)
            return;
        m_application.event_loop().deferred_invoke([this] {
            show_login();
            say("ready");
        });
    }

    void end_all()
    {
        for (auto& session : m_sessions)
            session->end();
        m_sessions.clear();
    }

    // ---- The screen saver.
    void start_saver()
    {
        if (m_saver_window || !console_session())
            return;
        auto const prefs = preferences_of(console_session()->account().home(), "com.vulture.screensaver"_sv);
        auto const name = prefs.get_string("name"_sv).value_or("Drift"_sv);
        m_saver_window = adopt_own(*new UI::Window("Screen Saver"_sv, m_frame, WS::WindowBare | WS::WindowCanBecomeKey | WS::WindowSticky));
        m_saver_window->backend().set_level(WS::LevelScreenSaver);
        auto& view = m_saver_window->set_content<SaverView>(name);
        view.on_woken = [this] {
            m_application.event_loop().deferred_invoke([this] { saver_woken(); });
        };
        m_saver_started = now_seconds();
        m_saver_window->show();
        m_saver_window->make_first_responder(&view);
        if (!s_still) {
            m_saver_timer = m_application.event_loop().add_timer(33, true, [this, &view] {
                view.saver().advance(now_seconds() - m_saver_started);
            });
        }
        say("screen saver started");
    }

    void stop_saver()
    {
        if (!m_saver_window)
            return;
        if (m_saver_timer > 0)
            m_application.event_loop().cancel_timer(m_saver_timer);
        m_saver_timer = -1;
        m_saver_window->hide();
        m_saver_window = nullptr;
        say("screen saver stopped");
    }

    void saver_woken()
    {
        auto* session = console_session();
        stop_saver();
        if (!session)
            return;
        // Asking for the password straight away is a Mac's way, unless they've said not to.
        auto const prefs = preferences_of(session->account().home(), "com.vulture.screensaver"_sv);
        if (prefs.get_boolean("askForPassword"_sv).value_or(true))
            lock();
    }

    static double now_seconds()
    {
        struct timespec moment;
        clock_gettime(CLOCK_MONOTONIC, &moment);
        return static_cast<double>(moment.tv_sec) + static_cast<double>(moment.tv_nsec) / 1e9;
    }

    void every_little_while()
    {
        if (m_screen)
            m_screen->tick();
        auto* session = console_session();
        if (!session || m_locked || m_saver_window)
            return;
        auto const prefs = preferences_of(session->account().home(), "com.vulture.screensaver"_sv);
        // Minutes; 0 for never. A Mac's default is 20.
        auto const minutes = prefs.get_number("idleTime"_sv).value_or(20);
        if (minutes <= 0)
            return;
        auto idle = m_application.connection().server().idle_time();
        if (!idle.is_error() && static_cast<double>(idle.value()) >= minutes * 60 * 1000)
            start_saver();
    }

    Application& m_application;
    IntRect m_frame;
    OwnPtr<UI::Window> m_window;
    LoginScreen* m_screen { nullptr };
    LoginWindow::Setup* m_setup { nullptr };
    Vector<OwnPtr<Session>> m_sessions;
    uid_t m_console_uid { 0 };
    bool m_locked { false };
    int m_wrong { 0 };
    OwnPtr<UI::Window> m_saver_window;
    int m_saver_timer { -1 };
    double m_saver_started { 0 };
};

LoginScreen::LoginScreen(LoginWindowController& controller)
    : m_controller(controller)
{
    auto const& t = Appearance::the();
    m_clock = &add<Clock>();
    m_people = &add<StackView>(Axis::Horizontal, t.metric("loginwindow.tileSpacing"_sv), Alignment::Leading);
    m_person = &add<StackView>(Axis::Vertical, t.metric("loginwindow.spacing"_sv), Alignment::Center);
    m_picture = &m_person->add<Picture>(t.metric("loginwindow.avatar"_sv));
    m_name = &m_person->add<Label>(""_sv, FontSpec { t.metric("loginwindow.nameSize"_sv), FontWeight::SemiBold });
    m_name->set_color_token("loginwindow.text"_sv);
    m_field_row = &m_person->add<StackView>(Axis::Horizontal, 8, Alignment::Center);
    m_back = &m_field_row->add<Button>(StringView {}, ButtonStyle::Circular);
    m_back->set_symbol(Symbol::ChevronLeft);
    m_back->set_accessibility_label("Back"_sv);
    m_back->on_action = [this] { m_controller.back_to_people(); };
    m_field = &m_field_row->add<TextField>(StringView {}, FieldStyle::Rounded);
    m_field->set_secure(true);
    m_field->set_placeholder("Enter Password"_sv);
    m_field->set_fixed_size({ t.metric("loginwindow.fieldWidth"_sv), t.metric("loginwindow.fieldHeight"_sv) });
    m_field->set_accessibility_label("Password"_sv);
    // As wide as Back, on the other side, so that the field stays in the middle.
    m_balance = &m_field_row->add<View>();
    m_balance->set_fixed_size(m_back->preferred_size());
    m_message = &m_person->add<Label>(" "_sv, t.named_font("small"_sv));
    m_message->set_color_token("loginwindow.error"_sv);
    m_switch_user = &m_person->add<Button>("Switch User"_sv, ButtonStyle::Borderless);
    m_switch_user->on_action = [this] { m_controller.switch_user(); };
    m_buttons = &add<StackView>(Axis::Horizontal, t.metric("loginwindow.buttonSpacing"_sv), Alignment::Center);
    (void)m_buttons->add<PowerButton>("Restart"_sv, true, [this] { m_controller.restart_now(); });
    (void)m_buttons->add<PowerButton>("Shut Down"_sv, false, [this] { m_controller.shut_down_now(); });
}

void LoginScreen::show_people(Vector<Core::AccountDatabase::Person> const& people, Vector<String> const& logged_in)
{
    m_people->remove_all_children();
    m_people_count = people.size();
    for (auto const& person : people) {
        bool in = false;
        for (auto const& name : logged_in)
            in = in || name.view() == person.name.view();
        auto copy = person;
        (void)m_people->add<UserTile>(person.name.view(), person.full_name.view(), in, [this, copy] { m_controller.person_chosen(copy); });
    }
    m_mode = Mode::Choosing;
    m_chosen = {};
    m_people->set_hidden(false);
    m_person->set_hidden(true);
    m_buttons->set_hidden(false);
    clear_message();
    set_needs_layout();
}

void LoginScreen::choose(Core::AccountDatabase::Person const& person, Mode mode)
{
    m_mode = mode;
    m_chosen = person.name;
    m_picture->set_full_name(person.full_name.view());
    m_name->set_text(person.full_name.view());
    m_people->set_hidden(true);
    m_person->set_hidden(false);
    // Back to the list, if there is one to go back to; another person, from the lock screen, if there are others.
    m_back->set_hidden(mode != Mode::Password || m_people_count < 2);
    m_balance->set_hidden(m_back->is_hidden());
    m_switch_user->set_hidden(mode != Mode::Locked || m_people_count < 2);
    // A locked screen can't be restarted from.
    m_buttons->set_hidden(mode == Mode::Locked);
    m_field->set_text({});
    clear_message();
    set_needs_layout();
}

void LoginScreen::say_wrong(Optional<String> const& hint)
{
    if (hint.has_value() && !hint->is_empty()) {
        StringBuilder text;
        text.appendff("Hint: {}", hint->view());
        m_message->set_text(text.view());
        return;
    }
    m_message->set_text("Incorrect password"_sv);
}

void LoginScreen::clear_message()
{
    m_message->set_text(" "_sv);
}

void LoginScreen::layout()
{
    auto const& t = Appearance::the();
    auto const clock = m_clock->preferred_size();
    m_clock->set_frame({ 0, height() * t.metric("loginwindow.clockTop"_sv), width(), clock.height });
    // The people, or the one chosen, in the lower part of the screen, as Sequoia has them.
    float const center_y = height() * t.metric("loginwindow.peopleCenter"_sv);
    auto const people = m_people->preferred_size();
    m_people->set_frame({ (width() - people.width) / 2, center_y - people.height / 2, people.width, people.height });
    auto const person = m_person->preferred_size();
    float const picture = t.metric("loginwindow.avatar"_sv);
    m_person->set_frame({ (width() - person.width) / 2, center_y - picture / 2, person.width, person.height });
    auto const buttons = m_buttons->preferred_size();
    m_buttons->set_frame({ (width() - buttons.width) / 2, height() - buttons.height - 48, buttons.width, buttons.height });
}

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
    LoginWindow::set_still(s_still);
    auto application_or_error = Application::create();
    if (application_or_error.is_error()) {
        fprintf(stderr, "LoginWindow: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();
    if (s_still)
        set_motion_enabled(false);

    LoginWindowController controller(*application);
    auto port = IPC::check_in("com.vulture.loginwindow"_sv);
    OwnPtr<IPC::Listener> listener;
    if (!port.is_error())
        listener = adopt_own(*new IPC::Listener(application->event_loop(), port.release_value(), [&](IPC::ReceivedMessage& message) { (void)controller.handle(message); }));
    controller.begin();
    return application->exec();
}
