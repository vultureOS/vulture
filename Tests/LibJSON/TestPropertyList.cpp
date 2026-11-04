/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibJSON/PropertyList.h>
#include <LibTest/TestCase.h>

namespace {

ReadonlyBytes bytes_of(StringView text)
{
    return { reinterpret_cast<u8 const*>(text.characters_without_null_termination()), text.length() };
}

constexpr auto s_xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Age</key>
	<integer>30</integer>
	<key>Name</key>
	<string>Alice &amp; Bob</string>
	<key>On</key>
	<true/>
	<key>Off</key>
	<false/>
	<key>Ratio</key>
	<real>1.5</real>
	<key>Tags</key>
	<array>
		<string>one</string>
		<integer>-2</integer>
	</array>
	<key>Inner</key>
	<dict/>
</dict>
</plist>
)"_sv;

void check_document(JSON::Value const& value)
{
    EXPECT(value.is_object());
    EXPECT_EQ(value.get_integer("Age"_sv).value_or(0), 30);
    EXPECT(value.get_string("Name"_sv).value_or(""_sv) == "Alice & Bob"_sv);
    EXPECT(value.get_boolean("On"_sv).value_or(false));
    EXPECT(!value.get_boolean("Off"_sv).value_or(true));
    auto const* tags = value.get("Tags"_sv);
    EXPECT(tags && tags->is_array() && tags->size() == 2);
    EXPECT(tags->at(0).as_string() == "one"_sv);
    EXPECT_EQ(tags->at(1).as_integer(), -2);
    auto const* inner = value.get("Inner"_sv);
    EXPECT(inner && inner->is_object() && inner->size() == 0);
}

}

TEST_CASE(reads_xml)
{
    check_document(MUST(JSON::parse_property_list(bytes_of(s_xml))));
}

TEST_CASE(reads_json_as_well)
{
    auto value = MUST(JSON::parse_property_list(bytes_of("{\"a\": [1, true]}"_sv)));
    EXPECT(value.get("a"_sv)->at(1).as_boolean());
}

TEST_CASE(xml_round_trip)
{
    auto first = MUST(JSON::parse_property_list(bytes_of(s_xml)));
    auto text = MUST(JSON::serialize_property_list_xml(first));
    check_document(MUST(JSON::parse_property_list(bytes_of(text.view()))));
}

TEST_CASE(binary_round_trip)
{
    auto first = MUST(JSON::parse_property_list(bytes_of(s_xml)));
    auto binary = MUST(JSON::serialize_property_list_binary(first));
    EXPECT(binary.size() > 40);
    EXPECT(__builtin_memcmp(binary.data(), "bplist00", 8) == 0);
    check_document(MUST(JSON::parse_property_list(binary.span())));
}

TEST_CASE(binary_of_more_than_fits_in_a_small_reference)
{
    // Past 256 values, references take more than a byte each; and past 14 in one collection, its length has to say so another way.
    auto array = JSON::Value::array();
    for (i64 i = 0; i < 1000; ++i)
        MUST(array.append(JSON::Value(i * 70000)));
    auto binary = MUST(JSON::serialize_property_list_binary(array));
    auto back = MUST(JSON::parse_property_list(binary.span()));
    EXPECT(back.is_array());
    EXPECT_EQ(back.size(), 1000u);
    EXPECT_EQ(back.at(999).as_integer(), 999 * 70000);
}

TEST_CASE(binary_of_text_that_isnt_ascii)
{
    auto object = JSON::Value::object();
    MUST(object.set("name"_sv, MUST(JSON::Value::string("caf\xc3\xa9 \xf0\x9f\x98\x80"_sv))));
    auto binary = MUST(JSON::serialize_property_list_binary(object));
    auto back = MUST(JSON::parse_property_list(binary.span()));
    EXPECT(back.get_string("name"_sv).value_or(""_sv) == "caf\xc3\xa9 \xf0\x9f\x98\x80"_sv);
}

TEST_CASE(for_people)
{
    auto value = MUST(JSON::parse_property_list(bytes_of("{\"b\": [1, \"x\"], \"a\": true}"_sv)));
    auto text = MUST(JSON::print_property_list_for_people(value));
    EXPECT(text.view() == "{\n  \"a\" => true\n  \"b\" => [\n    0 => 1\n    1 => \"x\"\n  ]\n}"_sv);
    auto described = MUST(JSON::describe_property_list(value));
    EXPECT(described.view() == "{\n    a = 1;\n    b =     (\n        1,\n        x\n    );\n}"_sv || described.view().contains("a = 1;"_sv));
}

TEST_CASE(whats_not_a_property_list)
{
    EXPECT(JSON::parse_property_list(bytes_of("hello"_sv)).is_error());
    EXPECT(JSON::parse_property_list(bytes_of("<plist><dict>"_sv)).is_error());
    EXPECT(JSON::parse_property_list(bytes_of("<plist><dict><key>a</key></dict></plist>"_sv)).is_error());
    EXPECT(JSON::parse_property_list(bytes_of("bplist00"_sv)).is_error());
    // A binary one, however it's cut short.
    auto binary = MUST(JSON::serialize_property_list_binary(MUST(JSON::parse_property_list(bytes_of(s_xml)))));
    for (size_t length = 8; length < binary.size(); ++length)
        (void)JSON::parse_property_list(binary.span().slice(0, length));
}
