/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Error.h>
#include <LibBase/HashMap.h>
#include <LibBase/String.h>
#include <LibBase/StringView.h>
#include <LibBase/Vector.h>

// What a key types, on a keyboard of some country: from a file in /System/Library/Keyboard Layouts, made from measuring a real Mac (Meta/generate-keyboards.py).

class KeyboardLayout {
public:
    // By the file's name, without its extension: US.
    [[nodiscard]] static ErrorOr<KeyboardLayout> load(StringView identifier);
    // What every machine starts as, needing no file: US, as far as plain keys go.
    [[nodiscard]] static KeyboardLayout builtin();

    enum Layer : u8 {
        Plain,
        Shift,
        Option,
        ShiftOption,
        Caps,
        LayerCount,
    };

    // What pressing this key types, in UTF-8. A dead key types nothing yet: the next key says what comes of it.
    [[nodiscard]] String type(u16 code, bool shift, bool option, bool caps, bool command_or_control);

    [[nodiscard]] StringView name() const { return m_name.view(); }
    [[nodiscard]] StringView identifier() const { return m_identifier.view(); }

private:
    struct KeyEntry {
        u16 code { 0 };
        // In UTF-8. A dead key has `dead` set, and this is what it is by itself.
        String text[LayerCount];
        bool dead[LayerCount] {};
    };
    struct ComposeEntry {
        u16 dead_code { 0 };
        u8 dead_layer { 0 };
        u16 code { 0 };
        bool shifted { false };
        String text;
    };

    [[nodiscard]] KeyEntry const* entry_for(u16 code) const;

    String m_name;
    String m_identifier;
    Vector<KeyEntry> m_keys;
    Vector<ComposeEntry> m_compose;
    // A dead key that's waiting for what's next.
    KeyEntry const* m_pending { nullptr };
    u8 m_pending_layer { 0 };
};
