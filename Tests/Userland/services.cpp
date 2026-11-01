/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <LibIPC/Bootstrap.h>
#include <NotifyEndpoint.h>
#include <PasteboardEndpoint.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <os/log.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The system services, each a vlaunchd job as at boot: logd, notifyd,
// configd and pboard, in a vlaunchd of the test's own. A client (this
// program with --client) runs `log stream`, then works each service through
// its C interface and its tool (notify(3) and notifyutil, scutil, pbcopy and
// pbpaste, os_log and log), and finally checks the stream showed what the
// services logged meanwhile. The client reports in client.out.

namespace {

char const* const s_directory = "/tmp/services-test";

void pause_milliseconds(long milliseconds)
{
    struct timespec duration { milliseconds / 1000, (milliseconds % 1000) * 1'000'000 };
    nanosleep(&duration, nullptr);
}

void write_file(char const* name, char const* text)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    (void)write(fd, text, strlen(text));
    close(fd);
}

char const* read_file(char const* name)
{
    static char text[32768];
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    text[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return text;
    size_t length = 0;
    ssize_t got;
    while (length + 1 < sizeof(text) && (got = read(fd, text + length, sizeof(text) - 1 - length)) > 0)
        length += static_cast<size_t>(got);
    text[length] = '\0';
    close(fd);
    return text;
}

// A command's standard output (through /bin/sh) and exit status.
char const* run(char const* command, int* status = nullptr)
{
    static char output[32768];
    output[0] = '\0';
    FILE* pipe = popen(command, "r");
    if (!pipe)
        return output;
    size_t length = 0;
    size_t got;
    while (length + 1 < sizeof(output) && (got = fread(output + length, 1, sizeof(output) - 1 - length, pipe)) > 0)
        length += got;
    output[length] = '\0';
    int result = pclose(pipe);
    if (status)
        *status = WIFEXITED(result) ? WEXITSTATUS(result) : 128;
    return output;
}

// In the client: a line per finding.
void report(char const* what, bool ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    fflush(stdout);
}

// Runs `body` in a child as user 501 (vulture); its exit status.
template<typename Body>
int as_user(Body body)
{
    pid_t child = fork();
    if (child == 0) {
        if (setgid(20) < 0 || setuid(501) < 0)
            _exit(125);
        _exit(body());
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

void check_notifications()
{
    int token = 0, check = 0;
    bool ok = notify_register_check("com.vulture.test.ping", &token) == NOTIFY_STATUS_OK && notify_check(token, &check) == NOTIFY_STATUS_OK
        && check == 1;
    ok = ok && notify_check(token, &check) == NOTIFY_STATUS_OK && check == 0;
    ok = ok && notify_post("com.vulture.test.ping") == NOTIFY_STATUS_OK && notify_check(token, &check) == NOTIFY_STATUS_OK && check == 1;
    report("notify_check sees posts", ok);

    uint64_t state = 0;
    ok = notify_set_state(token, 42) == NOTIFY_STATUS_OK && notify_get_state(token, &state) == NOTIFY_STATUS_OK && state == 42;
    report("state", ok);
    notify_cancel(token);
    report("cancelled tokens are invalid", !notify_is_valid_token(token) && notify_check(token, &check) == NOTIFY_STATUS_INVALID_TOKEN);

    int fd = -1;
    ok = notify_register_file_descriptor("com.vulture.test.file", &fd, 0, &token) == NOTIFY_STATUS_OK;
    ok = ok && notify_post("com.vulture.test.file") == NOTIFY_STATUS_OK;
    unsigned char bytes[4] = {};
    pollfd watched { fd, POLLIN, 0 };
    ok = ok && poll(&watched, 1, 2000) == 1 && read(fd, bytes, 4) == 4;
    int told = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
    report("a file descriptor receives the token", ok && told == token);
    notify_cancel(token);
    close(fd);

    int receive = -1;
    ok = notify_register_port("com.vulture.test.port", &receive, 0, &token) == NOTIFY_STATUS_OK;
    ok = ok && notify_post("com.vulture.test.port") == NOTIFY_STATUS_OK;
    IPC::Port port(receive);
    bool told_by_port = false;
    if (ok) {
        auto message = IPC::receive(port);
        if (!message.is_error()) {
            IPC::Decoder decoder(message.value());
            i32 sent_token = 0;
            String name;
            told_by_port = !decoder.expect_magic(NotifyClientEndpoint::magic).is_error() && !decoder.decode(sent_token).is_error()
                && !decoder.decode(name).is_error() && sent_token == token && name.view() == "com.vulture.test.port"_sv;
        }
    }
    report("a port receives the notification", told_by_port);
    notify_cancel(token);

    // The system's names are root's to set.
    int status = as_user([] {
        int user_token = 0;
        notify_register_check("com.vulture.system.test", &user_token);
        return notify_set_state(user_token, 1) == NOTIFY_STATUS_NOT_AUTHORIZED ? 0 : 1;
    });
    report("only root sets the state of com.vulture.system names", status == 0);

    // notifyutil: one watching for a post, one posting until it is seen.
    pid_t watcher = fork();
    if (watcher == 0) {
        int out = open("/tmp/services-test/notifyutil.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(out, STDOUT_FILENO);
        execl("/usr/bin/notifyutil", "notifyutil", "-1", "com.vulture.test.tool", nullptr);
        _exit(127);
    }
    for (int i = 0; i < 40 && !strstr(read_file("notifyutil.out"), "com.vulture.test.tool"); ++i)
        run("notifyutil -z 50 -p com.vulture.test.tool");
    int watcher_status = 0;
    waitpid(watcher, &watcher_status, 0);
    report("notifyutil -p reaches notifyutil -1", strcmp(read_file("notifyutil.out"), "com.vulture.test.tool\n") == 0);
    run("notifyutil -s com.vulture.test.tool 7");
    report("notifyutil -s and -g", strcmp(run("notifyutil -g com.vulture.test.tool"), "com.vulture.test.tool 7\n") == 0);
}

void check_configuration()
{
    char local[128] = {};
    snprintf(local, sizeof(local), "%s", run("scutil --get LocalHostName"));
    report("there is a LocalHostName", local[0] && local[0] != '\n');
    int status = 0;
    char const* output = run("scutil --get HostName", &status);
    report("HostName: not set", strcmp(output, "HostName: not set\n") == 0 && status == 1);

    run("scutil --set ComputerName 'Test Machine'", &status);
    report("scutil --set ComputerName", status == 0 && strcmp(run("scutil --get ComputerName"), "Test Machine\n") == 0);
    report("the preferences are saved", strstr(read_file("preferences.plist"), "\"ComputerName\" : \"Test Machine\"") != nullptr);

    int token = 0, check = 0;
    notify_register_check("com.vulture.system.hostname", &token);
    notify_check(token, &check);
    run("scutil --set HostName services.example", &status);
    char host[256] = {};
    gethostname(host, sizeof(host));
    report("HostName becomes the host name", status == 0 && strcmp(host, "services.example") == 0);
    report("the change is posted", notify_check(token, &check) == NOTIFY_STATUS_OK && check == 1);
    notify_cancel(token);

    run("echo '' | scutil --set HostName", &status);
    gethostname(host, sizeof(host));
    char expected[160];
    local[strcspn(local, "\n")] = '\0';
    snprintf(expected, sizeof(expected), "%s.local", local);
    report("without HostName, the host name is LocalHostName.local", status == 0 && strcmp(host, expected) == 0);

    run("scutil --set LocalHostName 'not valid' 2>/dev/null", &status);
    report("an invalid name is refused", status == 1);
    status = as_user([] {
        execl("/usr/sbin/scutil", "scutil", "--set", "ComputerName", "Mine", nullptr);
        return 127;
    });
    report("only root sets names", status == 1 && strcmp(run("scutil --get ComputerName"), "Test Machine\n") == 0);
}

void check_pasteboard()
{
    run("echo hello pasteboard | pbcopy");
    report("pbcopy and pbpaste", strcmp(run("pbpaste"), "hello pasteboard\n") == 0);
    run("echo needle | pbcopy -pboard find");
    report("boards are separate", strcmp(run("pbpaste -pboard find"), "needle\n") == 0 && strcmp(run("pbpaste"), "hello pasteboard\n") == 0);

    // Big enough to go out of line.
    auto port = IPC::look_up("com.vulture.pasteboard"_sv);
    bool ok = !port.is_error();
    if (ok) {
        PasteboardProxy pasteboard(port.release_value());
        Vector<u8> data;
        for (size_t i = 0; i < 300 * 1024; ++i)
            data.append(static_cast<u8>(i * 7));
        auto set = pasteboard.set("general"_sv, "public.data"_sv, data.span());
        auto got = pasteboard.get("general"_sv, "public.data"_sv);
        ok = !set.is_error() && set.value().error == 0 && !got.is_error() && got.value().found && got.value().data.size() == data.size()
            && memcmp(got.value().data.data(), data.data(), data.size()) == 0;
        auto types = pasteboard.types("general"_sv);
        ok = ok && !types.is_error() && types.value().size() == 1 && types.value()[0].view() == "public.data"_sv;
        auto count = pasteboard.change_count("general"_sv);
        ok = ok && !count.is_error() && count.value() == set.value().change_count;
    }
    report("300 KiB through the pasteboard", ok);

    // Each user has their own.
    int status = as_user([] {
        int fd = open("/tmp/services-test/user-paste", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        dup2(fd, STDOUT_FILENO);
        execl("/usr/bin/pbpaste", "pbpaste", "-pboard", "find", nullptr);
        return 127;
    });
    report("another user's pasteboard is their own", status == 0 && read_file("user-paste")[0] == '\0');
}

int run_client()
{
    // `log stream` first, to see what the services log from now on.
    pid_t streamer = fork();
    if (streamer == 0) {
        int out = open("/tmp/services-test/stream.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(out, STDOUT_FILENO);
        execl("/usr/bin/log", "log", "stream", "--level", "debug", nullptr);
        _exit(127);
    }
    for (int i = 0; i < 100 && !strstr(read_file("stream.out"), "Timestamp"); ++i)
        pause_milliseconds(20);
    report("log stream starts", strstr(read_file("stream.out"), "Timestamp                       Thread     Type        Activity             PID    TTL  \n") != nullptr);

    os_log_t log = os_log_create("com.vulture.test", "client");
    os_log(log, "Hello from the %{public}s", "client");
    os_log_error(log, "An error, %d", 42);
    run("logger -t tester a message from logger");

    check_notifications();
    check_configuration();
    check_pasteboard();

    // What the stream saw.
    for (int i = 0; i < 100 && !strstr(read_file("stream.out"), "pboard:"); ++i)
        pause_milliseconds(20);
    pause_milliseconds(100);
    kill(streamer, SIGTERM);
    waitpid(streamer, nullptr, 0);
    char const* stream = read_file("stream.out");
    report("the stream shows the client",
        strstr(stream, "services: [com.vulture.test:client] Hello from the client") && strstr(stream, " Error       0x0 ")
            && strstr(stream, "[com.vulture.test:client] An error, 42"));
    report("the stream shows logger", strstr(stream, "logger: tester: a message from logger") != nullptr);
    report("the stream shows notifyd", strstr(stream, "notifyd: [com.vulture.notifyd:server] com.vulture.test.ping posted by") != nullptr);
    report("the stream shows configd", strstr(stream, "configd: [com.vulture.SystemConfiguration:configd] ComputerName set to \"Test Machine\"") != nullptr);
    report("the stream shows pboard", strstr(stream, "pboard: [com.vulture.pasteboard:server] ") != nullptr && strstr(stream, " Info        0x0 "));
    if (!strstr(stream, "pboard:"))
        printf("stream:\n%s\n", stream);

    // log show: everything since logd started, its own start first.
    char const* shown = run("log show --info --debug");
    report("log show", strstr(shown, "Skipping") == nullptr && strstr(shown, "logd: [com.vulture.logd:server] Starting")
            && strstr(shown, "configd: [com.vulture.SystemConfiguration:configd] Starting")
            && strstr(shown, "\n--------------------------------------------------------------------------------------------------------------------\nLog      - Default: "));
    shown = run("log show --process notifyd");
    report("log show --process", strstr(shown, "Skipping info and debug messages, pass --info and/or --debug to include.\n")
            && strstr(shown, "Filtering the log data using \"process == \"notifyd\"\"\n") && strstr(shown, "notifyd: [com.vulture.notifyd:server] Starting")
            && !strstr(shown, "configd:"));
    printf("done\n");
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--client") == 0)
        return run_client();

    char original_host[256] = {};
    gethostname(original_host, sizeof(original_host));
    system("rm -rf /tmp/services-test");
    mkdir(s_directory, 0755);
    write_file("logd.plist", R"({
        "Label" : "com.vulture.logd",
        "ProgramArguments" : [ "/usr/libexec/logd" ],
        "MachServices" : { "com.vulture.logd" : true },
        "RunAtLoad" : true,
        "StandardErrorPath" : "/tmp/services-test/services.err"
    })");
    write_file("notifyd.plist", R"({
        "Label" : "com.vulture.notifyd",
        "ProgramArguments" : [ "/usr/sbin/notifyd" ],
        "MachServices" : { "com.vulture.notifyd" : true },
        "RunAtLoad" : true,
        "StandardErrorPath" : "/tmp/services-test/services.err"
    })");
    write_file("configd.plist", R"({
        "Label" : "com.vulture.configd",
        "ProgramArguments" : [ "/usr/libexec/configd", "--preferences", "/tmp/services-test/preferences.plist" ],
        "MachServices" : { "com.vulture.SystemConfiguration.configd" : true },
        "RunAtLoad" : true,
        "StandardErrorPath" : "/tmp/services-test/services.err"
    })");
    write_file("pboard.plist", R"({
        "Label" : "com.vulture.pboard",
        "ProgramArguments" : [ "/usr/libexec/pboard" ],
        "MachServices" : { "com.vulture.pasteboard" : true },
        "UserName" : "nobody",
        "StandardErrorPath" : "/tmp/services-test/services.err"
    })");
    write_file("client.plist", R"({
        "Label" : "com.vulture.test.client",
        "ProgramArguments" : [ "/usr/tests/services", "--client" ],
        "RunAtLoad" : true,
        "StandardOutPath" : "/tmp/services-test/client.out",
        "StandardErrorPath" : "/tmp/services-test/client.out"
    })");

    pid_t launchd = fork();
    if (launchd == 0) {
        char const* arguments[] = { "vlaunchd", "--log", "/tmp/services-test/log", "--directory", s_directory, nullptr };
        execv("/sbin/vlaunchd", const_cast<char**>(arguments));
        _exit(127);
    }
    for (int waited = 0; waited < 600 && !strstr(read_file("client.out"), "done"); ++waited)
        pause_milliseconds(50);
    kill(launchd, SIGTERM);
    int status = 0;
    waitpid(launchd, &status, 0);
    sethostname(original_host, strlen(original_host));

    static char report_text[32768];
    snprintf(report_text, sizeof(report_text), "%s", read_file("client.out"));
    char const* const expected[] = {
        "ok: log stream starts",
        "ok: notify_check sees posts",
        "ok: state",
        "ok: cancelled tokens are invalid",
        "ok: a file descriptor receives the token",
        "ok: a port receives the notification",
        "ok: only root sets the state of com.vulture.system names",
        "ok: notifyutil -p reaches notifyutil -1",
        "ok: notifyutil -s and -g",
        "ok: there is a LocalHostName",
        "ok: HostName: not set",
        "ok: scutil --set ComputerName",
        "ok: the preferences are saved",
        "ok: HostName becomes the host name",
        "ok: the change is posted",
        "ok: without HostName, the host name is LocalHostName.local",
        "ok: an invalid name is refused",
        "ok: only root sets names",
        "ok: pbcopy and pbpaste",
        "ok: boards are separate",
        "ok: 300 KiB through the pasteboard",
        "ok: another user's pasteboard is their own",
        "ok: the stream shows the client",
        "ok: the stream shows logger",
        "ok: the stream shows notifyd",
        "ok: the stream shows configd",
        "ok: the stream shows pboard",
        "ok: log show",
        "ok: log show --process",
        "done",
    };
    bool all = true;
    for (auto const* line : expected) {
        if (!strstr(report_text, line)) {
            fprintf(stderr, "services: missing \"%s\"\n", line);
            all = false;
        }
    }
    CHECK(all);
    if (!all)
        fprintf(stderr, "services: the client said:\n%s\nvlaunchd's log:\n%s\nthe services said:\n%s", report_text, read_file("log"),
            read_file("services.err"));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    system("rm -rf /tmp/services-test");
    return finish();
}
