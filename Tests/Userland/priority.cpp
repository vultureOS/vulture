/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/port.h>
#include <time.h>
#include <unistd.h>

// Who goes first, and what happens when the thread that everyone's waiting for isn't it. With every processor kept busy at one level, a thread below that
// gets nothing: which is as it should be, until something above it is waiting for it. Then it has to be let through.

namespace {

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "priority: line %d: %s: %s\n", __LINE__, #condition, strerror(errno)); \
            exit(1);                                                                           \
        }                                                                                      \
    } while (0)

double now()
{
    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

// However many it takes to use every processor, at the default level.
constexpr int most_hogs = 16;
pthread_t s_hogs[most_hogs];
int s_hog_count = 0;
volatile int s_stop_hogs = 0;

void* hog(void*)
{
    while (!s_stop_hogs) { }
    return nullptr;
}

void start_hogs()
{
    s_stop_hogs = 0;
    s_hog_count = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    CHECK(s_hog_count > 0 && s_hog_count <= most_hogs);
    for (int i = 0; i < s_hog_count; ++i)
        CHECK(pthread_create(&s_hogs[i], nullptr, hog, nullptr) == 0);
    // Until they've all got a processor.
    timespec settle { 0, 50 * 1000 * 1000 };
    nanosleep(&settle, nullptr);
}

void stop_hogs()
{
    s_stop_hogs = 1;
    for (int i = 0; i < s_hog_count; ++i)
        pthread_join(s_hogs[i], nullptr);
}

// Enough to notice, and no more.
void work_a_little()
{
    for (volatile long i = 0; i < 2000000; i = i + 1) { }
}

constexpr double more_than_it_should_take = 1.0;

void the_interface()
{
    CHECK(qos_class_self() == QOS_CLASS_DEFAULT);
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0) == 0);
    CHECK(qos_class_self() == QOS_CLASS_UTILITY);
    qos_class_t got = QOS_CLASS_UNSPECIFIED;
    int relative = 1;
    CHECK(pthread_get_qos_class_np(pthread_self(), &got, &relative) == 0 && got == QOS_CLASS_UTILITY && relative == 0);
    CHECK(pthread_set_qos_class_self_np(static_cast<qos_class_t>(0x77), 0) == EINVAL);
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 1) == EINVAL);

    // New threads take after whoever made them, unless they've been told otherwise.
    pthread_t thread;
    void* result = nullptr;
    CHECK(pthread_create(&thread, nullptr, [](void*) -> void* { return reinterpret_cast<void*>(static_cast<long>(qos_class_self())); }, nullptr) == 0);
    pthread_join(thread, &result);
    CHECK(reinterpret_cast<long>(result) == QOS_CLASS_UTILITY);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    CHECK(pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_USER_INITIATED, 0) == 0);
    CHECK(pthread_attr_get_qos_class_np(&attributes, &got, nullptr) == 0 && got == QOS_CLASS_USER_INITIATED);
    CHECK(pthread_create(&thread, &attributes, [](void*) -> void* { return reinterpret_cast<void*>(static_cast<long>(qos_class_self())); }, nullptr) == 0);
    pthread_join(thread, &result);
    CHECK(reinterpret_cast<long>(result) == QOS_CLASS_USER_INITIATED);
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0) == 0);
    printf("priority: threads can say what they're for\n");
}

// That this is as bad as all that without help: otherwise the rest of this isn't showing anything.
volatile long s_counted = 0;
volatile int s_stop_counting = 0;

void the_control()
{
    pthread_attr_t background;
    pthread_attr_init(&background);
    pthread_attr_set_qos_class_np(&background, QOS_CLASS_BACKGROUND, 0);
    start_hogs();
    pthread_t counter;
    CHECK(pthread_create(&counter, &background, [](void*) -> void* {
        while (!s_stop_counting)
            s_counted = s_counted + 1;
        return nullptr;
    }, nullptr) == 0);
    timespec a_while { 0, 400 * 1000 * 1000 };
    nanosleep(&a_while, nullptr);
    long const counted_while_busy = s_counted;
    stop_hogs();
    timespec brief { 0, 100 * 1000 * 1000 };
    nanosleep(&brief, nullptr);
    long const counted_after = s_counted - counted_while_busy;
    s_stop_counting = 1;
    pthread_join(counter, nullptr);
    printf("priority: a background thread counted to %ld with everything busy, and %ld more in a quarter of the time when not\n", counted_while_busy, counted_after);
    CHECK(counted_while_busy < counted_after / 4);
}

pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int s_holder_has_it = 0;

void* holder_of_the_lock(void*)
{
    pthread_mutex_lock(&s_lock);
    s_holder_has_it = 1;
    // Waits for the hogs to be up and running, at its level, having nothing to do with them.
    timespec settle { 0, 150 * 1000 * 1000 };
    nanosleep(&settle, nullptr);
    work_a_little();
    pthread_mutex_unlock(&s_lock);
    return nullptr;
}

void inheritance()
{
    pthread_attr_t background;
    pthread_attr_init(&background);
    pthread_attr_set_qos_class_np(&background, QOS_CLASS_BACKGROUND, 0);
    pthread_t holder;
    s_holder_has_it = 0;
    CHECK(pthread_create(&holder, &background, holder_of_the_lock, nullptr) == 0);
    while (!s_holder_has_it)
        sched_yield();
    start_hogs();
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0);
    double const started = now();
    pthread_mutex_lock(&s_lock);
    double const waited = now() - started;
    pthread_mutex_unlock(&s_lock);
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0) == 0);
    stop_hogs();
    pthread_join(holder, nullptr);
    printf("priority: a background thread with a lock got it done in %d ms, with every processor busy\n", static_cast<int>(waited * 1000));
    CHECK(waited < more_than_it_should_take);
}

// Each passes it on to the next: A waits for B, which waits for C.
pthread_mutex_t s_first = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t s_second = PTHREAD_MUTEX_INITIALIZER;
volatile int s_chain_step = 0;

void chains()
{
    pthread_attr_t background;
    pthread_attr_init(&background);
    pthread_attr_set_qos_class_np(&background, QOS_CLASS_BACKGROUND, 0);
    pthread_t last, middle;
    s_chain_step = 0;
    CHECK(pthread_create(&last, &background, [](void*) -> void* {
        pthread_mutex_lock(&s_second);
        s_chain_step = 1;
        timespec settle { 0, 200 * 1000 * 1000 };
        nanosleep(&settle, nullptr);
        work_a_little();
        pthread_mutex_unlock(&s_second);
        return nullptr;
    }, nullptr) == 0);
    while (s_chain_step < 1)
        sched_yield();
    CHECK(pthread_create(&middle, &background, [](void*) -> void* {
        pthread_mutex_lock(&s_first);
        s_chain_step = 2;
        pthread_mutex_lock(&s_second);
        work_a_little();
        pthread_mutex_unlock(&s_second);
        pthread_mutex_unlock(&s_first);
        return nullptr;
    }, nullptr) == 0);
    while (s_chain_step < 2)
        sched_yield();
    start_hogs();
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0);
    double const started = now();
    pthread_mutex_lock(&s_first);
    double const waited = now() - started;
    pthread_mutex_unlock(&s_first);
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0) == 0);
    stop_hogs();
    pthread_join(middle, nullptr);
    pthread_join(last, nullptr);
    printf("priority: through two of them, in %d ms\n", static_cast<int>(waited * 1000));
    CHECK(waited < more_than_it_should_take);
}

int s_service_port[2];
volatile int s_service_ready = 0;

void* a_slow_service(void*)
{
    s_service_ready = 1;
    static char buffer[64];
    for (;;) {
        vulture_port_received received {};
        received.bytes = buffer;
        received.capacity = sizeof(buffer);
        if (port_receive(s_service_port[0], &received, 0) != 0)
            return nullptr;
        work_a_little();
        vulture_port_message answer {};
        answer.id = 2;
        answer.bytes = "done";
        answer.size = 4;
        answer.reply = -1;
        CHECK(port_send(received.reply, &answer, 0) == 0);
        close(received.reply);
    }
}

void messages()
{
    CHECK(port_create(s_service_port, 0) == 0);
    pthread_attr_t background;
    pthread_attr_init(&background);
    pthread_attr_set_qos_class_np(&background, QOS_CLASS_BACKGROUND, 0);
    pthread_t service;
    s_service_ready = 0;
    CHECK(pthread_create(&service, &background, a_slow_service, nullptr) == 0);
    while (!s_service_ready)
        sched_yield();
    // Into its first wait, before everything gets busy.
    timespec settle { 0, 100 * 1000 * 1000 };
    nanosleep(&settle, nullptr);
    start_hogs();
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0);
    double const started = now();
    vulture_port_message request {};
    request.id = 1;
    request.bytes = "go";
    request.size = 2;
    request.reply = -1;
    char buffer[64];
    vulture_port_received answer {};
    answer.bytes = buffer;
    answer.capacity = sizeof(buffer);
    CHECK(port_call(s_service_port[1], &request, &answer, 0) == 0);
    double const waited = now() - started;
    CHECK(pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0) == 0);
    stop_hogs();
    printf("priority: a background service, doing something for someone who's waiting, took %d ms\n", static_cast<int>(waited * 1000));
    CHECK(waited < more_than_it_should_take);
    close(s_service_port[1]);
    pthread_join(service, nullptr);
    close(s_service_port[0]);
}

}

int main()
{
    the_interface();
    the_control();
    inheritance();
    chains();
    messages();
    printf("priority: PASS\n");
    return 0;
}
