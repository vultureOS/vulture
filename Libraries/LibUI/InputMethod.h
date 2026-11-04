/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/OwnPtr.h>
#include <LibBase/String.h>
#include <LibBase/Vector.h>
#include <LibGfx/Geometry.h>
#include <LibUI/Event.h>

// For languages with more characters than a keyboard has keys. What's being typed is shown, underlined, in the text ("marked"), until it's been decided what it is.

namespace UI {

class TextStorage;

class InputMethod {
public:
    virtual ~InputMethod() = default;

    struct Result {
        // Whether that key's been dealt with: if not, it does whatever it would have.
        bool handled { false };
        // Text that's now decided.
        String commit;
    };
    [[nodiscard]] virtual Result handle_key(KeyEvent const&) = 0;

    // What's still being typed.
    [[nodiscard]] virtual StringView marked_text() const = 0;
    [[nodiscard]] virtual Span<String const> candidates() const { return {}; }
    [[nodiscard]] virtual size_t selected_candidate() const { return 0; }
    virtual void reset() = 0;
};

void install_input_context();

[[nodiscard]] OwnPtr<InputMethod> make_pinyin_input_method();
[[nodiscard]] OwnPtr<InputMethod> make_kana_input_method();
[[nodiscard]] OwnPtr<InputMethod> make_hangul_input_method();

// The one that's in use in this program, as Settings says. Text controls ask it first about each key.
class InputContext {
public:
    [[nodiscard]] static InputContext& the();

    // True if it's been dealt with. Where the candidates go, if there are any: just below this, on the screen.
    [[nodiscard]] bool handle_key(KeyEvent const&, TextStorage&, Gfx::IntPoint caret_on_screen);
    void focus_lost(TextStorage&);
    void open_emoji_picker(TextStorage&);

    // Which is in use: for tests, as Settings otherwise says. An empty name is none.
    void select(StringView name);

    [[nodiscard]] StringView selected() const { return m_name.view(); }

private:
    InputContext() = default;

    void refresh();
    void show_candidates(Gfx::IntPoint);
    void hide_candidates();

    OwnPtr<InputMethod> m_method;
    String m_name;
    i64 m_preferences_changed { -1 };
    bool m_selected_by_hand { false };
    class CandidateWindow* m_candidate_window { nullptr };
    class EmojiPicker* m_emoji_picker { nullptr };
    TextStorage* m_emoji_target { nullptr };
    Vector<String> m_accents;
};

}
