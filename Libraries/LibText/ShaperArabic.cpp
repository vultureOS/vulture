/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibText/Shaper.h>

// Arabic, and those that join the way it does: each letter takes the form that goes with whether it joins what's before it and what's after it. That's worked out here, from Unicode's
// joining types, and the font has a feature for each form.

namespace Text::Shaping {

using Unicode::JoiningGroup;
using Unicode::JoiningType;

void prepare_arabic(ShapingBuffer& buffer, Plan& plan, bool is_syriac)
{
    u32 const isol = plan.mask_for(feature_tag("isol"));
    u32 const fina = plan.mask_for(feature_tag("fina"));
    u32 const fin2 = plan.mask_for(feature_tag("fin2"));
    u32 const fin3 = plan.mask_for(feature_tag("fin3"));
    u32 const medi = plan.mask_for(feature_tag("medi"));
    u32 const med2 = plan.mask_for(feature_tag("med2"));
    u32 const init = plan.mask_for(feature_tag("init"));

    auto const can_join_next = [](JoiningType type) { return type == JoiningType::D || type == JoiningType::L || type == JoiningType::C; };
    auto const can_join_previous = [](JoiningType type) { return type == JoiningType::D || type == JoiningType::R || type == JoiningType::C; };

    // What's transparent is looked through.
    Vector<size_t> letters;
    MUST(letters.try_ensure_capacity(buffer.size()));
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (Unicode::joining_type_of(buffer[i].code_point) != JoiningType::T)
            letters.append(i);
    }
    for (size_t n = 0; n < letters.size(); ++n) {
        auto& glyph = buffer[letters[n]];
        auto const type = Unicode::joining_type_of(glyph.code_point);
        bool const joins_previous = n > 0 && can_join_previous(type) && can_join_next(Unicode::joining_type_of(buffer[letters[n - 1]].code_point));
        bool const joins_next = n + 1 < letters.size() && can_join_next(type) && can_join_previous(Unicode::joining_type_of(buffer[letters[n + 1]].code_point));
        // (Non-joiners and such aren't letters to anyone: they don't take a form.)
        if (type == JoiningType::U || type == JoiningType::C)
            continue;
        if (joins_previous && joins_next) {
            glyph.mask |= medi;
        } else if (joins_previous) {
            glyph.mask |= fina;
        } else if (joins_next) {
            glyph.mask |= init;
        } else {
            glyph.mask |= isol;
        }
    }

    // Syriac's Alaph has forms of its own at the end of a word, by what's before it, and Dalath and Rish have an effect on it.
    if (is_syriac) {
        for (size_t n = 0; n < letters.size(); ++n) {
            auto& glyph = buffer[letters[n]];
            if (Unicode::joining_group_of(glyph.code_point) != JoiningGroup::ALAPH)
                continue;
            if (glyph.mask & fina)
                continue;
            if (!(glyph.mask & isol))
                continue;
            if (n == 0)
                continue;
            auto const previous = buffer[letters[n - 1]].code_point;
            auto const previous_type = Unicode::joining_type_of(previous);
            glyph.mask &= ~isol;
            if (previous_type == JoiningType::R || previous_type == JoiningType::U)
                glyph.mask |= Unicode::joining_group_of(previous) == JoiningGroup::DALATH_RISH ? fin3 : fin2;
            else
                glyph.mask |= fin2;
        }
        (void)med2;
    }

    // Each form is a step of its own, so that what's next can see what the one before did.
    plan.add(feature_tag("isol"), isol);
    plan.pause();
    plan.add(feature_tag("fina"), fina);
    plan.pause();
    plan.add(feature_tag("fin2"), fin2);
    plan.pause();
    plan.add(feature_tag("fin3"), fin3);
    plan.pause();
    plan.add(feature_tag("medi"), medi);
    plan.pause();
    plan.add(feature_tag("med2"), med2);
    plan.pause();
    plan.add(feature_tag("init"), init);
    plan.pause();
}

void prepare_hangul(ShapingBuffer& buffer, Plan& plan)
{
    // Jamo that have no syllable to become are given their positions' forms.
    u32 const ljmo = plan.mask_for(feature_tag("ljmo"));
    u32 const vjmo = plan.mask_for(feature_tag("vjmo"));
    u32 const tjmo = plan.mask_for(feature_tag("tjmo"));
    for (auto& glyph : buffer) {
        auto const code = glyph.code_point;
        if ((code >= 0x1100 && code <= 0x115f) || (code >= 0xa960 && code <= 0xa97c))
            glyph.mask |= ljmo;
        else if ((code >= 0x1160 && code <= 0x11a7) || (code >= 0xd7b0 && code <= 0xd7c6))
            glyph.mask |= vjmo;
        else if ((code >= 0x11a8 && code <= 0x11ff) || (code >= 0xd7cb && code <= 0xd7fb))
            glyph.mask |= tjmo;
    }
    plan.add(feature_tag("ljmo"), ljmo);
    plan.add(feature_tag("vjmo"), vjmo);
    plan.add(feature_tag("tjmo"), tjmo);
    plan.pause();
}

}
