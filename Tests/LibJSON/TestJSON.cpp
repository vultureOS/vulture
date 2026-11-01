/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibJSON/JSON.h>
#include <LibTest/TestCase.h>

namespace {

JSON::Value must_parse(StringView text)
{
    auto parsed = JSON::parse(text);
    VERIFY(!parsed.is_error());
    return parsed.release_value();
}

bool serializes_to(JSON::Value const& value, StringView expected, bool pretty = false)
{
    auto text = value.serialize(pretty);
    if (text.is_error() || text.value().view() != expected) {
        if (!text.is_error()) {
            Test::print("serialized: "_sv);
            Test::print(text.value().view());
            Test::print("\n"_sv);
        }
        return false;
    }
    return true;
}

}

TEST_CASE(literals_and_numbers)
{
    EXPECT(must_parse("null"_sv).is_null());
    EXPECT(must_parse(" true "_sv).as_boolean());
    EXPECT(!must_parse("false"_sv).as_boolean());
    EXPECT_EQ(must_parse("0"_sv).as_integer(), 0);
    EXPECT_EQ(must_parse("-42"_sv).as_integer(), -42);
    EXPECT_EQ(must_parse("9223372036854775807"_sv).as_integer(), static_cast<i64>(0x7fffffffffffffff));
    EXPECT_EQ(must_parse("-9223372036854775808"_sv).as_integer(), static_cast<i64>(0x8000000000000000));
    EXPECT(!must_parse("1.0"_sv).is_integer());
    EXPECT(must_parse("1.5"_sv).as_number() == 1.5);
    EXPECT(must_parse("-0.25"_sv).as_number() == -0.25);
    EXPECT(must_parse("1e3"_sv).as_number() == 1000.0);
    EXPECT(must_parse("2.5E-3"_sv).as_number() == 0.0025);
    EXPECT(must_parse("0.1"_sv).as_number() == 0.1);
    // Too big for an i64: a double.
    EXPECT(!must_parse("18446744073709551616"_sv).is_integer());
    EXPECT(must_parse("18446744073709551616"_sv).as_number() == 18446744073709551616.0);
}

TEST_CASE(strings_and_escapes)
{
    EXPECT(must_parse("\"plain\""_sv).as_string() == "plain"_sv);
    EXPECT(must_parse("\"a\\\"b\\\\c\\/d\\n\\t\""_sv).as_string() == "a\"b\\c/d\n\t"_sv);
    // é, and 😀 from a surrogate pair, as UTF-8; UTF-8 passes through.
    EXPECT(must_parse("\"caf\\u00e9\""_sv).as_string() == "caf\xc3\xa9"_sv);
    EXPECT(must_parse("\"\\ud83d\\ude00\""_sv).as_string() == "\xf0\x9f\x98\x80"_sv);
    EXPECT(must_parse("\"\xe2\x8c\x98 Command\""_sv).as_string() == "\xe2\x8c\x98 Command"_sv);
}

TEST_CASE(objects_and_arrays)
{
    auto value = must_parse(R"({
        "Label" : "com.vulture.getty",
        "ProgramArguments" : [ "/usr/libexec/getty", "std.9600", "console" ],
        "KeepAlive" : true,
        "ThrottleInterval" : 10,
        "Nested" : { "empty" : {}, "list" : [] }
    })"_sv);
    EXPECT(value.is_object());
    EXPECT_EQ(value.size(), 5u);
    EXPECT(value.key_at(0) == "Label"_sv);
    EXPECT(value.get_string("Label"_sv).value() == "com.vulture.getty"_sv);
    EXPECT(value.get_boolean("KeepAlive"_sv).value());
    EXPECT_EQ(value.get_integer("ThrottleInterval"_sv).value(), 10);
    EXPECT(!value.get_string("KeepAlive"_sv).has_value());
    EXPECT(!value.has("Missing"_sv));
    auto const* arguments = value.get_array("ProgramArguments"_sv);
    EXPECT(arguments != nullptr);
    EXPECT_EQ(arguments->size(), 3u);
    EXPECT(arguments->at(2).as_string() == "console"_sv);
    auto const* nested = value.get_object("Nested"_sv);
    EXPECT(nested && nested->get_object("empty"_sv) && nested->get_array("list"_sv));
}

TEST_CASE(serializing)
{
    auto value = JSON::Value::object();
    MUST(value.set("name"_sv, MUST(JSON::Value::string("vulture \"OS\"\n"_sv))));
    MUST(value.set("count"_sv, JSON::Value(3)));
    auto list = JSON::Value::array();
    MUST(list.append(JSON::Value(true)));
    MUST(list.append(JSON::Value()));
    MUST(list.append(JSON::Value(0.5)));
    MUST(value.set("list"_sv, move(list)));
    MUST(value.set("count"_sv, JSON::Value(4))); // Replaces, keeping the place.
    EXPECT(serializes_to(value, R"({"name":"vulture \"OS\"\n","count":4,"list":[true,null,0.5]})"_sv));
    EXPECT(serializes_to(value, "{\n    \"name\" : \"vulture \\\"OS\\\"\\n\",\n    \"count\" : 4,\n    \"list\" : [\n        true,\n        null,\n        0.5\n    ]\n}"_sv, true));
    EXPECT(value.remove("name"_sv));
    EXPECT(serializes_to(value, R"({"count":4,"list":[true,null,0.5]})"_sv));
    EXPECT(serializes_to(JSON::Value::object(), "{}"_sv, true));
}

TEST_CASE(numbers_written_back)
{
    EXPECT(serializes_to(JSON::Value(0.1), "0.1"_sv));
    EXPECT(serializes_to(JSON::Value(-2.5), "-2.5"_sv));
    EXPECT(serializes_to(JSON::Value(100.0), "100"_sv));
    EXPECT(serializes_to(JSON::Value(1e21), "1e+21"_sv));
    EXPECT(serializes_to(JSON::Value(1.5e-9), "1.5e-9"_sv));
    EXPECT(serializes_to(JSON::Value(0.001), "0.001"_sv));
    EXPECT(serializes_to(JSON::Value(static_cast<i64>(-7)), "-7"_sv));
    double third = 1.0 / 3.0;
    auto back = must_parse(MUST(JSON::Value(third).serialize()).view());
    EXPECT(back.as_number() == third);
}

TEST_CASE(round_trip)
{
    StringView text = R"({"a":[1,2,{"b":null}],"c":"d","e":false,"f":-1.25})"_sv;
    auto value = must_parse(text);
    EXPECT(serializes_to(value, text));
    EXPECT(must_parse(MUST(value.serialize(true)).view()) == value);
}

TEST_CASE(errors_say_where)
{
    JSON::ParseError error;
    EXPECT(JSON::parse("{\n  \"a\" : 1,\n  \"b\" 2\n}"_sv, &error).is_error());
    EXPECT_EQ(error.line, 3u);
    EXPECT_EQ(error.column, 7u);
    EXPECT(StringView(error.message) == "expected ':' after the member name"_sv);

    char const* const invalid[] = { "", "{", "[1,]", "[1 2]", "{\"a\":}", "\"open", "01", "1.", "-", "tru", "\"\\x\"", "\"\\ud800\"",
        "[1] [2]", "{'a':1}", "\"tab\there\"" };
    for (auto const* text : invalid) {
        if (!JSON::parse(StringView(text)).is_error()) {
            Test::print("accepted: "_sv);
            Test::print(StringView(text));
            Test::print("\n"_sv);
            EXPECT(false);
        }
    }
    // Deep nesting is refused, not a crash.
    static char deep[1200];
    for (size_t i = 0; i < 600; ++i) {
        deep[i] = '[';
        deep[600 + i] = ']';
    }
    EXPECT(JSON::parse(StringView(deep, 1200), &error).is_error());
    EXPECT(StringView(error.message) == "nested too deeply"_sv);
}
