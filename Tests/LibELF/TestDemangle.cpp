/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <LibELF/Demangle.h>
#include <LibTest/TestCase.h>

#define EXPECT_DEMANGLE(mangled, expected)                \
    do {                                                  \
        FixedStringBuilder<256> builder_;                 \
        EXPECT(ELF::demangle(mangled##_sv, builder_));    \
        EXPECT_EQ(builder_.view(), StringView(expected)); \
    } while (0)

#define EXPECT_NOT_DEMANGLED(mangled)                   \
    do {                                                \
        FixedStringBuilder<256> builder_;               \
        EXPECT(!ELF::demangle(mangled##_sv, builder_)); \
        EXPECT(builder_.view().is_empty());             \
    } while (0)

TEST_CASE(namespaced_functions)
{
    EXPECT_DEMANGLE("_ZN6Kernel5panicEN4Base10StringViewE", "Kernel::panic");
    EXPECT_DEMANGLE("_Z11kernel_initv", "kernel_init");
    EXPECT_DEMANGLE("_ZN4Base6detail6helperEii", "Base::detail::helper");
}

TEST_CASE(internal_linkage_and_anonymous_namespaces)
{
    EXPECT_DEMANGLE("_ZL12print_memoryv", "print_memory");
    EXPECT_DEMANGLE("_ZN4BootL12print_memoryERNS_7ConsoleE", "Boot::print_memory");
    EXPECT_DEMANGLE("_ZN12_GLOBAL__N_16helperEv", "(anonymous namespace)::helper");
}

TEST_CASE(constructors_destructors_and_operators)
{
    EXPECT_DEMANGLE("_ZN4Boot7ConsoleC2EPN4UEFI24SimpleTextOutputProtocolE", "Boot::Console::Console");
    EXPECT_DEMANGLE("_ZN4Boot7ConsoleD2Ev", "Boot::Console::~Console");
    EXPECT_DEMANGLE("_ZNK4Base10StringVieweqES0_", "Base::StringView::operator==");
    EXPECT_DEMANGLE("_ZN4Base5ArrayIiLm4EEixEm", "Base::Array<...>::operator[]");
}

TEST_CASE(template_arguments)
{
    EXPECT_DEMANGLE("_ZN4Base8OptionalIiE5valueEv", "Base::Optional<int>::value");
    EXPECT_DEMANGLE("_ZN4Base9FormatterINS_10StringViewEE6formatERNS_10FormatSinkERKNS_10FormatSpecES1_",
        "Base::Formatter<Base::StringView>::format");
    EXPECT_DEMANGLE("_ZN4Base4SpanIKhE5sliceEmm", "Base::Span<unsigned char const>::slice");
    EXPECT_DEMANGLE("_ZN4Base9format_toIJiEEEvRNS_10FormatSinkE", "Base::format_to<...>");
}

TEST_CASE(local_names_and_lambdas)
{
    EXPECT_DEMANGLE("_ZZN6Kernel4initEvENKUlvE_clEv", "Kernel::init()::{lambda#1}::operator()");
    EXPECT_DEMANGLE("_ZZN6Kernel4initEvENKUlvE0_clEv", "Kernel::init()::{lambda#2}::operator()");
}

TEST_CASE(special_names)
{
    EXPECT_DEMANGLE("_ZTVN4Boot7ConsoleE", "vtable for Boot::Console");
    EXPECT_DEMANGLE("_ZThn8_N4Boot7Console5writeEN4Base10StringViewE", "non-virtual thunk to Boot::Console::write");
    EXPECT_DEMANGLE("_ZN6Kernel5panicEv.cold", "Kernel::panic");
}

TEST_CASE(rejects_what_it_cannot_read)
{
    EXPECT_NOT_DEMANGLED("kernel_main");
    EXPECT_NOT_DEMANGLED("_ZN4Base");
    EXPECT_NOT_DEMANGLED("_Z3fo");
    EXPECT_NOT_DEMANGLED("_ZNS3_4nameEv");
    EXPECT_NOT_DEMANGLED("");
}
