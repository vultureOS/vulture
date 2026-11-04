#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Makes LibUnicode's tables from the Unicode Character Database.

    Meta/generate-unicode.py UCD_DIRECTORY Libraries/LibUnicode/Generated

The tables are checked in, so that building needs no download: this is only run for a new version of Unicode. Each property's table is
a list of where its value changes, which is how it's looked up too (see Tables.h), and each topic is in a file of its own, so that a
program that only wants one doesn't have to carry the rest.
"""

import pathlib
import re
import sys

MAX = 0x110000
HEADER = """/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Made by Meta/generate-unicode.py from the Unicode Character Database, version {version}. Don't edit it.

"""


def data_lines(path):
    with open(path, encoding="utf-8") as file:
        for raw in file:
            line = raw.split("#", 1)[0].strip()
            if line:
                yield [field.strip() for field in line.split(";")]


def code_range(text):
    if ".." in text:
        first, last = text.split("..")
        return int(first, 16), int(last, 16)
    return int(text, 16), int(text, 16)


def identifier(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


class Property:
    """A value for each code point, which starts as the default."""

    def __init__(self, default):
        self.values = [default] * MAX

    def assign(self, first, last, value):
        for code in range(first, last + 1):
            self.values[code] = value

    def runs_of(self, wanted):
        """(first, last) of each run of any of these values, with what value it is."""
        start = None
        for code, value in enumerate(self.values):
            if value in wanted:
                if start is None:
                    start = code
            elif start is not None:
                yield (start, code - 1), None
                start = None
        if start is not None:
            yield (start, MAX - 1), None

    def runs(self):
        runs = []
        previous = object()
        for code, value in enumerate(self.values):
            if value != previous:
                runs.append((code, value))
                previous = value
        return runs


def read_missing_defaults(path, prop):
    """The "@missing" lines say what isn't listed is; later ones win. They're comments, so they get found this way."""
    with open(path, encoding="utf-8") as file:
        for line in file:
            match = re.match(r"#\s*@missing:\s*([0-9A-Fa-f.]+)\s*;\s*([A-Za-z_]+)", line)
            if match:
                first, last = code_range(match.group(1))
                prop.assign(first, last, match.group(2))


def read_enumerated(path, default, field=1):
    prop = Property(default)
    read_missing_defaults(path, prop)
    for fields in data_lines(path):
        first, last = code_range(fields[0])
        prop.assign(first, last, fields[field])
    return prop


def boolean_set(path, wanted):
    """Whichever code points have this property in a file that lists several."""
    members = bytearray(MAX)
    for fields in data_lines(path):
        if fields[1] == wanted and len(fields) == 2:
            first, last = code_range(fields[0])
            for code in range(first, last + 1):
                members[code] = 1
    return members


def toggles_of(members):
    toggles = []
    state = 0
    for code in range(MAX):
        if members[code] != state:
            toggles.append(code)
            state = members[code]
    return toggles


def write_u32_table(out, name, values, per_line=10):
    out.append(f"extern u32 const {name}[];")
    out.append(f"u32 const {name}[] = {{")
    for i in range(0, len(values), per_line):
        out.append("    " + ", ".join(f"{value:#x}" for value in values[i : i + per_line]) + ",")
    out.append("};")
    out.append(f"extern size_t const {name}_count;")
    out.append(f"size_t const {name}_count = {len(values)};")
    out.append("")


def write_enumerated_table(out, name, prop, order):
    """Each entry is where a run starts, in the upper 24 bits, and what its value is, in the lower 8."""
    place = {value: index for index, value in enumerate(order)}
    write_u32_table(out, name, [(code << 8) | place[value] for code, value in prop.runs()])


def write_enum(out, name, order):
    out.append(f"enum class {name} : u8 {{")
    for value in order:
        out.append(f"    {identifier(value)},")
    out.append("};")
    out.append("")


def source_file(directory, name, version, body):
    text = HEADER.format(version=version) + '#include <LibUnicode/Tables.h>\n\nnamespace Unicode::Tables {\n\n' + "\n".join(body) + "\n}\n"
    (directory / name).write_text(text, encoding="utf-8")


def read_unicode_data(ucd):
    """Everything from UnicodeData.txt, by code point."""
    categories = Property("Cn")
    combining = Property(0)
    upper = {}
    lower = {}
    title = {}
    decompositions = {}
    decimal_digits = {}
    first_of_range = None
    for fields in data_lines(ucd / "UnicodeData.txt"):
        code = int(fields[0], 16)
        name = fields[1]
        if name.endswith(", First>"):
            first_of_range = code
            continue
        first = code
        if name.endswith(", Last>"):
            first = first_of_range
        categories.assign(first, code, fields[2])
        combining.assign(first, code, int(fields[3]))
        if fields[5]:
            parts = fields[5].split()
            compatibility = parts[0].startswith("<")
            mapping = [int(part, 16) for part in (parts[1:] if compatibility else parts)]
            decompositions[code] = (compatibility, mapping)
        if fields[6]:
            decimal_digits[code] = int(fields[6])
        if fields[12]:
            upper[code] = int(fields[12], 16)
        if fields[13]:
            lower[code] = int(fields[13], 16)
        if fields[14]:
            title[code] = int(fields[14], 16)
        elif fields[12]:
            title[code] = int(fields[12], 16)
    return categories, combining, upper, lower, title, decompositions, decimal_digits


def write_mapping_table(out, name, mapping):
    flat = []
    for code in sorted(mapping):
        flat.extend((code, mapping[code]))
    write_u32_table(out, name, flat, per_line=8)


def write_sequence_table(out, name, mapping):
    """Each entry is a code point, and what it maps to: how many, and the values, up to three."""
    flat = []
    for code in sorted(mapping):
        values = mapping[code]
        assert len(values) <= 3
        flat.extend((code, len(values), *values, *([0] * (3 - len(values)))))
    write_u32_table(out, name, flat, per_line=10)


def generate(ucd, directory):
    version = "16.0.0"
    categories, combining, upper, lower, title, decompositions, decimal_digits = read_unicode_data(ucd)

    # --- What's what.
    category_order = ["Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn"]
    enums = []
    write_enum(enums, "GeneralCategory", category_order)
    body = []
    write_enumerated_table(body, "general_category", categories, category_order)
    source_file(directory, "GeneralCategory.cpp", version, body)

    body = []
    combining_values = sorted(set(combining.values))
    write_enumerated_table(body, "combining_class", combining, combining_values)
    write_u32_table(body, "combining_class_values", combining_values)
    source_file(directory, "CombiningClass.cpp", version, body)

    # --- Boolean properties. Each its own table: most programs want very few.
    derived = ucd / "DerivedCoreProperties.txt"
    prop_list = ucd / "PropList.txt"
    emoji = ucd / "emoji-data.txt"
    booleans = [
        ("alphabetic", derived, "Alphabetic"), ("uppercase", derived, "Uppercase"), ("lowercase", derived, "Lowercase"), ("cased", derived, "Cased"),
        ("case_ignorable", derived, "Case_Ignorable"), ("id_start", derived, "ID_Start"), ("id_continue", derived, "ID_Continue"),
        ("xid_start", derived, "XID_Start"), ("xid_continue", derived, "XID_Continue"), ("math", derived, "Math"),
        ("default_ignorable", derived, "Default_Ignorable_Code_Point"), ("grapheme_extend", derived, "Grapheme_Extend"), ("grapheme_base", derived, "Grapheme_Base"),
        ("white_space", prop_list, "White_Space"), ("dash", prop_list, "Dash"), ("hyphen_like", prop_list, "Hyphen"), ("quotation_mark", prop_list, "Quotation_Mark"),
        ("terminal_punctuation", prop_list, "Terminal_Punctuation"), ("diacritic", prop_list, "Diacritic"), ("extender", prop_list, "Extender"),
        ("hex_digit", prop_list, "Hex_Digit"), ("ideographic", prop_list, "Ideographic"), ("unified_ideograph", prop_list, "Unified_Ideograph"), ("join_control", prop_list, "Join_Control"),
        ("noncharacter", prop_list, "Noncharacter_Code_Point"), ("soft_dotted", prop_list, "Soft_Dotted"), ("variation_selector", prop_list, "Variation_Selector"),
        ("regional_indicator", prop_list, "Regional_Indicator"),
        ("emoji", emoji, "Emoji"), ("emoji_presentation", emoji, "Emoji_Presentation"), ("emoji_modifier", emoji, "Emoji_Modifier"),
        ("emoji_modifier_base", emoji, "Emoji_Modifier_Base"), ("emoji_component", emoji, "Emoji_Component"), ("extended_pictographic", emoji, "Extended_Pictographic"),
    ]
    body = []
    for name, path, wanted in booleans:
        write_u32_table(body, f"is_{name}", toggles_of(boolean_set(path, wanted)))
    source_file(directory, "Booleans.cpp", version, body)

    # --- Scripts.
    scripts = read_enumerated(ucd / "Scripts.txt", "Unknown")
    script_order = sorted(set(scripts.values) | {"Unknown"})
    write_enum(enums, "Script", script_order)
    body = []
    write_enumerated_table(body, "script", scripts, script_order)
    source_file(directory, "Scripts.cpp", version, body)

    # --- Width on a terminal or a grid.
    east_asian = read_enumerated(ucd / "EastAsianWidth.txt", "N")
    east_asian_order = ["N", "Na", "H", "W", "F", "A"]
    write_enum(enums, "EastAsianWidth", east_asian_order)
    body = []
    write_enumerated_table(body, "east_asian_width", east_asian, east_asian_order)
    source_file(directory, "EastAsianWidth.cpp", version, body)

    # --- Case. Simple mappings, and those that don't come to one code point.
    special_upper = {}
    special_lower = {}
    special_title = {}
    for fields in data_lines(ucd / "SpecialCasing.txt"):
        # A fifth field is a condition: for those, there's code.
        if len(fields) > 4 and fields[4]:
            continue
        code = int(fields[0], 16)
        lower_to = [int(part, 16) for part in fields[1].split()]
        title_to = [int(part, 16) for part in fields[2].split()]
        upper_to = [int(part, 16) for part in fields[3].split()]
        if lower_to != [code]:
            special_lower[code] = lower_to
        if title_to != [code]:
            special_title[code] = title_to
        if upper_to != [code]:
            special_upper[code] = upper_to
    simple_fold = {}
    full_fold = {}
    turkic_fold = {}
    for fields in data_lines(ucd / "CaseFolding.txt"):
        code = int(fields[0], 16)
        mapping = [int(part, 16) for part in fields[2].split()]
        if fields[1] in ("C", "S"):
            simple_fold[code] = mapping[0]
        if fields[1] == "F":
            full_fold[code] = mapping
        if fields[1] == "T":
            turkic_fold[code] = mapping[0]
    body = []
    write_mapping_table(body, "simple_upper", upper)
    write_mapping_table(body, "simple_lower", lower)
    write_mapping_table(body, "simple_title", title)
    write_sequence_table(body, "special_upper", special_upper)
    write_sequence_table(body, "special_lower", special_lower)
    write_sequence_table(body, "special_title", special_title)
    source_file(directory, "CaseMapping.cpp", version, body)
    body = []
    write_mapping_table(body, "simple_fold", simple_fold)
    write_sequence_table(body, "full_fold", full_fold)
    write_mapping_table(body, "turkic_fold", turkic_fold)
    source_file(directory, "CaseFolding.cpp", version, body)

    # --- Digits.
    body = []
    write_mapping_table(body, "decimal_digit", decimal_digits)
    source_file(directory, "Digits.cpp", version, body)

    # --- Normalization.
    excluded = bytearray(MAX)
    for fields in data_lines(ucd / "DerivedNormalizationProps.txt"):
        if fields[1] == "Full_Composition_Exclusion":
            first, last = code_range(fields[0])
            for code in range(first, last + 1):
                excluded[code] = 1
    # Each is a code point with whether it's a compatibility mapping in the top bit; where its mapping starts, and how long that is.
    entries = []
    sequences = []
    for code in sorted(decompositions):
        compatibility, mapping = decompositions[code]
        entries.extend((code | (0x80000000 if compatibility else 0), (len(sequences) << 8) | len(mapping)))
        sequences.extend(mapping)
    compositions = []
    for code in sorted(decompositions):
        compatibility, mapping = decompositions[code]
        if not compatibility and not excluded[code] and len(mapping) == 2:
            compositions.append((mapping[0], mapping[1], code))
    compositions.sort()
    body = []
    write_u32_table(body, "decompositions", entries, per_line=8)
    write_u32_table(body, "decomposition_sequences", sequences)
    flat = []
    for first, second, composite in compositions:
        flat.extend((first, second, composite))
    write_u32_table(body, "compositions", flat, per_line=9)
    source_file(directory, "Normalization.cpp", version, body)

    # --- Segmentation.
    grapheme = read_enumerated(ucd / "GraphemeBreakProperty.txt", "Other")
    grapheme_order = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator", "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"]
    word = read_enumerated(ucd / "WordBreakProperty.txt", "Other")
    word_order = ["Other", "CR", "LF", "Newline", "Extend", "ZWJ", "Regional_Indicator", "Format", "Katakana", "Hebrew_Letter", "ALetter", "Single_Quote", "Double_Quote", "MidNumLet", "MidLetter", "MidNum", "Numeric", "ExtendNumLet", "WSegSpace"]
    sentence = read_enumerated(ucd / "SentenceBreakProperty.txt", "Other")
    sentence_order = ["Other", "CR", "LF", "Sep", "Extend", "Format", "Sp", "Lower", "Upper", "OLetter", "Numeric", "ATerm", "SContinue", "STerm", "Close"]
    write_enum(enums, "GraphemeBreak", grapheme_order)
    write_enum(enums, "WordBreak", word_order)
    write_enum(enums, "SentenceBreak", sentence_order)
    conjunct = Property("None")
    for fields in data_lines(derived):
        if fields[1] == "InCB":
            first, last = code_range(fields[0])
            conjunct.assign(first, last, fields[2])
    conjunct_order = ["None", "Linker", "Consonant", "Extend"]
    write_enum(enums, "IndicConjunctBreak", conjunct_order)
    body = []
    write_enumerated_table(body, "grapheme_break", grapheme, grapheme_order)
    write_enumerated_table(body, "word_break", word, word_order)
    write_enumerated_table(body, "sentence_break", sentence, sentence_order)
    write_enumerated_table(body, "indic_conjunct_break", conjunct, conjunct_order)
    source_file(directory, "Segmentation.cpp", version, body)

    # --- Line breaking.
    line = read_enumerated(ucd / "LineBreak.txt", "XX")
    line_order = sorted(set(line.values) | {"XX"})
    write_enum(enums, "LineBreakClass", line_order)
    body = []
    write_enumerated_table(body, "line_break", line, line_order)
    source_file(directory, "LineBreak.cpp", version, body)

    # --- Bidi.
    bidi = read_enumerated(ucd / "DerivedBidiClass.txt", "L")
    # (Its "@missing" lines use the long names.)
    long_names = {"Left_To_Right": "L", "Right_To_Left": "R", "Arabic_Letter": "AL", "European_Terminator": "ET"}
    bidi.values = [long_names.get(value, value) for value in bidi.values]
    bidi_order = ["L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"]
    assert set(bidi.values) <= set(bidi_order), set(bidi.values) - set(bidi_order)
    write_enum(enums, "BidiClass", bidi_order)
    mirrored = {}
    for fields in data_lines(ucd / "BidiMirroring.txt"):
        mirrored[int(fields[0], 16)] = int(fields[1], 16)
    brackets = []
    for fields in data_lines(ucd / "BidiBrackets.txt"):
        if fields[2] != "n":
            brackets.extend((int(fields[0], 16), int(fields[1], 16), 1 if fields[2] == "o" else 0))
    body = []
    write_enumerated_table(body, "bidi_class", bidi, bidi_order)
    write_mapping_table(body, "bidi_mirror", mirrored)
    write_u32_table(body, "bidi_brackets", brackets, per_line=9)
    source_file(directory, "Bidi.cpp", version, body)

    # --- What shaping needs: how letters join, and how Indic letters behave.
    joining = Property("U")
    for code, value in categories.runs_of(("Mn", "Me", "Cf")):
        joining.assign(code[0], code[1], "T")
    for fields in data_lines(ucd / "ArabicShaping.txt"):
        code = int(fields[0], 16)
        joining.values[code] = fields[2]
    joining_order = ["U", "R", "D", "L", "C", "T"]
    write_enum(enums, "JoiningType", joining_order)
    # (Syriac's own rules look at these two groups, and at nothing else that's a group.)
    joining_groups = Property("None")
    for fields in data_lines(ucd / "ArabicShaping.txt"):
        code = int(fields[0], 16)
        group = fields[3].replace(" ", "_")
        if group in ("ALAPH", "DALATH_RISH"):
            joining_groups.values[code] = group
    joining_group_order = ["None", "ALAPH", "DALATH_RISH"]
    write_enum(enums, "JoiningGroup", joining_group_order)
    syllabic = read_enumerated(ucd / "IndicSyllabicCategory.txt", "Other")
    syllabic_order = ["Other"] + sorted(set(syllabic.values) - {"Other"})
    write_enum(enums, "IndicSyllabicCategory", syllabic_order)
    positional = read_enumerated(ucd / "IndicPositionalCategory.txt", "NA")
    positional_order = ["NA"] + sorted(set(positional.values) - {"NA"})
    write_enum(enums, "IndicPositionalCategory", positional_order)
    vertical = read_enumerated(ucd / "VerticalOrientation.txt", "R")
    vertical_order = ["U", "R", "Tu", "Tr"]
    write_enum(enums, "VerticalOrientation", vertical_order)
    body = []
    write_enumerated_table(body, "joining_type", joining, joining_order)
    write_enumerated_table(body, "joining_group", joining_groups, joining_group_order)
    write_enumerated_table(body, "indic_syllabic_category", syllabic, syllabic_order)
    write_enumerated_table(body, "indic_positional_category", positional, positional_order)
    write_enumerated_table(body, "vertical_orientation", vertical, vertical_order)
    source_file(directory, "Shaping.cpp", version, body)

    # --- What it all comes to, for code to use.
    header = HEADER.format(version=version) + "#pragma once\n\n#include <LibBase/Types.h>\n\nnamespace Unicode {\n\n" + "\n".join(enums) + "\n}\n"
    (directory / "Enums.h").write_text(header, encoding="utf-8")
    names = "\n".join(f'    "{name}",' for name in script_order)
    (directory / "ScriptNames.cpp").write_text(
        HEADER.format(version=version)
        + "#include <LibUnicode/Tables.h>\n\nnamespace Unicode::Tables {\n\nextern char const* const script_names[];\nchar const* const script_names[] = {\n"
        + names
        + "\n};\n\n}\n",
        encoding="utf-8",
    )


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    directory = pathlib.Path(sys.argv[2])
    directory.mkdir(parents=True, exist_ok=True)
    generate(pathlib.Path(sys.argv[1]), directory)
    return 0


if __name__ == "__main__":
    sys.exit(main())
