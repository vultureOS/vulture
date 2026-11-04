/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Common.h"
#include <Kernel/API/POSIX/sysctl.h>
#include <pwd.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

// top [-l samples] [-n processes] [-s delay] [-o cpu|mem|pid|command]: processes, by how much they're using. Keeps redrawing
// on a terminal until it's stopped. With -l, that many samples, one after another, for scripts.

namespace {

struct Sample {
    vulture_process_info* processes { nullptr };
    size_t count { 0 };
    timespec taken {};
};

bool take_sample(Sample& sample)
{
    size_t length = 0;
    if (sysctlbyname("kern.proc.all", nullptr, &length, nullptr, 0) != 0)
        return false;
    // A little more, as it may have grown.
    length += 16 * sizeof(vulture_process_info);
    free(sample.processes);
    sample.processes = static_cast<vulture_process_info*>(malloc(length));
    if (!sample.processes || sysctlbyname("kern.proc.all", sample.processes, &length, nullptr, 0) != 0)
        return false;
    sample.count = length / sizeof(vulture_process_info);
    clock_gettime(CLOCK_MONOTONIC, &sample.taken);
    return true;
}

double seconds_between(timespec const& a, timespec const& b)
{
    return static_cast<double>(b.tv_sec - a.tv_sec) + static_cast<double>(b.tv_nsec - a.tv_nsec) / 1e9;
}

struct Row {
    vulture_process_info const* process;
    double cpu;
};

void format_size(unsigned long long bytes, char* out, size_t size)
{
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(out, size, "%lluG", bytes / (1024ull * 1024 * 1024));
    else if (bytes >= 1024ull * 1024)
        snprintf(out, size, "%lluM", bytes / (1024ull * 1024));
    else
        snprintf(out, size, "%lluK", bytes / 1024);
}

void format_time(unsigned long long nanoseconds, char* out, size_t size)
{
    unsigned long long const hundredths = nanoseconds / 10'000'000ull;
    unsigned long long const seconds = hundredths / 100;
    if (seconds >= 3600)
        snprintf(out, size, "%02llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else
        snprintf(out, size, "%02llu:%02llu.%02llu", seconds / 60, seconds % 60, hundredths % 100);
}

char const* state_name(char state)
{
    switch (state) {
    case 'R':
        return "running";
    case 'S':
        return "sleeping";
    case 'T':
        return "stopped";
    case 'Z':
        return "zombie";
    default:
        return "unknown";
    }
}

enum class Order {
    CPU,
    Memory,
    Process,
    Command,
};

void print_screen(Sample const& now, Sample const* before, int limit, Order order)
{
    auto* rows = static_cast<Row*>(malloc(sizeof(Row) * (now.count + 1)));
    size_t running = 0, sleeping = 0, stopped = 0, zombie = 0;
    unsigned long long threads = 0;
    double const elapsed = before ? seconds_between(before->taken, now.taken) : 0;
    for (size_t i = 0; i < now.count; ++i) {
        auto const& process = now.processes[i];
        switch (process.state) {
        case 'R':
            ++running;
            break;
        case 'T':
            ++stopped;
            break;
        case 'Z':
            ++zombie;
            break;
        default:
            ++sleeping;
            break;
        }
        threads += process.threads;
        double cpu = 0;
        if (before && elapsed > 0) {
            for (size_t j = 0; j < before->count; ++j) {
                if (before->processes[j].pid == process.pid) {
                    unsigned long long const was = before->processes[j].cpu_nanoseconds;
                    if (process.cpu_nanoseconds >= was)
                        cpu = static_cast<double>(process.cpu_nanoseconds - was) / (elapsed * 1e9) * 100.0;
                    break;
                }
            }
        }
        rows[i] = { &process, cpu };
    }
    for (size_t i = 1; i < now.count; ++i) {
        Row const held = rows[i];
        size_t j = i;
        auto goes_before = [&](Row const& a, Row const& b) {
            switch (order) {
            case Order::CPU:
                return a.cpu != b.cpu ? a.cpu > b.cpu : a.process->pid > b.process->pid;
            case Order::Memory:
                return a.process->resident_bytes > b.process->resident_bytes;
            case Order::Process:
                return a.process->pid > b.process->pid;
            case Order::Command:
                return strcmp(a.process->name, b.process->name) < 0;
            }
            return false;
        };
        while (j > 0 && goes_before(held, rows[j - 1])) {
            rows[j] = rows[j - 1];
            --j;
        }
        rows[j] = held;
    }

    printf("Processes: %zu total, %zu running, %zu sleeping", now.count, running, sleeping);
    if (stopped)
        printf(", %zu stopped", stopped);
    if (zombie)
        printf(", %zu zombie", zombie);
    printf(", %llu threads\n", threads);
    time_t const wall = time(nullptr);
    tm parts {};
    localtime_r(&wall, &parts);
    char date[64];
    strftime(date, sizeof(date), "%Y/%m/%d %H:%M:%S", &parts);
    puts(date);
    unsigned long long total_memory = 0, free_memory = 0;
    size_t size = sizeof(total_memory);
    (void)sysctlbyname("hw.memsize", &total_memory, &size, nullptr, 0);
    size = sizeof(free_memory);
    (void)sysctlbyname("hw.freemem", &free_memory, &size, nullptr, 0);
    int processors = 1;
    size = sizeof(processors);
    (void)sysctlbyname("hw.ncpu", &processors, &size, nullptr, 0);
    double used_share = 0;
    for (size_t i = 0; i < now.count; ++i)
        used_share += rows[i].cpu;
    used_share /= processors > 0 ? processors : 1;
    double load[3] = {};
    (void)getloadavg(load, 3);
    printf("Load Avg: %.2f, %.2f, %.2f\n", load[0], load[1], load[2]);
    // (Which of what it's spent is the kernel's isn't known, per process: it's all said to be the program's.)
    printf("CPU usage: %.2f%% user, 0.00%% sys, %.2f%% idle\n", used_share, used_share < 100 ? 100 - used_share : 0);
    unsigned long long resident = 0;
    for (size_t i = 0; i < now.count; ++i)
        resident += now.processes[i].resident_bytes;
    char resident_text[32];
    format_size(resident, resident_text, sizeof(resident_text));
    printf("SharedLibs: 0B resident, 0B data, 0B linkedit.\n");
    printf("MemRegions: %zu total, %s resident, 0B private, 0B shared.\n", now.count, resident_text);
    char used_text[32], free_text[32];
    format_size(total_memory - free_memory, used_text, sizeof(used_text));
    format_size(free_memory, free_text, sizeof(free_text));
    unsigned long long compressed = 0;
    size = sizeof(compressed);
    (void)sysctlbyname("vm.compressor.pool_bytes", &compressed, &size, nullptr, 0);
    char compressed_text[32];
    format_size(compressed, compressed_text, sizeof(compressed_text));
    printf("PhysMem: %s used (0B wired, %s compressor), %s unused.\n", used_text, compressed_text, free_text);
    printf("VM: 0B vsize, 0B framework vsize, 0(0) swapins, 0(0) swapouts.\n");
    printf("Networks: packets: 0/0B in, 0/0B out.\n");
    printf("Disks: 0/0B read, 0/0B written.\n");
    putchar('\n');
    if (limit == 0) {
        free(rows);
        return;
    }

    printf("%-6s %-16s %-6s %-9s %-4s %-7s %-6s %-6s %-9s %-5s\n", "PID", "COMMAND", "%CPU", "TIME", "#TH", "MEM", "PGRP", "PPID", "STATE", "UID");
    size_t const shown = limit >= 0 && static_cast<size_t>(limit) < now.count ? static_cast<size_t>(limit) : now.count;
    for (size_t i = 0; i < shown; ++i) {
        auto const& process = *rows[i].process;
        char memory_text[32], cpu_time[32];
        format_size(process.resident_bytes, memory_text, sizeof(memory_text));
        format_time(process.cpu_nanoseconds, cpu_time, sizeof(cpu_time));
        printf("%-6d %-16.16s %-6.1f %-9s %-4llu %-7s %-6d %-6d %-9s %-5u\n", process.pid, process.name, rows[i].cpu, cpu_time, process.threads, memory_text, process.pgid, process.ppid, state_name(process.state), process.uid);
    }
    free(rows);
}

}

int main(int argc, char** argv)
{
    int samples = -1, limit = -1, delay = 1;
    Order order = Order::CPU;
    for (int i = 1; i < argc; ++i) {
        char const* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (strcmp(argv[i], "-l") == 0 && value) {
            samples = atoi(value);
            ++i;
        } else if (strcmp(argv[i], "-n") == 0 && value) {
            limit = atoi(value);
            ++i;
        } else if (strcmp(argv[i], "-s") == 0 && value) {
            delay = atoi(value);
            ++i;
        } else if (strcmp(argv[i], "-o") == 0 && value) {
            if (strcmp(value, "cpu") == 0)
                order = Order::CPU;
            else if (strcmp(value, "mem") == 0)
                order = Order::Memory;
            else if (strcmp(value, "pid") == 0)
                order = Order::Process;
            else if (strcmp(value, "command") == 0)
                order = Order::Command;
            else {
                fprintf(stderr, "top: invalid order: %s\n", value);
                return 1;
            }
            ++i;
        } else {
            fprintf(stderr, "usage: top [-l samples] [-n processes] [-s delay] [-o cpu|mem|pid|command]\n");
            return 1;
        }
    }
    bool const redraw = samples < 0 && isatty(STDOUT_FILENO);
    // Keep to the screen, when it's being redrawn.
    if (redraw && limit < 0)
        limit = 15;
    Sample before, now;
    bool have_before = false;
    for (int taken = 0; samples < 0 || taken < samples; ++taken) {
        if (!take_sample(now)) {
            warn("top", "kern.proc.all");
            return 1;
        }
        if (redraw)
            fputs("\x1b[H\x1b[2J", stdout);
        print_screen(now, have_before ? &before : nullptr, limit, order);
        if (!redraw)
            putchar('\n');
        fflush(stdout);
        // Swap, so that "before" keeps what it was.
        Sample const held = before;
        before = now;
        now = held;
        have_before = true;
        if (samples < 0 || taken + 1 < samples)
            sleep(static_cast<unsigned>(delay > 0 ? delay : 1));
    }
    return 0;
}
