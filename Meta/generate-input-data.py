#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Makes what the input methods look things up in, from Unicode's own data (Unihan_Readings.txt, emoji-test.txt):

    Meta/generate-input-data.py UNIHAN_READINGS EMOJI_TEST OUTPUT_DIRECTORY

Each is a text file with a line for each reading, or emoji:

    Pinyin.table:  ni 你 尼 泥 ...        from most used to least, as far as Unicode knows (kHanyuPinlu), and then the rest.
    Kana.table:    か 下 化 花 ...         a Japanese reading, in hiragana, and what it could be.
    Emoji.table:   GROUP 😀 grinning face
"""

import collections
import pathlib
import re
import sys

TONE = {
    "ā": "a", "á": "a", "ǎ": "a", "à": "a", "ē": "e", "é": "e", "ě": "e", "è": "e", "ī": "i", "í": "i", "ǐ": "i", "ì": "i", "ō": "o", "ó": "o", "ǒ": "o", "ò": "o",
    "ū": "u", "ú": "u", "ǔ": "u", "ù": "u", "ǖ": "v", "ǘ": "v", "ǚ": "v", "ǜ": "v", "ü": "v", "ń": "n", "ň": "n", "ǹ": "n", "ḿ": "m",
}


def plain_pinyin(reading):
    return "".join(TONE.get(c, c) for c in reading)


def katakana_to_hiragana(text):
    return text


def romaji_to_hiragana(text):
    """Unihan has its Japanese readings in plain letters, in the older way of writing them."""
    table = {
        "A": "あ", "I": "い", "U": "う", "E": "え", "O": "お", "KA": "か", "KI": "き", "KU": "く", "KE": "け", "KO": "こ", "SA": "さ", "SHI": "し", "SU": "す", "SE": "せ", "SO": "そ",
        "TA": "た", "CHI": "ち", "TSU": "つ", "TE": "て", "TO": "と", "NA": "な", "NI": "に", "NU": "ぬ", "NE": "ね", "NO": "の", "HA": "は", "HI": "ひ", "FU": "ふ", "HE": "へ", "HO": "ほ",
        "MA": "ま", "MI": "み", "MU": "む", "ME": "め", "MO": "も", "YA": "や", "YU": "ゆ", "YO": "よ", "RA": "ら", "RI": "り", "RU": "る", "RE": "れ", "RO": "ろ", "WA": "わ", "WO": "を", "N": "ん",
        "GA": "が", "GI": "ぎ", "GU": "ぐ", "GE": "げ", "GO": "ご", "ZA": "ざ", "JI": "じ", "ZU": "ず", "ZE": "ぜ", "ZO": "ぞ", "DA": "だ", "DE": "で", "DO": "ど", "BA": "ば", "BI": "び", "BU": "ぶ",
        "BE": "べ", "BO": "ぼ", "PA": "ぱ", "PI": "ぴ", "PU": "ぷ", "PE": "ぺ", "PO": "ぽ", "KYA": "きゃ", "KYU": "きゅ", "KYO": "きょ", "SHA": "しゃ", "SHU": "しゅ", "SHO": "しょ", "CHA": "ちゃ",
        "CHU": "ちゅ", "CHO": "ちょ", "NYA": "にゃ", "NYU": "にゅ", "NYO": "にょ", "HYA": "ひゃ", "HYU": "ひゅ", "HYO": "ひょ", "MYA": "みゃ", "MYU": "みゅ", "MYO": "みょ", "RYA": "りゃ", "RYU": "りゅ",
        "RYO": "りょ", "GYA": "ぎゃ", "GYU": "ぎゅ", "GYO": "ぎょ", "JA": "じゃ", "JU": "じゅ", "JO": "じょ", "BYA": "びゃ", "BYU": "びゅ", "BYO": "びょ", "PYA": "ぴゃ", "PYU": "ぴゅ", "PYO": "ぴょ",
        "WI": "ゐ", "WE": "ゑ",
    }
    out = ""
    text = text.upper()
    i = 0
    while i < len(text):
        for length in (3, 2, 1):
            part = text[i : i + length]
            if part in table:
                out += table[part]
                i += length
                break
        else:
            # Doubled consonants are the small tsu; anything else isn't a reading this understands.
            if i + 1 < len(text) and text[i] == text[i + 1] and text[i] not in "AIUEON":
                out += "っ"
                i += 1
            else:
                return None
    return out


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    directory = pathlib.Path(sys.argv[3])
    directory.mkdir(parents=True, exist_ok=True)

    counts = collections.defaultdict(dict)  # Syllable, then character, to how often.
    mandarin = collections.defaultdict(list)
    kana = collections.defaultdict(list)
    for line in open(sys.argv[1], encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        code, field, value = line.rstrip("\n").split("\t")
        character = chr(int(code[2:], 16))
        if field == "kHanyuPinlu":
            # "ni3(4525) ..." : with how often it's seen in a large body of text.
            for reading, count in re.findall(r"([^\s(]+?)\d?\((\d+)\)", value):
                syllable = re.sub(r"\d", "", plain_pinyin(reading))
                counts[syllable][character] = counts[syllable].get(character, 0) + int(count)
        elif field == "kMandarin":
            for reading in value.split():
                mandarin[plain_pinyin(reading)].append(character)
        elif field in ("kJapanese", "kJapaneseKun", "kJapaneseOn"):
            for reading in value.split():
                hiragana = romaji_to_hiragana(reading) if reading.isascii() else None
                if hiragana:
                    kana[hiragana].append(character)

    # Only the Basic Multilingual Plane's own block and its first extension: the rest are for scholars, and only make the lists longer.
    def common(character):
        return 0x4e00 <= ord(character) <= 0x9fff or 0x3400 <= ord(character) <= 0x4dbf

    lines = []
    for syllable in sorted(set(counts) | set(mandarin)):
        ordered = sorted(counts.get(syllable, {}), key=lambda c: -counts[syllable][c])
        seen = set(ordered)
        for character in mandarin.get(syllable, []):
            if character not in seen and common(character):
                seen.add(character)
                ordered.append(character)
        if ordered and re.fullmatch(r"[a-z]+", syllable):
            lines.append(syllable + " " + "".join(ordered))
    (directory / "Pinyin.table").write_text("\n".join(lines) + "\n", encoding="utf-8")

    lines = []
    for reading in sorted(kana):
        characters = []
        for character in kana[reading]:
            if common(character) and character not in characters:
                characters.append(character)
        if characters:
            lines.append(reading + " " + "".join(characters))
    (directory / "Kana.table").write_text("\n".join(lines) + "\n", encoding="utf-8")

    lines = []
    group = ""
    for line in open(sys.argv[2], encoding="utf-8"):
        if line.startswith("# group:"):
            group = line.split(":", 1)[1].strip().replace(" ", "_")
            continue
        if line.startswith("#") or not line.strip() or "; fully-qualified" not in line:
            continue
        codes, rest = line.split(";", 1)
        emoji = "".join(chr(int(c, 16)) for c in codes.split())
        name = rest.split("#", 1)[1].strip().split(" ", 2)[2]
        # (Skin tones are chosen from the one with none, as on a Mac.)
        if any(0x1f3fb <= ord(c) <= 0x1f3ff for c in emoji):
            continue
        lines.append(f"{group} {emoji} {name}")
    (directory / "Emoji.table").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
