/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// What commands say, on this system and on a real Mac (Meta/oracle-test.sh): the same program, run on each, with the same files to work on, for
// Meta/compare-commands.py to compare. Machine-specific things like sizes and dates are what's in Tests/Parity/command-differences.json.

namespace {

char const* const s_directory = "/tmp/commands-test";

void write_file(char const* name, char const* text)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", s_directory, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    (void)write(fd, text, strlen(text));
    close(fd);
}

void run(char const* name, char const* command)
{
    printf("commands: BEGIN %s\n", name);
    fflush(stdout);
    int ends[2];
    if (pipe(ends) != 0)
        return;
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        close(ends[0]);
        dup2(ends[1], 1);
        dup2(ends[1], 2);
        close(ends[1]);
        int nothing = open("/dev/null", O_RDONLY);
        dup2(nothing, 0);
        close(nothing);
        if (chdir(s_directory) != 0)
            _exit(126);
        execl("/bin/sh", "sh", "-c", command, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(ends[1]);
    fcntl(ends[0], F_SETFL, O_NONBLOCK);
    // As it comes, for as long as it should take at most.
    timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    bool timed_out = false;
    for (;;) {
        pollfd waiting { ends[0], POLLIN, 0 };
        (void)poll(&waiting, 1, 100);
        char buffer[4096];
        ssize_t const got = read(ends[0], buffer, sizeof(buffer));
        if (got > 0) {
            fwrite(buffer, 1, static_cast<size_t>(got), stdout);
            continue;
        }
        if (got == 0)
            break;
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - started.tv_sec > 20) {
            timed_out = true;
            kill(-child, SIGKILL);
            kill(child, SIGKILL);
            break;
        }
    }
    close(ends[0]);
    int status = -1;
    waitpid(child, &status, 0);
    fflush(stdout);
    if (timed_out)
        printf("TIMED OUT\n");
    printf("commands: END %s status=%d\n", name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    fflush(stdout);
}

void make_files()
{
    (void)system("rm -rf /tmp/commands-test");
    mkdir(s_directory, 0755);
    write_file("hello.txt", "hello\nworld\n");
    write_file("hello2.txt", "hello\nthere\nworld\n");
    write_file("numbers.txt", "1 one\n2 two\n3 three\n4 four\n");
    write_file("columns.txt", "alice 30 london\nbob 25 paris\ncarol 41 rome\n");
    write_file("empty.txt", "");
    write_file("script.sh", "#!/bin/sh\necho hi\n");
    write_file("page.html", "<!DOCTYPE html>\n<html><body>hi</body></html>\n");
    write_file("data.json", "{\"a\": 1, \"b\": [true, \"x\"]}\n");
    write_file("code.c", "#include <stdio.h>\nint main(void) { return 0; }\n");
    write_file("Makefile",
        "all: out.txt\n"
        "\t@echo done\n"
        "out.txt: hello.txt\n"
        "\t@echo building $@ from $<\n"
        "\t@cp $< $@\n"
        "NAME = value\n"
        "show:\n"
        "\t@echo $(NAME) $(words a b c) $(patsubst %.c,%.o,x.c y.c)\n"
        ".PHONY: all show\n");
    write_file("list.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>Age</key>\n\t<integer>30</integer>\n"
        "\t<key>Name</key>\n\t<string>Alice</string>\n"
        "\t<key>Tags</key>\n\t<array>\n\t\t<string>one</string>\n\t\t<true/>\n\t\t<real>1.5</real>\n\t</array>\n"
        "</dict>\n</plist>\n");
    mkdir("/tmp/commands-test/tree", 0755);
    mkdir("/tmp/commands-test/tree/sub", 0755);
    write_file("tree/a.txt", "file a\n");
    write_file("tree/sub/b.txt", "file b\n");
}

}

int main()
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    make_files();

    // cmp
    run("cmp the same", "cmp hello.txt hello.txt");
    run("cmp different", "cmp hello.txt hello2.txt");
    run("cmp -l", "cmp -l hello.txt hello2.txt");
    run("cmp -s", "cmp -s hello.txt hello2.txt; echo $?");
    run("cmp shorter", "cmp hello.txt empty.txt");
    run("cmp of nothing", "cmp hello.txt absent.txt");

    // dd
    run("dd copies", "dd if=hello.txt of=dd-out.txt 2>/dev/null; cat dd-out.txt");
    run("dd bs and count", "dd if=numbers.txt bs=4 count=2 2>/dev/null");
    run("dd skip", "dd if=numbers.txt bs=1 skip=6 count=5 2>/dev/null");
    run("dd seek", "dd if=hello.txt of=dd2.txt bs=1 seek=3 2>/dev/null; od -c dd2.txt | head -2");
    run("dd conv=ucase", "dd if=hello.txt conv=ucase 2>/dev/null");
    run("dd of nothing", "dd if=absent.txt");

    // file
    run("file of text", "file hello.txt");
    run("file of empty", "file empty.txt");
    run("file of a script", "file script.sh");
    run("file of html", "file page.html");
    run("file of json", "file data.json");
    run("file of C", "file code.c");
    run("file of a directory", "file tree");
    run("file -b", "file -b hello.txt");
    run("file of nothing", "file absent.txt");

    // diff
    run("diff the same", "diff hello.txt hello.txt; echo $?");
    run("diff normal", "diff hello.txt hello2.txt; echo $?");
    run("diff -u", "diff -u hello.txt hello2.txt | tail -n +3");
    run("diff -c", "diff -c hello.txt hello2.txt | tail -n +3");
    run("diff of change", "diff numbers.txt columns.txt");
    run("diff -q", "diff -q hello.txt hello2.txt");
    run("diff of nothing", "diff hello.txt absent.txt");

    // patch
    run("patch applies", "cp hello.txt p.txt; diff -u hello.txt hello2.txt > p.diff; patch p.txt p.diff; cat p.txt");
    run("patch -R", "cp hello2.txt r.txt; diff -u hello.txt hello2.txt > r.diff; patch -R r.txt r.diff; cat r.txt");

    // gzip
    run("gzip and back", "cp hello.txt g.txt; gzip g.txt; ls g.txt* ; gunzip g.txt.gz; cat g.txt");
    run("gzip -c, zcat", "gzip -c hello.txt > c.gz; zcat < c.gz");
    run("gzip -t", "gzip -c hello.txt > t.gz; gzip -t t.gz; echo $?");
    run("gzip of nothing", "gzip absent.txt");
    run("gunzip of nonsense", "cp hello.txt n.gz; gunzip n.gz");

    // tar
    run("tar c and t", "tar cf t.tar tree; tar tf t.tar | sort");
    run("tar x", "tar cf x.tar tree; mkdir out; cd out && tar xf ../x.tar; cat tree/sub/b.txt");
    run("tar z", "tar czf z.tgz tree; tar tzf z.tgz | sort");
    run("tar v", "tar cvf v.tar hello.txt");
    run("tar -C", "tar cf cc.tar -C tree a.txt; tar tf cc.tar");
    run("tar of nothing", "tar cf none.tar absent.txt");
    run("tar reading nothing", "tar tf absent.tar");

    // zip, unzip
    run("zip", "zip -q z.zip hello.txt numbers.txt; unzip -l z.zip | tail -n +2 | cut -c1-9,29-");
    run("zip -r", "zip -qr r.zip tree; unzip -Z1 r.zip | sort");
    run("unzip", "zip -q u.zip hello.txt; mkdir uz; cd uz && unzip -q ../u.zip && cat hello.txt");
    run("unzip -p", "zip -q p.zip hello.txt; unzip -p p.zip hello.txt");
    run("unzip of nothing", "unzip -q absent.zip");

    // ditto
    run("ditto a tree", "ditto tree copied; cat copied/sub/b.txt");
    run("ditto a zip", "ditto -c -k --norsrc tree d.zip; unzip -Z1 d.zip | sort");
    run("ditto back", "ditto -c -k tree e.zip; ditto -x -k e.zip back; cat back/a.txt");

    // awk
    run("awk print", "awk '{ print $2 }' numbers.txt");
    run("awk NR and NF", "awk '{ print NR, NF, $0 }' numbers.txt");
    run("awk pattern", "awk '/t/ { print $1 }' numbers.txt");
    run("awk BEGIN and END", "awk 'BEGIN { print \"start\" } { sum += $1 } END { print sum }' numbers.txt");
    run("awk -F", "echo a:b:c | awk -F: '{ print $3, $1 }'");
    run("awk printf", "awk '{ printf \"%-6s|%3d|\\n\", $1, $2 }' columns.txt");
    run("awk arrays", "awk '{ count[length($1)]++ } END { for (k in count) n++; print n }' columns.txt");
    run("awk functions", "echo Hello | awk '{ print length($0), toupper($0), tolower($0), substr($0, 2, 3), index($0, \"l\") }'");
    run("awk split and gsub", "echo a-b-c | awk '{ n = split($0, p, \"-\"); gsub(/-/, \"+\"); print n, p[2], $0 }'");
    run("awk numbers", "awk 'BEGIN { print 1/4, 10%3, 2^10, int(3.9), 1e3 }'");
    run("awk conditions", "awk '$2 > 26 && $2 < 40 { print $1 } $1 == \"bob\" { print \"found\" }' columns.txt");
    run("awk -v", "awk -v x=5 'BEGIN { print x * 2 }'");
    run("awk a syntax error", "awk 'BEGIN {' ; echo $?");

    // make
    run("make", "make");
    run("make again", "make; make");
    run("make a target", "make show");
    run("make -n", "touch hello.txt; rm -f out.txt; make -n");
    run("make of nothing", "make absent");
    run("make VAR=", "make show NAME=other");

    // plutil
    run("plutil -lint", "plutil -lint list.plist");
    run("plutil -p", "plutil -p list.plist");
    run("plutil to json", "plutil -convert json -o - list.plist; echo");
    run("plutil to xml", "plutil -convert xml1 -o - data.json");
    run("plutil to binary and back", "plutil -convert binary1 -o b.plist list.plist; file b.plist; plutil -convert xml1 -o - b.plist | head -6");
    run("plutil -extract", "plutil -extract Name raw -o - list.plist");
    run("plutil of nonsense", "plutil -lint hello.txt");

    // defaults
    run("defaults write and read", "defaults write /tmp/commands-test/prefs Greeting Hello; defaults read /tmp/commands-test/prefs Greeting");
    run("defaults types", "defaults write /tmp/commands-test/prefs Count -int 3; defaults write /tmp/commands-test/prefs On -bool true; defaults read /tmp/commands-test/prefs");
    run("defaults read-type", "defaults write /tmp/commands-test/prefs N -int 3; defaults read-type /tmp/commands-test/prefs N");
    run("defaults delete", "defaults write /tmp/commands-test/prefs X 1; defaults delete /tmp/commands-test/prefs X; defaults read /tmp/commands-test/prefs X");
    run("defaults of nothing", "defaults read /tmp/commands-test/absent");

    // killall, sw_vers, system_profiler, networksetup, launchctl, top, mkfifo: what these say depends on the machine, and how is what's compared.
    run("killall of nothing", "killall no-such-program");
    run("killall -s", "(sleep 30 &) ; killall -s sleep | head -1 | cut -d' ' -f1; killall sleep");
    run("sw_vers", "sw_vers");
    run("sw_vers -productName", "sw_vers -productName");
    run("mkfifo", "mkfifo pipe; ls -l pipe | cut -c1; mkfifo pipe; rm pipe");
    run("mkfifo -m", "mkfifo -m 600 pipe2; ls -l pipe2 | cut -c1-10");
    run("launchctl list", "launchctl list | head -1");
    run("launchctl of nothing", "launchctl list no.such.job");
    run("top -l", "top -l 1 -n 0 | grep -v '^[0-9]' | cut -d: -f1");
    run("networksetup -listallnetworkservices", "networksetup -listallnetworkservices | head -1");
    run("system_profiler", "system_profiler SPSoftwareDataType | head -3");

    // What's attached to a file. (A Mac also says where a file came from, which isn't anybody's business here.)
    (void)system("rm -f /tmp/commands-test/attached /tmp/commands-test/also");
    write_file("attached", "");
    write_file("also", "");
    run("xattr -w and list", "xattr -w org.example.one hello attached; xattr -w org.example.two 'a value' attached; xattr attached | grep -v provenance");
    run("xattr -l", "xattr -l attached | grep -v provenance");
    run("xattr -p", "xattr -p org.example.one attached");
    run("xattr -px", "xattr -px org.example.one attached");
    run("xattr -p of nothing", "xattr -p org.example.none attached; echo status $?");
    run("xattr -lx", "xattr -d org.example.two attached; xattr -lx attached | tail -n 3");
    run("xattr -d of nothing", "xattr -d org.example.none attached; echo status $?");
    run("xattr -wx", "xattr -wx org.example.hex 48656c6c6f attached; xattr -p org.example.hex attached");
    run("xattr of two files", "xattr -w a b also; xattr attached also | grep -v provenance");
    run("xattr -l of two files", "xattr -l attached also | grep -v provenance");
    run("xattr of no file", "xattr nothing-here; echo status $?");
    run("xattr -c", "xattr -c attached; xattr attached | grep -v provenance; echo done");
    run("ls -l@", "xattr -w org.example.one hello also; ls -l@ also | tail -n +2 | grep -v provenance");
    run("ls -lO", "ls -lO attached | awk '{print $5}'; chflags uchg attached; ls -lO attached | awk '{print $5}'; chflags nouchg attached");
    run("ls -lO of two", "chflags hidden,nodump attached; ls -lO attached | awk '{print $5}'; chflags 0 attached; ls -lO attached | awk '{print $5}'");
    run("ls -lo", "ls -lo attached | wc -w");
    run("chflags of nonsense", "chflags nonsense attached; echo status $?");
    run("chflags of no file", "chflags nouchg nothing-here; echo status $?");
    run("chmod +a", "chmod +a 'everyone deny write' attached; ls -le attached | tail -n +2");
    run("chmod +a allow", "chmod +a 'user:root allow read' attached; ls -le attached | tail -n +2");
    run("chmod +a#", "chmod +a# 1 'everyone allow read' attached; ls -le attached | tail -n +2");
    run("chmod -a#", "chmod -a# 0 attached; ls -le attached | tail -n +2");
    run("chmod -a", "chmod -a 'everyone allow read' attached; ls -le attached | tail -n +2");
    run("chmod =a#", "chmod =a# 0 'everyone deny execute' attached; ls -le attached | tail -n +2");
    run("chmod -N", "chmod -N attached; ls -le attached | tail -n +2; echo done");
    run("chmod +a of nonsense", "chmod +a 'nobody-of-the-name allow read' attached; echo status $?");
    run("chmod +a of all a file has", "chmod +a 'everyone allow read,write,execute,delete,append,readattr,writeattr,readextattr,writeextattr,readsecurity,writesecurity,chown' attached; ls -le attached | tail -n +2; chmod -N attached");
    run("chmod +a of all a directory has", "mkdir folder-with-rights; chmod +a 'everyone allow list,add_file,search,delete,add_subdirectory,delete_child,file_inherit,directory_inherit,limit_inherit,only_inherit' folder-with-rights; ls -lde folder-with-rights | tail -n +2; rmdir folder-with-rights");
    run("ls -l with an access list", "chmod +a 'everyone deny write' also; xattr -c also; ls -l also | cut -c11; chmod -N also");
    run("cp -c", "cp -c also also-copy; cat also-copy; xattr also-copy | grep -v provenance; rm also-copy");

    (void)system("rm -rf /tmp/commands-test");
    printf("commands: finished\n");
    return 0;
}
