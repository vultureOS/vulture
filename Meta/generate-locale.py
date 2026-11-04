#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""Makes LibLocale's tables from CLDR, the Unicode Consortium's data about the world's languages and places.

    Meta/generate-locale.py CLDR_COMMON_DIRECTORY Libraries/LibLocale/Generated

The tables are checked in, so that building needs no download: this is only run for a new version of CLDR, or for another locale. What's in them is only what
LibLocale uses, from only the locales in LOCALES.
"""

import pathlib
import re
import sys
import xml.etree.ElementTree as ET

# Each locale, and the files it's made of, from the most general to the most particular.
LOCALES = {
    "en_US": ["root", "en"],
    "en_GB": ["root", "en", "en_001", "en_GB"],
    "fr_FR": ["root", "fr"],
    "de_DE": ["root", "de"],
    "es_ES": ["root", "es"],
    "it_IT": ["root", "it"],
    "pt_BR": ["root", "pt"],
    "nl_NL": ["root", "nl"],
    "sv_SE": ["root", "sv"],
    "pl_PL": ["root", "pl"],
    "tr_TR": ["root", "tr"],
    "ru_RU": ["root", "ru"],
    "ar_EG": ["root", "ar", "ar_EG"],
    "he_IL": ["root", "he"],
    "hi_IN": ["root", "hi"],
    "ja_JP": ["root", "ja"],
    "ko_KR": ["root", "ko"],
    "zh_CN": ["root", "zh", "zh_Hans"],
}

CURRENCIES = ["USD", "EUR", "GBP", "JPY", "CNY", "KRW", "INR", "RUB", "BRL", "CAD", "AUD", "CHF", "MXN", "SEK", "PLN", "TRY", "ILS", "EGP"]
# What the Language & Region pane offers, by name.
LANGUAGES = ["ar", "de", "en", "es", "fr", "he", "hi", "it", "ja", "ko", "nl", "pl", "pt", "ru", "sv", "tr", "zh"]
REGIONS = ["BR", "CN", "DE", "EG", "ES", "FR", "GB", "IL", "IN", "IT", "JP", "KR", "NL", "PL", "RU", "SE", "TR", "US"]

HEADER = """/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Made by Meta/generate-locale.py from CLDR {version}. Don't edit it.

"""


# What a Mac says where CLDR 46 says something else, as Tests/Parity/locale-oracle.mm found on macOS 26.4. Apple's own data is its own, and a Mac is what's being matched: these are
# facts about what it prints, measured from outside. Lists go by the place in them that's different: 0 is full, 1 long, 2 medium, 3 short; months and days from 0.
NNBSP = "\u202f"
NBSP = "\u00a0"
MEASURED = {
    "en_US": {"date_at_time_formats": {2: "{1} 'at' {0}"}},
    "en_GB": {"date_at_time_formats": {2: "{1} 'at' {0}"}, "months_format_abbreviated": {8: "Sep"}},
    "fr_FR": {"date_at_time_formats": {2: "{1} '\u00e0' {0}"}},
    "es_ES": {"date_at_time_formats": {1: "{1} 'a' 'las' {0}"}},
    "nl_NL": {"am": "AM", "pm": "PM"},
    "sv_SE": {
        "time_formats": {0: "'kl'. HH:mm:ss zzzz"},
        "date_at_time_formats": {0: "{1} {0}", 1: "{1} {0}"},
    },
    "pl_PL": {"date_at_time_formats": {0: "{1} 'o' {0}", 1: "{1} 'o' {0}", 2: "{1} 'o' {0}"}},
    "ru_RU": {
        "date_formats": {2: f"d MMMM y{NNBSP}'\u0433'."},
        "months_format_abbreviated": {2: "\u043c\u0430\u0440\u0442\u0430", 5: "\u0438\u044e\u043d\u044f", 6: "\u0438\u044e\u043b\u044f"},
        "days_format_abbreviated": {0: "\u0412\u0441", 1: "\u041f\u043d", 2: "\u0412\u0442", 3: "\u0421\u0440", 4: "\u0427\u0442", 5: "\u041f\u0442", 6: "\u0421\u0431"},
    },
    "ar_EG": {
        "date_formats": {0: "EEEE\u060c d MMMM\u060c y", 1: "d MMMM\u060c y"},
        "time_formats": {0: f"h:mm:ss{NBSP}a zzzz", 1: f"h:mm:ss{NBSP}a z", 2: f"h:mm:ss{NBSP}a", 3: f"h:mm{NBSP}a"},
        "date_at_time_formats": {0: "{1}\u060c {0}", 1: "{1}\u060c {0}"},
        "days_format_abbreviated": {
            0: "\u0623\u062d\u062f",
            1: "\u0627\u062b\u0646\u064a\u0646",
            2: "\u062b\u0644\u0627\u062b\u0627\u0621",
            3: "\u0623\u0631\u0628\u0639\u0627\u0621",
            4: "\u062e\u0645\u064a\u0633",
            5: "\u062c\u0645\u0639\u0629",
            6: "\u0633\u0628\u062a",
        },
    },
    "he_IL": {"am": "\u05dc\u05e4\u05f3", "pm": "\u05d0\u05d7\u05f3"},
    "hi_IN": {
        "am": "\u092a\u0942",
        "pm": "\u0905",
        "time_formats": {0: "a h:mm:ss zzzz", 1: "a h:mm:ss z", 2: "a h:mm:ss", 3: "a h:mm"},
        "date_at_time_formats": {0: "{1}, {0}", 1: "{1}, {0}"},
    },
    "ja_JP": {"date_formats": {0: "y\u5e74M\u6708d\u65e5 EEEE"}, "symbol_JPY": "\u00a5"},
    "ko_KR": {"date_formats": {3: "y. M. d."}},
    "zh_CN": {"date_formats": {0: "y\u5e74M\u6708d\u65e5 EEEE"}, "first_day_of_week": 0},
}


# For the menu bar: what the Mac's own formatter gives for "weekday, month, day, hour, minute" (NSDateFormatter's dateFormatFromTemplate), with what's only there to join the date to the
# time left out, as the menu bar has two spaces for that.
CLOCK_FORMATS = {
    "en_US": "EEE MMM d  h:mm a",
    "en_GB": "EEE d MMM  HH:mm",
    "fr_FR": "EEE d MMM  HH:mm",
    "de_DE": "EEE d. MMM  HH:mm",
    "es_ES": "EEE d MMM  H:mm",
    "it_IT": "EEE d MMM  HH:mm",
    "pt_BR": "EEE, d 'de' MMM  HH:mm",
    "nl_NL": "EEE d MMM  HH:mm",
    "sv_SE": "EEE d MMM  HH:mm",
    "pl_PL": "EEE d MMM  HH:mm",
    "tr_TR": "d MMM EEE  HH:mm",
    "ru_RU": "ccc, d MMM  HH:mm",
    "ar_EG": f"EEE\u060c d MMM  h:mm{NBSP}a",
    "he_IL": "EEE, d \u05d1MMM  H:mm",
    "hi_IN": "EEE, d MMM  a h:mm",
    "ja_JP": "M\u6708d\u65e5(EEE)  H:mm",
    "ko_KR": "MMM d\uc77c (EEE)  a h:mm",
    "zh_CN": "M\u6708d\u65e5 EEE  HH:mm",
}


def measured_value(identifier, field, value):
    return MEASURED.get(identifier, {}).get(field, value)


def measured_list(identifier, field, values):
    values = list(values)
    for place, value in MEASURED.get(identifier, {}).get(field, {}).items():
        values[place] = value
    return values


def day_period(resolved, which):
    """Abbreviated if it has one, or else its wide one, before what's further up: 'a' in Korean is the word itself."""
    for tree in resolved.trees:
        for width in ("abbreviated", "wide"):
            element = find_good(tree, f"dates/calendars/calendar[@type='gregorian']/dayPeriods/dayPeriodContext[@type='format']/dayPeriodWidth[@type='{width}']/dayPeriod[@type='{which}']")
            if element is not None and element.text and element.text != "↑↑↑":
                return element.text
    return None


def good_enough(element):
    # As CLDR's own releases have it: nothing that's only a suggestion.
    return element.get("draft") not in ("unconfirmed", "provisional") and element.get("alt") is None


def find_good(parent, path):
    if parent is None:
        return None
    for element in parent.findall(path):
        if good_enough(element):
            return element
    return None


class Resolved:
    """A locale's files, most particular first, for looking something up in each until it's there."""

    def __init__(self, directory, names):
        self.trees = [ET.parse(directory / "main" / f"{name}.xml").getroot() for name in reversed(names)]

    def text(self, path):
        for tree in self.trees:
            element = find_good(tree, path)
            if element is not None and element.text and element.text != "↑↑↑":
                return element.text
        return None

    def number_pattern(self, kind, system):
        """A pattern from the one of its kind with no "type": those with one are the short forms, like 1K."""
        for tree in self.trees:
            for formats in tree.findall(f"numbers/{kind}Formats"):
                if formats.get("numberSystem") not in (system, None):
                    continue
                for length in formats.findall(f"{kind}FormatLength"):
                    if length.get("type") is not None:
                        continue
                    for entry in length.findall(f"{kind}Format"):
                        if entry.get("type") not in (None, "standard") or entry.get("alt") is not None:
                            continue
                        pattern = find_good(entry, "pattern")
                        if pattern is not None and pattern.text and pattern.text != "↑↑↑":
                            return pattern.text
        return None

    def gregorian(self, rest):
        return self.text(f"dates/calendars/calendar[@type='gregorian']/{rest}")


def names_in(resolved, kind, context, width, key, keys):
    """Months or days, with what CLDR's own root says about what stands in for what."""
    root_rules = {
        "months": {("format", "abbreviated"): ("format", "wide"), ("format", "narrow"): ("stand-alone", "narrow"), ("stand-alone", "abbreviated"): ("format", "abbreviated"), ("stand-alone", "wide"): ("format", "wide")},
        "days": {("format", "abbreviated"): ("format", "wide"), ("format", "narrow"): ("stand-alone", "narrow"), ("format", "short"): ("format", "abbreviated"), ("stand-alone", "abbreviated"): ("format", "abbreviated"), ("stand-alone", "short"): ("format", "short"), ("stand-alone", "wide"): ("format", "wide")},
    }[kind]
    single = kind[:-1]
    result = []
    for wanted in keys:
        current = (context, width)
        found = None
        seen = set()
        while found is None and current not in seen:
            seen.add(current)
            found = resolved.gregorian(f"{kind}/{single}Context[@type='{current[0]}']/{single}Width[@type='{current[1]}']/{key}[@type='{wanted}']")
            if found is None:
                current = root_rules.get(current, current)
        result.append(found or "")
    return result


def cxx(text):
    if text is None:
        return "nullptr"
    out = ['"']
    previous_was_escape = False
    for byte in text.encode("utf-8"):
        character = chr(byte)
        if byte == 0x22 or byte == 0x5c:
            out.append("\\" + character)
            previous_was_escape = False
        elif 0x20 <= byte < 0x7f:
            # (After a \x, what looks like more of it has to be kept apart.)
            if previous_was_escape and character in "0123456789abcdefABCDEF":
                out.append('" "')
            out.append(character)
            previous_was_escape = False
        else:
            out.append(f"\\x{byte:02x}")
            previous_was_escape = True
    out.append('"')
    return "".join(out)


def cxx_list(values):
    return "{ " + ", ".join(cxx(value) for value in values) + " }"


def currency_digits(directory):
    tree = ET.parse(directory / "supplemental" / "supplementalData.xml").getroot()
    digits = {}
    for info in tree.findall("currencyData/fractions/info"):
        digits[info.get("iso4217")] = int(info.get("digits"))
    default = digits.pop("DEFAULT", 2)
    return {code: digits.get(code, default) for code in CURRENCIES}


def region_currency(directory, region):
    tree = ET.parse(directory / "supplemental" / "supplementalData.xml").getroot()
    for currency in tree.findall(f"currencyData/region[@iso3166='{region}']/currency"):
        if currency.get("to") is None and currency.get("tender") != "false":
            return currency.get("iso4217")
    return "USD"


def first_day_of_week(directory, region):
    tree = ET.parse(directory / "supplemental" / "supplementalData.xml").getroot()
    order = {"sun": 0, "mon": 1, "tue": 2, "wed": 3, "thu": 4, "fri": 5, "sat": 6}
    fallback = 1
    for entry in tree.findall("weekData/firstDay"):
        if entry.get("alt") is not None:
            continue
        territories = entry.get("territories").split()
        if region in territories:
            return order[entry.get("day")]
        if "001" in territories:
            fallback = order[entry.get("day")]
    return fallback


def numbering_digits(directory, system):
    tree = ET.parse(directory / "supplemental" / "numberingSystems.xml").getroot()
    for entry in tree.iter("numberingSystem"):
        if entry.get("id") == system and entry.get("digits"):
            return entry.get("digits")
    return "0123456789"


def generate(directory, out_directory):
    version = "46"
    locales = []
    digits_of_currency = currency_digits(directory)
    for identifier, files in LOCALES.items():
        resolved = Resolved(directory, files)
        region = identifier.split("_")[1]
        # (CLDR has these under "alt" for some, for what's used in everyday writing, as against the other. Arabic is one.)
        system = "latn"
        for tree in resolved.trees:
            found = [element for element in tree.findall("numbers/defaultNumberingSystem") if element.get("alt") is None]
            if found:
                system = found[0].text
                break

        def number_text(path):
            return resolved.text(path.replace("{system}", system)) or resolved.text(path.replace("{system}", "latn"))

        def number_symbol(name):
            return number_text(f"numbers/symbols[@numberSystem='{{system}}']/{name}")

        def pattern_of(kind):
            return resolved.number_pattern(kind, system) or resolved.number_pattern(kind, "latn")

        months = range(1, 13)
        days = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]
        data = []
        data.append(f'    .identifier = {cxx(identifier)},')
        language = identifier.split("_")[0]
        data.append(f'    .language = {cxx(language)},')
        data.append(f'    .region = {cxx(region)},')
        order = resolved.text("layout/orientation/characterOrder") or "left-to-right"
        data.append(f'    .is_right_to_left = {"true" if order == "right-to-left" else "false"},')
        data.append(f'    .first_day_of_week = {MEASURED.get(identifier, {}).get("first_day_of_week", first_day_of_week(directory, region))},')
        data.append(f'    .default_currency = {cxx(region_currency(directory, region))},')
        data.append(f'    .digits = {cxx(numbering_digits(directory, system))},')
        minimum = resolved.text("numbers/minimumGroupingDigits") or "1"
        data.append(f'    .minimum_grouping_digits = {minimum},')
        data.append(f'    .decimal_separator = {cxx(number_symbol("decimal"))},')
        data.append(f'    .group_separator = {cxx(number_symbol("group"))},')
        data.append(f'    .percent_sign = {cxx(number_symbol("percentSign"))},')
        data.append(f'    .minus_sign = {cxx(number_symbol("minusSign"))},')
        data.append(f'    .plus_sign = {cxx(number_symbol("plusSign"))},')
        data.append(f'    .infinity = {cxx(number_symbol("infinity"))},')
        data.append(f'    .not_a_number = {cxx(number_symbol("nan"))},')
        data.append(f'    .decimal_pattern = {cxx(pattern_of("decimal"))},')
        data.append(f'    .percent_pattern = {cxx(pattern_of("percent"))},')
        data.append(f'    .currency_pattern = {cxx(pattern_of("currency"))},')
        spacing = number_text("numbers/currencyFormats[@numberSystem='{system}']/currencySpacing/beforeCurrency/insertBetween")
        data.append(f'    .currency_spacing = {cxx(spacing)},')
        symbols = []
        for code in CURRENCIES:
            symbols.append(MEASURED.get(identifier, {}).get(f"symbol_{code}") or resolved.text(f"numbers/currencies/currency[@type='{code}']/symbol") or code)
        data.append(f'    .currency_symbols = {cxx_list(symbols)},')

        for context, context_name in (("format", "format"), ("stand-alone", "standalone")):
            for width in ("wide", "abbreviated", "narrow"):
                values = measured_list(identifier, f"months_{context_name}_{width}", names_in(resolved, "months", context, width, "month", [str(month) for month in months]))
                data.append(f'    .months_{context_name}_{width} = {cxx_list(values)},')
            for width in ("wide", "abbreviated", "short", "narrow"):
                values = measured_list(identifier, f"days_{context_name}_{width}", names_in(resolved, "days", context, width, "day", days))
                data.append(f'    .days_{context_name}_{width} = {cxx_list(values)},')
        data.append(f'    .am = {cxx(measured_value(identifier, "am", day_period(resolved, "am") or "AM"))},')
        data.append(f'    .pm = {cxx(measured_value(identifier, "pm", day_period(resolved, "pm") or "PM"))},')
        data.append(f'''    .era_ad = {cxx(resolved.gregorian("eras/eraAbbr/era[@type='1']") or "AD")},''')
        lengths = ("full", "long", "medium", "short")
        data.append(f'''    .date_formats = {cxx_list(measured_list(identifier, "date_formats", [resolved.gregorian(f"dateFormats/dateFormatLength[@type='{length}']/dateFormat/pattern") for length in lengths]))},''')
        data.append(f'''    .time_formats = {cxx_list(measured_list(identifier, "time_formats", [resolved.gregorian(f"timeFormats/timeFormatLength[@type='{length}']/timeFormat/pattern") for length in lengths]))},''')
        data.append(f'''    .date_time_formats = {cxx_list(measured_list(identifier, "date_time_formats", [resolved.gregorian(f"dateTimeFormats/dateTimeFormatLength[@type='{length}']/dateTimeFormat[@type='standard']/pattern") or resolved.gregorian(f"dateTimeFormats/dateTimeFormatLength[@type='{length}']/dateTimeFormat/pattern") for length in lengths]))},''')
        data.append(f'''    .date_at_time_formats = {cxx_list(measured_list(identifier, "date_at_time_formats", [resolved.gregorian(f"dateTimeFormats/dateTimeFormatLength[@type='{length}']/dateTimeFormat[@type='atTime']/pattern") for length in lengths]))},''')
        data.append(f'    .clock_format = {cxx(CLOCK_FORMATS[identifier])},')
        data.append(f'''    .gmt_format = {cxx(resolved.text("dates/timeZoneNames/gmtFormat") or "GMT{0}")},''')
        data.append(f'''    .gmt_zero_format = {cxx(resolved.text("dates/timeZoneNames/gmtZeroFormat") or "GMT")},''')
        data.append(f'''    .hour_format = {cxx(resolved.text("dates/timeZoneNames/hourFormat") or "+HH:mm;-HH:mm")},''')
        data.append(f'''    .utc_short_name = {cxx(resolved.text("dates/timeZoneNames/zone[@type='Etc/UTC']/short/standard") or "UTC")},''')
        data.append(f'''    .utc_long_name = {cxx(resolved.text("dates/timeZoneNames/zone[@type='Etc/UTC']/long/standard") or "Coordinated Universal Time")},''')
        data.append(f'''    .language_names = {cxx_list(resolved.text(f"localeDisplayNames/languages/language[@type='{code}']") or code for code in LANGUAGES)},''')
        data.append(f'''    .region_names = {cxx_list(resolved.text(f"localeDisplayNames/territories/territory[@type='{code}']") or code for code in REGIONS)},''')
        locales.append("{\n" + "\n".join(data) + "\n}")

    text = HEADER.format(version=version) + "#include <LibLocale/Locale.h>\n\nnamespace Locale {\n\n"
    text += "char const* const currency_codes[currency_count] = " + cxx_list(CURRENCIES) + ";\n"
    text += "int const currency_decimal_places[currency_count] = { " + ", ".join(str(digits_of_currency[code]) for code in CURRENCIES) + " };\n"
    text += "char const* const language_codes[language_name_count] = " + cxx_list(LANGUAGES) + ";\n"
    text += "char const* const region_codes[region_name_count] = " + cxx_list(REGIONS) + ";\n\n"
    text += f"size_t const locale_count = {len(locales)};\n"
    text += "LocaleData const locales[] = {\n" + ",\n".join(locales) + "\n};\n\n}\n"
    (out_directory / "Locales.cpp").write_text(text, encoding="utf-8")

    sizes = HEADER.format(version=version) + "#pragma once\n\n#include <LibBase/Types.h>\n\nnamespace Locale {\n\n"
    sizes += f"constexpr size_t currency_count = {len(CURRENCIES)};\nconstexpr size_t language_name_count = {len(LANGUAGES)};\nconstexpr size_t region_name_count = {len(REGIONS)};\n\n}}\n"
    (out_directory / "Sizes.h").write_text(sizes, encoding="utf-8")

    generate_collation(directory, out_directory, version)


def parse_elements(text):
    """[.2380.0020.0002][*0209.0020.0002]: each with whether it's variable."""
    elements = []
    for variable, primary, secondary, tertiary in re.findall(r"\[([.*])([0-9A-F]+)\.([0-9A-F]+)\.([0-9A-F]+)\]", text):
        elements.append((int(primary, 16), int(secondary, 16), int(tertiary, 16), variable == "*"))
    return elements


def pack(element):
    primary, secondary, tertiary, variable = element
    assert primary < 0x10000 and secondary < 0x400 and tertiary < 0x20, element
    return (primary << 16) | (secondary << 6) | (tertiary << 1) | (1 if variable else 0)


# What the rules in a CLDR tailoring come to, for the rules that the ones here use.
TAILORED = {"es": "es", "sv": "sv", "tr": "tr", "pl": "pl", "de": None, "fr": None, "it": None, "pt": None, "nl": None, "en": None}


def strftime_from_pattern(pattern):
    """A CLDR date or time pattern, as what strftime() would say instead."""
    out = ""
    i = 0
    quoted = False
    while i < len(pattern):
        c = pattern[i]
        if c == "'":
            quoted = not quoted
            i += 1
            continue
        if quoted or not c.isalpha():
            out += c
            i += 1
            continue
        count = 1
        while i + count < len(pattern) and pattern[i + count] == c:
            count += 1
        i += count
        out += {
            "y": "%Y", "M": "%m", "L": "%m", "d": "%d", "H": "%H", "h": "%I", "K": "%I", "k": "%H", "m": "%M", "s": "%S", "a": "%p", "b": "%p", "B": "%p",
        }.get(c, "")
    return out.strip()


def write_posix_files(directory, base):
    """What setlocale() reads: as a Mac keeps them, a directory for each, with a line for each thing. From the same data as everything else."""
    for identifier, files in LOCALES.items():
        resolved = Resolved(directory, files)
        target = base / f"{identifier}.UTF-8"
        target.mkdir(parents=True, exist_ok=True)
        region = identifier.split("_")[1]
        system = "latn"

        def symbol(name):
            for tree in resolved.trees:
                for symbols in tree.findall("numbers/symbols"):
                    if symbols.get("numberSystem") not in ("latn", None):
                        continue
                    element = find_good(symbols, name)
                    if element is not None and element.text and element.text != "↑↑↑":
                        return element.text
            return None

        decimal = symbol("decimal") or "."
        group = symbol("group") or ","
        pattern = resolved.number_pattern("decimal", system) or "#,##0.###"
        integer = pattern.split(";")[0].split(".")[0]
        sizes = [len(part) for part in integer.split(",")[1:]]
        grouping = ";".join(str(size) for size in reversed(sizes))
        (target / "LC_NUMERIC").write_text(f"{decimal}\n{group}\n{grouping}\n", encoding="utf-8")

        # Money.
        code = region_currency(directory, region)
        digits = currency_digits(directory).get(code, 2)
        money_symbol = MEASURED.get(identifier, {}).get(f"symbol_{code}") or resolved.text(f"numbers/currencies/currency[@type='{code}']/symbol") or code
        money = (resolved.number_pattern("currency", system) or "¤#,##0.00").split(";")[0].replace("\u200f", "").replace("\u200e", "")
        precedes = 1 if money.index("¤") < money.index("0") else 0
        separated = 1 if ("\u00a0" in money or " " in money) else 0
        minus = symbol("minusSign") or "-"
        (target / "LC_MONETARY").write_text(
            f"{code} \n{money_symbol}\n{decimal}\n{group}\n{grouping}\n\n{minus}\n{digits}\n{digits}\n{precedes}\n{separated}\n{precedes}\n{separated}\n1\n1\n", encoding="utf-8"
        )

        # Dates and times.
        months = [str(month) for month in range(1, 13)]
        days = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]

        def names(kind, key, keys, context, width, field):
            return measured_list(identifier, field, names_in(resolved, kind, context, width, key, keys))

        lines = []
        lines += names("months", "month", months, "format", "abbreviated", "months_format_abbreviated")
        lines += names("months", "month", months, "format", "wide", "months_format_wide")
        lines += names("days", "day", days, "format", "abbreviated", "days_format_abbreviated")
        lines += names("days", "day", days, "format", "wide", "days_format_wide")
        time_medium = measured_list(identifier, "time_formats", [resolved.gregorian(f"timeFormats/timeFormatLength[@type='{length}']/timeFormat/pattern") for length in ("full", "long", "medium", "short")])[2]
        date_short = measured_list(identifier, "date_formats", [resolved.gregorian(f"dateFormats/dateFormatLength[@type='{length}']/dateFormat/pattern") for length in ("full", "long", "medium", "short")])[3]
        twelve_hours = "a" in time_medium
        # (Always the 24-hour clock: %r is for the other.)
        time_format = "%H:%M:%S"
        date_format = strftime_from_pattern(date_short)
        am = measured_value(identifier, "am", day_period(resolved, "am") or "AM") if twelve_hours else ""
        pm = measured_value(identifier, "pm", day_period(resolved, "pm") or "PM") if twelve_hours else ""
        first_is_month = date_format.find("%m") < date_format.find("%d")
        lines += [time_format, date_format, "%a %e %b %X %Y" if not first_is_month else "%a %b %e %X %Y", am, pm, "%a %e %b %Y %X %Z" if not first_is_month else "%a %b %e %X %Z %Y"]
        lines += names("months", "month", months, "stand-alone", "wide", "months_standalone_wide")
        lines += ["md" if first_is_month else "dm", "%I:%M:%S %p" if twelve_hours else ""]
        (target / "LC_TIME").write_text("\n".join(lines) + "\n", encoding="utf-8")


def generate_collation(directory, out_directory, version):
    singles = {}
    contractions = {}
    with open(directory / "uca" / "allkeys_CLDR.txt", encoding="utf-8") as file:
        for raw in file:
            line = raw.split("#", 1)[0].strip()
            if not line or line.startswith("@"):
                continue
            key, elements = line.split(";", 1)
            codes = [int(part, 16) for part in key.split()]
            packed = [pack(element) for element in parse_elements(elements)]
            if len(codes) == 1:
                singles[codes[0]] = packed
            else:
                contractions[tuple(codes)] = packed

    elements = []
    places = {}

    def place_of(packed):
        key = tuple(packed)
        if key not in places:
            places[key] = len(elements)
            elements.extend(packed)
        return places[key]

    # Those with one element, the great majority, have it right there, with no table to go through: the entry's second word has a flag in the bit that a
    # packed element can't have set along with being ignorable; rather than be clever, there are two tables.
    one = []
    many = []
    for code in sorted(singles):
        packed = singles[code]
        if len(packed) == 1:
            one.extend((code, packed[0]))
        else:
            many.extend((code, (place_of(packed) << 8) | len(packed)))
    contraction_table = []
    longest = 0
    for codes in sorted(contractions):
        packed = contractions[codes]
        longest = max(longest, len(codes))
        assert len(codes) <= 3
        contraction_table.extend((codes[0], codes[1], codes[2] if len(codes) > 2 else 0, (place_of(packed) << 8) | len(packed)))

    def table(name, values, per_line=8):
        lines = [f"extern u32 const {name}[];", f"u32 const {name}[] = {{"]
        for i in range(0, len(values), per_line):
            lines.append("    " + ", ".join(f"{value:#x}" for value in values[i : i + per_line]) + ",")
        lines.append("};")
        lines.append(f"extern size_t const {name}_count;")
        lines.append(f"size_t const {name}_count = {len(values)};")
        lines.append("")
        return lines

    body = table("single_elements", one) + table("expansions", many) + table("contraction_entries", contraction_table) + table("collation_elements", elements)
    (out_directory / "CollationRoot.cpp").write_text(HEADER.format(version=version) + "#include <LibLocale/CollationTables.h>\n\nnamespace Locale::Tables {\n\n" + "\n".join(body) + "\n}\n", encoding="utf-8")

    # The tailorings, as the rules CLDR has them: they're small, and applied when one's asked for.
    rules = {}
    for language in ("es", "sv", "tr", "pl", "ru", "ar", "he", "hi", "ko", "ja", "zh"):
        path = directory / "collation" / f"{language}.xml"
        if not path.exists():
            continue
        tree = ET.parse(path).getroot()
        for collation in tree.findall("collations/collation"):
            if collation.get("type") == "standard" and good_enough(collation):
                text = collation.find("cr").text if collation.find("cr") is not None else ""
                # Only those small enough to be for a few letters.
                if len(text) < 2000:
                    rules[language] = "\n".join(line.strip() for line in text.splitlines() if line.strip() and not line.strip().startswith("#"))
    lines = ["TailoringRules const tailoring_rules[] = {"]
    for language in sorted(rules):
        lines.append(f"    {{ {cxx(language)}, {cxx(rules[language])} }},")
    lines.append("};")
    lines.append(f"size_t const tailoring_rules_count = {len(rules)};")
    (out_directory / "CollationTailoring.cpp").write_text(HEADER.format(version=version) + "#include <LibLocale/CollationTables.h>\n\nnamespace Locale::Tables {\n\n" + "\n".join(lines) + "\n\n}\n", encoding="utf-8")


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    out_directory = pathlib.Path(sys.argv[2])
    out_directory.mkdir(parents=True, exist_ok=True)
    generate(pathlib.Path(sys.argv[1]), out_directory)
    write_posix_files(pathlib.Path(sys.argv[1]), pathlib.Path(__file__).resolve().parent.parent / "Base" / "usr" / "share" / "locale")
    return 0


if __name__ == "__main__":
    sys.exit(main())
