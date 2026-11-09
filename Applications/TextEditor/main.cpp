/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <Documents.h>
#include <LibUI/Application.h>
#include <LibUI/Panels.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TableView.h>
#include <LibUI/TextView.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Plain text, in a window per document. A document that has changes says so in its
// title and asks before it goes. Saving writes a new file and moves it over the old,
// so that a failure along the way leaves the old one as it was.

using namespace UI;
using namespace Gfx;

namespace {

Appearance const& tokens()
{
    return Appearance::the();
}

StringView last_component(StringView path)
{
    for (size_t i = path.length(); i > 0; --i) {
        if (path[i - 1] == '/')
            return path.substring_view(i, path.length() - i);
    }
    return path;
}

int s_documents_opened = 0;
bool s_wraps = true;

void save_open_documents();
void rebuild_open_recent();

// How often what's been typed is put somewhere safe: at most this much is lost if the program, or the machine, goes.
constexpr u64 autosave_milliseconds = 2000;
constexpr StringView version_last_opened = "Last Opened"_sv;
constexpr StringView version_last_saved = "Last Saved"_sv;

class VersionsModel final : public TableModel {
public:
    explicit VersionsModel(Vector<Desktop::DocumentVersion> versions)
        : m_versions(move(versions))
    {
    }

    [[nodiscard]] size_t row_count() const override { return m_versions.size(); }
    [[nodiscard]] String text_at(size_t row, size_t) const override
    {
        time_t when = static_cast<time_t>(m_versions[row].time);
        tm parts;
        gmtime_r(&when, &parts);
        StringBuilder text;
        text.appendff("{} \xE2\x80\x94 {:04}-{:02}-{:02} {:02}:{:02}:{:02}", m_versions[row].label.view(), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
        return MUST(text.to_string());
    }
    [[nodiscard]] Desktop::DocumentVersion const& at(size_t row) const { return m_versions[row]; }

private:
    Vector<Desktop::DocumentVersion> m_versions;
};

class Document final : public UI::Window {
public:
    explicit Document(StringView path = {})
        : UI::Window("Untitled"_sv, frame_for_next())
    {
        auto& scroll = set_content<ScrollView>();
        m_scroll = &scroll;
        scroll.set_scrolls_horizontally(!s_wraps);
        auto& text = scroll.make_document<TextView>();
        text.set_corrects_text(true);
        text.set_accessibility_label("Document"_sv);
        m_text = &text;
        m_font = tokens().named_font("body"_sv);
        text.set_font(m_font);
        text.set_wraps(s_wraps);
        text.set_text_inset(tokens().metric("textEditor.inset"_sv));
        text.on_change = [this] { text_changed(); };
        (void)make_first_responder(&text);
        on_close_request = [this] { return confirm_close(); };
        if (!path.is_empty())
            load(path);
        update_title();
    }

    [[nodiscard]] bool has_changes() const { return m_edited; }
    [[nodiscard]] TextView& text() { return *m_text; }
    [[nodiscard]] StringView path() const { return m_path.view(); }
    [[nodiscard]] StringView autosave_path() const { return m_autosave_path.view(); }

    void load(StringView path)
    {
        auto bytes = Platform::the().read_file(path);
        if (bytes.is_error()) {
            report("The document could not be opened."_sv, path, bytes.error());
            return;
        }
        StringView contents(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
        m_text->set_text(contents);
        m_saved = MUST(String::from_view(contents));
        m_path = MUST(String::from_view(path));
        m_edited = false;
        m_needs_autosave = false;
        (void)Desktop::DocumentVersions(m_path.view()).add(contents, version_last_opened);
        Desktop::note_recent_document(m_path.view());
        update_title();
    }

    // What was there when the program went away: the file as it was autosaved, compared with what was last saved of it.
    void restore_named(StringView path, bool edited)
    {
        load(path);
        if (!edited)
            return;
        Desktop::DocumentVersions versions(m_path.view());
        auto saved = versions.newest_labelled(version_last_saved);
        if (!saved.has_value())
            saved = versions.newest_labelled(version_last_opened);
        if (saved.has_value()) {
            auto contents = versions.read(saved.value());
            if (!contents.is_error())
                m_saved = contents.release_value();
        }
        m_edited = m_text->text() != m_saved.view();
        update_title();
    }

    void restore_untitled(StringView autosave_path)
    {
        auto bytes = Platform::the().read_file(autosave_path);
        if (bytes.is_error())
            return;
        m_autosave_path = MUST(String::from_view(autosave_path));
        m_text->set_text(StringView(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size()));
        m_edited = true;
        update_title();
    }

    void save(Function<void()> then = nullptr)
    {
        if (m_path.is_empty()) {
            save_as(move(then));
            return;
        }
        write_out(m_path.view(), move(then));
    }

    void save_as(Function<void()> then = nullptr)
    {
        auto directory = Desktop::home_directory();
        StringBuilder documents;
        documents.append(directory.view());
        documents.append("/Documents"_sv);
        auto where = MUST(documents.to_string());
        auto suggested = m_path.is_empty() ? MUST(String::from_view("Untitled.txt"_sv)) : MUST(String::from_view(last_component(m_path.view())));
        auto shared_then = adopt_own(*new Function<void()>(move(then)));
        Desktop::run_file_panel(*this, FilePanel::Kind::Save, where.view(), suggested.view(), [this, then = move(shared_then)](StringView chosen) mutable {
            if (chosen.is_empty())
                return;
            auto path = MUST(String::from_view(chosen));
            write_out(path.view(), move(*then));
        });
    }

    // Written where it belongs, if it has somewhere to belong; and if not, where it can be found again.
    void autosave()
    {
        if (!m_needs_autosave)
            return;
        m_needs_autosave = false;
        if (!m_path.is_empty()) {
            // In place, as a Mac's documents are: what's on disk is what's in the window. It's how it was that's kept.
            (void)Desktop::write_file_atomically(m_path.view(), m_text->text());
        } else if (m_text->text().is_empty()) {
            forget_autosave();
        } else {
            bool const first_time = m_autosave_path.is_empty();
            if (first_time)
                m_autosave_path = Desktop::new_autosave_path();
            (void)Desktop::write_file_atomically(m_autosave_path.view(), m_text->text());
            // And written down where it is, or it's as good as lost.
            if (first_time)
                save_open_documents();
        }
        printf("texteditor: autosaved\n");
        fflush(stdout);
    }

    void revert_to(StringView label)
    {
        if (m_path.is_empty())
            return;
        Desktop::DocumentVersions versions(m_path.view());
        auto version = versions.newest_labelled(label);
        if (version.has_value())
            restore_version(version.value());
    }

    void browse_versions()
    {
        if (m_path.is_empty())
            return;
        auto const& t = tokens();
        Desktop::DocumentVersions versions(m_path.view());
        auto root = adopt_own(*new StackView(Axis::Vertical, 8, Alignment::Fill, Insets::all(t.metric("alert.padding"_sv))));
        (void)root->add<Label>("Versions of this document"_sv);
        auto model = adopt_own(*new VersionsModel(versions.list()));
        auto& table = root->add<TableView>(model.ptr());
        table.set_headers_visible(false);
        table.add_column("Version"_sv, 380);
        table.set_accessibility_label("Versions"_sv);
        table.set_stretch(0, 1);
        auto* table_pointer = &table;
        auto* model_pointer = model.ptr();
        auto& buttons = root->add<StackView>(Axis::Horizontal, 8, Alignment::Center);
        auto& filler = buttons.add<View>();
        filler.set_stretch(1, 0);
        auto& done = buttons.add<Button>("Done"_sv);
        done.on_action = [this] { Sheet::dismiss(*this); };
        auto& restore = buttons.add<Button>("Restore"_sv, ButtonStyle::Default);
        restore.on_action = [this, table_pointer, model_pointer] {
            int const row = table_pointer->selected_row();
            if (row < 0)
                return;
            restore_version(model_pointer->at(static_cast<size_t>(row)));
            Sheet::dismiss(*this);
        };
        buttons.set_fixed_height(20);
        m_versions_model = move(model);
        Sheet::present(*this, adopt_own(*static_cast<View*>(root.leak_ptr())), { 440, 300 });
    }

    void set_wraps(bool wraps)
    {
        m_text->set_wraps(wraps);
        m_scroll->set_scrolls_horizontally(!wraps);
        m_scroll->set_needs_layout();
    }

    void show_fonts()
    {
        FontPanel::the().show(m_font, [this](FontSpec font) {
            m_font = font;
            m_text->set_font(font);
        });
    }

    void forget_autosave()
    {
        if (m_autosave_path.is_empty())
            return;
        unlink(m_autosave_path.characters());
        m_autosave_path = {};
    }

private:
    [[nodiscard]] static IntRect frame_for_next()
    {
        auto const& t = tokens();
        int width = static_cast<int>(t.metric("textEditor.width"_sv));
        int height = static_cast<int>(t.metric("textEditor.height"_sv));
        auto screen = Platform::the().screen_size();
        // Each from the last, down and across.
        int offset = (s_documents_opened++ % 8) * static_cast<int>(t.metric("textEditor.cascade"_sv));
        return { (screen.width - width) / 2 + offset, max(40, (screen.height - height) / 3) + offset, width, height };
    }

    void restore_version(Desktop::DocumentVersion const& version)
    {
        auto contents = Desktop::DocumentVersions(m_path.view()).read(version);
        if (contents.is_error()) {
            report("The version could not be restored."_sv, m_path.view(), contents.error());
            return;
        }
        // What's being left behind isn't lost.
        (void)Desktop::DocumentVersions(m_path.view()).add(m_text->text(), "Before Reverting"_sv);
        m_text->set_text(contents.value().view());
        m_saved = contents.release_value();
        (void)Desktop::write_file_atomically(m_path.view(), m_saved.view());
        m_edited = false;
        m_needs_autosave = false;
        update_title();
        printf("texteditor: reverted to %s\n", version.label.characters());
        fflush(stdout);
    }

    void text_changed()
    {
        m_needs_autosave = true;
        bool edited = m_text->text() != m_saved.view();
        if (edited == m_edited)
            return;
        m_edited = edited;
        update_title();
        save_open_documents();
    }

    void update_title()
    {
        StringBuilder title;
        title.append(m_path.is_empty() ? "Untitled"_sv : last_component(m_path.view()));
        if (m_edited)
            title.append(" \xE2\x80\x94 Edited"_sv);
        set_title(title.view());
    }

    void write_out(StringView path, Function<void()> then)
    {
        auto written = Desktop::write_file_atomically(path, m_text->text());
        if (written.is_error()) {
            report("The document could not be saved."_sv, path, written.error());
            return;
        }
        m_path = MUST(String::from_view(path));
        m_saved = MUST(String::from_view(m_text->text()));
        m_edited = false;
        m_needs_autosave = false;
        forget_autosave();
        (void)Desktop::DocumentVersions(m_path.view()).add(m_saved.view(), version_last_saved);
        Desktop::note_recent_document(m_path.view());
        update_title();
        save_open_documents();
        rebuild_open_recent();
        if (then)
            then();
    }

    void report(StringView message, StringView path, Error const& error)
    {
        StringBuilder information;
        information.append(last_component(path));
        information.append(": "_sv);
        auto code = error.code();
        information.append(StringView(strerror(code), strlen(strerror(code))));
        auto text = MUST(information.to_string());
        Alert alert(message, text.view());
        alert.add_button("OK"_sv);
        alert.show(*this, [](size_t) { });
    }

    bool confirm_close()
    {
        if (m_closing_anyway)
            return true;
        if (m_path.is_empty()) {
            // A new document with nothing in it isn't worth a question.
            if (m_text->text().is_empty())
                return true;
            StringBuilder message;
            message.append("Do you want to keep this new document \xE2\x80\x9CUntitled\xE2\x80\x9D?"_sv);
            auto text = MUST(message.to_string());
            Alert alert(text.view(), "You can choose to save your changes, or delete this document immediately. You can\xE2\x80\x99t undo this action."_sv);
            alert.add_button("Save"_sv, true);
            alert.add_button("Cancel"_sv);
            alert.add_button("Delete"_sv);
            alert.show(*this, [this](size_t answer) {
                if (answer == 0) {
                    save([this] { close(); });
                } else if (answer == 2) {
                    forget_autosave();
                    m_closing_anyway = true;
                    close();
                }
            });
            return false;
        }
        if (!m_edited)
            return true;
        if (!Desktop::asks_to_keep_changes()) {
            // It's been saving itself all along; this is just the last of that.
            m_needs_autosave = true;
            autosave();
            return true;
        }
        StringBuilder message;
        message.append("Do you want to keep the changes you made to the document \xE2\x80\x9C"_sv);
        message.append(last_component(m_path.view()));
        message.append("\xE2\x80\x9D?"_sv);
        auto text = MUST(message.to_string());
        Alert alert(text.view(), "Your changes will be lost if you don\xE2\x80\x99t keep them."_sv);
        alert.add_button("Save"_sv, true);
        alert.add_button("Cancel"_sv);
        alert.add_button("Revert Changes"_sv);
        alert.show(*this, [this](size_t answer) {
            if (answer == 0) {
                save([this] { close(); });
            } else if (answer == 2) {
                (void)Desktop::write_file_atomically(m_path.view(), m_saved.view());
                m_closing_anyway = true;
                close();
            }
        });
        return false;
    }

    ScrollView* m_scroll { nullptr };
    TextView* m_text { nullptr };
    FontSpec m_font;
    String m_path;
    String m_saved;
    String m_autosave_path;
    bool m_edited { false };
    bool m_needs_autosave { false };
    bool m_closing_anyway { false };
    OwnPtr<VersionsModel> m_versions_model;
};

Vector<Document*> s_documents;
Menu* s_open_recent { nullptr };

Document& new_document(StringView path = {});

void save_open_documents()
{
    auto list = JSON::Value::array();
    for (auto* document : s_documents) {
        auto one = JSON::Value::object();
        MUST(one.set("path"_sv, MUST(JSON::Value::string(document->path()))));
        MUST(one.set("autosave"_sv, MUST(JSON::Value::string(document->autosave_path()))));
        MUST(one.set("edited"_sv, JSON::Value(document->has_changes())));
        auto frame = JSON::Value::array();
        auto const rect = document->frame();
        MUST(frame.append(JSON::Value(static_cast<i64>(rect.x))));
        MUST(frame.append(JSON::Value(static_cast<i64>(rect.y))));
        MUST(frame.append(JSON::Value(static_cast<i64>(rect.width))));
        MUST(frame.append(JSON::Value(static_cast<i64>(rect.height))));
        MUST(one.set("frame"_sv, move(frame)));
        MUST(list.append(move(one)));
    }
    auto state = JSON::Value::object();
    MUST(state.set("documents"_sv, move(list)));
    Desktop::SavedState("com.vulture.TextEditor"_sv).save(state);
}

void rebuild_open_recent()
{
    if (!s_open_recent)
        return;
    s_open_recent->items().clear();
    for (auto const& path : Desktop::recent_documents()) {
        auto copy = path;
        s_open_recent->add(last_component(path.view()), [copy = move(copy)] { new_document(copy.view()); });
    }
    if (!s_open_recent->items().is_empty())
        s_open_recent->add_separator();
    s_open_recent->add("Clear Menu"_sv, [] {
        Desktop::clear_recent_items();
        rebuild_open_recent();
    });
    Application::the().publish_menus();
}

Document& new_document(StringView path)
{
    auto& document = Application::the().make_window<Document>(path);
    s_documents.append(&document);
    document.on_closed = [&document] {
        s_documents.remove_all_matching([&](Document* other) { return other == &document; });
        document.forget_autosave();
        save_open_documents();
    };
    document.show();
    save_open_documents();
    return document;
}

Document* document_at(StringView path)
{
    for (auto* document : s_documents) {
        if (document->path() == path)
            return document;
    }
    return nullptr;
}

void open_or_show(StringView path)
{
    if (auto* existing = document_at(path)) {
        existing->show();
        existing->backend().activate();
        return;
    }
    new_document(path);
    rebuild_open_recent();
}

Document* key_document()
{
    auto* key = Application::the().key_window();
    for (auto* document : s_documents) {
        if (document == key)
            return document;
    }
    return nullptr;
}

void new_document_with(StringView text)
{
    auto& document = new_document();
    document.text().set_text(text);
}

// What it had open when it stopped without being asked to.
int restore_documents()
{
    auto state = Desktop::SavedState("com.vulture.TextEditor"_sv).load();
    if (!state.has_value())
        return 0;
    auto const* documents = state->get_array("documents"_sv);
    if (!documents)
        return 0;
    int restored = 0;
    for (size_t i = 0; i < documents->size(); ++i) {
        auto const& one = documents->at(i);
        auto path = one.get_string("path"_sv).value_or(StringView {});
        auto autosave = one.get_string("autosave"_sv).value_or(StringView {});
        bool const edited = one.get_boolean("edited"_sv).value_or(false);
        if (path.is_empty() && autosave.is_empty())
            continue;
        auto& document = new_document();
        if (!path.is_empty())
            document.restore_named(path, edited);
        else
            document.restore_untitled(autosave);
        if (auto const* frame = one.get_array("frame"_sv); frame && frame->size() == 4) {
            document.set_frame({ static_cast<int>(frame->at(0).as_number()), static_cast<int>(frame->at(1).as_number()),
                static_cast<int>(frame->at(2).as_number()), static_cast<int>(frame->at(3).as_number()) });
        }
        ++restored;
    }
    if (restored > 0) {
        printf("texteditor: restored %d\n", restored);
        fflush(stdout);
    }
    return restored;
}

void autosave_everything()
{
    for (auto* document : s_documents)
        document->autosave();
}

}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    auto application_or_error = Application::create("Text Editor"_sv, "com.vulture.TextEditor"_sv);
    if (application_or_error.is_error()) {
        fprintf(stderr, "Text Editor: cannot reach the window server\n");
        return 1;
    }
    auto application = application_or_error.release_value();

    // With no documents open it's still here, as a Mac's are.
    application->set_quits_with_last_window(false);
    auto menus = application->standard_menus();
    auto& file = *new Menu("File"_sv);
    file.add_with_shortcut("New"_sv, KeyN, "\xE2\x8C\x98N"_sv, [] { new_document(); });
    file.add_with_shortcut("Open\xE2\x80\xA6"_sv, KeyO, "\xE2\x8C\x98O"_sv, [] {
        auto* key = key_document();
        if (!key) {
            key = &new_document();
        }
        auto home = Desktop::home_directory();
        Desktop::run_file_panel(*key, FilePanel::Kind::Open, home.view(), {}, [](StringView chosen) {
            if (!chosen.is_empty())
                open_or_show(chosen);
        });
    });
    s_open_recent = &file.add_submenu("Open Recent"_sv);
    file.add_separator();
    file.add_with_shortcut("Close"_sv, KeyW, "\xE2\x8C\x98W"_sv, [] {
        if (auto* window = Application::the().key_window())
            window->close();
    });
    file.add_with_shortcut("Save"_sv, KeyS, "\xE2\x8C\x98S"_sv, [] {
        if (auto* document = key_document())
            document->save();
    });
    file.add_with_shortcut("Save As\xE2\x80\xA6"_sv, KeyS, "\xE2\x87\xA7\xE2\x8C\x98S"_sv, [] {
        if (auto* document = key_document())
            document->save_as();
    },
        WS::ModifierCommand | WS::ModifierShift);
    auto& revert = file.add_submenu("Revert To"_sv);
    revert.add("Last Saved"_sv, [] {
        if (auto* document = key_document())
            document->revert_to(version_last_saved);
    });
    revert.add("Last Opened"_sv, [] {
        if (auto* document = key_document())
            document->revert_to(version_last_opened);
    });
    revert.add_separator();
    revert.add("Browse All Versions\xE2\x80\xA6"_sv, [] {
        if (auto* document = key_document())
            document->browse_versions();
    });
    menus->items().insert_returning(1, "File"_sv).submenu = adopt_own(file);

    auto& format = *new Menu("Format"_sv);
    format.add_with_shortcut("Show Fonts"_sv, KeyT, "\xE2\x8C\x98T"_sv, [] {
        if (auto* document = key_document())
            document->show_fonts();
    });
    format.add_separator();
    auto& wrap = format.add("Wrap to Window"_sv);
    wrap.checked = true;
    wrap.action = [&wrap, &application] {
        s_wraps = !s_wraps;
        wrap.checked = s_wraps;
        for (auto* document : s_documents)
            document->set_wraps(s_wraps);
        application->publish_menus();
    };
    menus->items().insert_returning(3, "Format"_sv).submenu = adopt_own(format);
    application->set_main_menu(move(menus));

    // Each with changes asks in turn; the last to close ends the program.
    application->on_quit_request = [] {
        // Settings can say to leave them as they are, for next time.
        if (!Desktop::closes_windows_when_quitting()) {
            autosave_everything();
            save_open_documents();
            return true;
        }
        Vector<Document*> documents;
        for (auto* document : s_documents)
            documents.append(document);
        for (auto* document : documents)
            document->close();
        bool const finished = s_documents.is_empty();
        if (finished)
            Desktop::SavedState("com.vulture.TextEditor"_sv).clear();
        return finished;
    };
    application->on_open_arguments = [](Span<StringView const> arguments) {
        for (auto argument : arguments) {
            if (!argument.starts_with('-'))
                open_or_show(argument);
        }
    };
    application->on_perform_service = [](StringView message, StringView text) {
        if (message == "newWindowContainingSelection"_sv) {
            new_document_with(text);
            printf("texteditor: a new window, from a service\n");
            fflush(stdout);
        }
    };
    application->on_reopen = [] {
        if (s_documents.is_empty())
            new_document();
    };
    application->event_loop().add_timer(autosave_milliseconds, true, [] { autosave_everything(); });
    // Being told to stop, as at the end of a session: what's in the windows goes first.
    application->event_loop().on_signal(SIGTERM, [&application] {
        autosave_everything();
        application->quit();
    });

    int opened = restore_documents();
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--service") == 0 && i + 2 < argc) {
            new_document_with(StringView(argv[i + 2], strlen(argv[i + 2])));
            ++opened;
            i += 2;
            continue;
        }
        if (argv[i][0] == '-')
            continue;
        open_or_show(StringView(argv[i], strlen(argv[i])));
        ++opened;
    }
    if (opened == 0)
        new_document();
    rebuild_open_recent();
    printf("texteditor: ready\n");
    fflush(stdout);
    return application->exec();
}
