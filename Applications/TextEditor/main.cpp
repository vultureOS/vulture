/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Desktop.h>
#include <LibUI/Application.h>
#include <LibUI/Panels.h>
#include <LibUI/ScrollView.h>
#include <LibUI/TextView.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

ErrorOr<void> write_file_safely(StringView path, StringView contents)
{
    StringBuilder temporary_builder;
    temporary_builder.append(path);
    temporary_builder.appendff(".{}.saving", getpid());
    auto temporary = TRY(temporary_builder.to_string());
    auto final_path = TRY(String::from_view(path));
    int fd = open(temporary.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return Error::from_errno(errno);
    size_t done = 0;
    while (done < contents.length()) {
        ssize_t wrote = write(fd, contents.characters_without_null_termination() + done, contents.length() - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            unlink(temporary.characters());
            return Error::from_errno(saved);
        }
        done += static_cast<size_t>(wrote);
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(temporary.characters(), final_path.characters()) != 0) {
        int saved = errno;
        unlink(temporary.characters());
        return Error::from_errno(saved);
    }
    return {};
}

int s_documents_opened = 0;
bool s_wraps = true;

class Document final : public UI::Window {
public:
    explicit Document(StringView path = {})
        : UI::Window("Untitled"_sv, frame_for_next())
    {
        auto& scroll = set_content<ScrollView>();
        m_scroll = &scroll;
        scroll.set_scrolls_horizontally(!s_wraps);
        auto& text = scroll.make_document<TextView>();
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
        FilePanel::run(*this, FilePanel::Kind::Save, where.view(), suggested.view(), [this, then = move(shared_then)](StringView chosen) mutable {
            if (chosen.is_empty())
                return;
            auto path = MUST(String::from_view(chosen));
            write_out(path.view(), move(*then));
        });
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

    void text_changed()
    {
        bool edited = m_text->text() != m_saved.view();
        if (edited == m_edited)
            return;
        m_edited = edited;
        update_title();
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
        auto written = write_file_safely(path, m_text->text());
        if (written.is_error()) {
            report("The document could not be saved."_sv, path, written.error());
            return;
        }
        m_path = MUST(String::from_view(path));
        m_saved = MUST(String::from_view(m_text->text()));
        m_edited = false;
        update_title();
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
        if (!m_edited || m_closing_anyway)
            return true;
        StringBuilder message;
        message.append("Do you want to save the changes made to the document \xE2\x80\x9C"_sv);
        message.append(m_path.is_empty() ? "Untitled"_sv : last_component(m_path.view()));
        message.append("\xE2\x80\x9D?"_sv);
        auto text = MUST(message.to_string());
        Alert alert(text.view(), "Your changes will be lost if you don\xE2\x80\x99t save them."_sv);
        alert.add_button("Save"_sv, true);
        alert.add_button("Cancel"_sv);
        alert.add_button("Don\xE2\x80\x99t Save"_sv);
        alert.show(*this, [this](size_t answer) {
            if (answer == 0) {
                save([this] { close(); });
            } else if (answer == 2) {
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
    bool m_edited { false };
    bool m_closing_anyway { false };
};

Vector<Document*> s_documents;

Document& new_document(StringView path = {})
{
    auto& document = Application::the().make_window<Document>(path);
    s_documents.append(&document);
    document.on_closed = [&document] {
        s_documents.remove_all_matching([&](Document* other) { return other == &document; });
    };
    document.show();
    return document;
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

    auto menus = application->standard_menus();
    auto& file = *new Menu("File"_sv);
    file.add_with_shortcut("New"_sv, KeyN, "\xE2\x8C\x98N"_sv, [] { new_document(); });
    file.add_with_shortcut("Open\xE2\x80\xA6"_sv, KeyO, "\xE2\x8C\x98O"_sv, [] {
        auto* key = key_document();
        if (!key) {
            key = &new_document();
        }
        auto home = Desktop::home_directory();
        FilePanel::run(*key, FilePanel::Kind::Open, home.view(), {}, [](StringView chosen) {
            if (!chosen.is_empty())
                new_document(chosen);
        });
    });
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
        bool asked = false;
        Vector<Document*> documents;
        for (auto* document : s_documents)
            documents.append(document);
        for (auto* document : documents) {
            if (document->has_changes())
                asked = true;
            document->close();
        }
        return !asked;
    };

    int opened = 0;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-')
            continue;
        new_document(StringView(argv[i], strlen(argv[i])));
        ++opened;
    }
    if (opened == 0)
        new_document();
    printf("texteditor: ready\n");
    fflush(stdout);
    return application->exec();
}
