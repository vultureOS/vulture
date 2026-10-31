/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/Arch/Power.h>
#include <Kernel/Debug/Panic.h>
#include <Kernel/Tasks/Processor.h>
#include <Kernel/Tests/KernelTest.h>
#include <Kernel/Time/Time.h>

namespace Kernel::Tests {

static TestCase const* s_first;
static TestCase* s_last;
static Atomic<u32> s_failures { 0 };
static u64 s_soak_seconds = 2;

u64 soak_seconds()
{
    return s_soak_seconds;
}

TestCase::TestCase(char const* suite, char const* name, TestFunction function)
    : m_suite(suite)
    , m_name(name)
    , m_function(function)
{
    // Kept in registration order, which is declaration order within a file.
    if (s_last)
        s_last->m_next = this;
    else
        s_first = this;
    s_last = this;
}

TestCase const* TestCase::first()
{
    return s_first;
}

void fail(char const* file, unsigned line, StringView message)
{
    s_failures.fetch_add(1);
    u32 processor = Arch::current_processor_pointer() ? Processor::current().id() : 0;
    dmesgln("ktest:   {}:{}: {} (processor {})", file, line, message, processor);
}

void run(StringView filter, u64 seconds)
{
    s_soak_seconds = seconds;
    // A crash in a test must end the run, not hang it.
    set_power_off_on_panic(true);
    dmesgln("ktest: running kernel tests{}{} on {} processors", filter.is_empty() ? ""_sv : " matching "_sv, filter, Processor::online_count());

    u32 passed = 0;
    u32 failed = 0;
    u64 run_start = Time::now();
    for (auto const* test = TestCase::first(); test; test = test->next()) {
        FixedStringBuilder<96> name;
        format_to(name, "{}.{}", test->suite(), test->name());
        if (!name.view().starts_with(filter))
            continue;

        dmesgln("ktest: [ RUN  ] {}", name.view());
        s_failures.store(0);
        u64 start = Time::now();
        test->run();
        u64 elapsed = Time::microseconds_from_ticks(Time::now() - start);
        if (s_failures.load() == 0) {
            ++passed;
            dmesgln("ktest: [  OK  ] {} ({} us)", name.view(), elapsed);
        } else {
            ++failed;
            dmesgln("ktest: [ FAIL ] {} ({} failed expectations)", name.view(), s_failures.load());
        }
    }

    u64 total = Time::microseconds_from_ticks(Time::now() - run_start) / 1000;
    dmesgln("ktest: {} passed, {} failed in {} ms", passed, failed, total);
    dmesgln("ktest: {}", failed == 0 && passed > 0 ? "PASS"_sv : "FAIL"_sv);
    Arch::power_off();
}

}
