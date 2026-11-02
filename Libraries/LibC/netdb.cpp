/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// The resolver: names from /etc/hosts, or a question over UDP to whoever /etc/resolv.conf says
// (RFC 1035). IPv4 addresses, as that is all there is here.

namespace {

constexpr int largest_addresses = 16;
constexpr int attempts_per_server = 2;
constexpr int milliseconds_per_attempt = 2000;
constexpr int largest_redirects = 8;

struct Addresses {
    in_addr_t values[largest_addresses];
    int count { 0 };
};

bool read_text_file(char const* path, char* buffer, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t got = read(fd, buffer, size - 1);
    close(fd);
    if (got < 0)
        return false;
    buffer[got] = '\0';
    return true;
}

char lowered(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
}

bool same_name(char const* a, size_t a_length, char const* b, size_t b_length)
{
    if (a_length != b_length)
        return false;
    for (size_t i = 0; i < a_length; ++i) {
        if (lowered(a[i]) != lowered(b[i]))
            return false;
    }
    return true;
}

bool look_in_hosts_file(char const* name, Addresses& addresses)
{
    static char text[8192];
    if (!read_text_file("/etc/hosts", text, sizeof(text)))
        return false;
    size_t name_length = strlen(name);
    char* line = text;
    while (line && *line) {
        char* end = strchr(line, '\n');
        if (end)
            *end = '\0';
        char* comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        char* at = line;
        while (*at == ' ' || *at == '\t')
            ++at;
        char* address_start = at;
        while (*at && *at != ' ' && *at != '\t')
            ++at;
        if (*at) {
            *at++ = '\0';
            in_addr_t value;
            if (inet_pton(AF_INET, address_start, &value) == 1) {
                while (*at) {
                    while (*at == ' ' || *at == '\t')
                        ++at;
                    char* word = at;
                    while (*at && *at != ' ' && *at != '\t')
                        ++at;
                    if (same_name(word, static_cast<size_t>(at - word), name, name_length) && addresses.count < largest_addresses)
                        addresses.values[addresses.count++] = value;
                }
            }
        }
        line = end ? end + 1 : nullptr;
    }
    return addresses.count > 0;
}

// The nameservers in /etc/resolv.conf, in the order given.
int read_name_servers(in_addr_t* servers, int room)
{
    static char text[4096];
    if (!read_text_file("/etc/resolv.conf", text, sizeof(text)))
        return 0;
    int count = 0;
    char* line = text;
    while (line && *line && count < room) {
        char* end = strchr(line, '\n');
        if (end)
            *end = '\0';
        if (strncmp(line, "nameserver", 10) == 0) {
            char* at = line + 10;
            while (*at == ' ' || *at == '\t')
                ++at;
            char* word = at;
            while (*at && *at != ' ' && *at != '\t' && *at != '\r')
                ++at;
            *at = '\0';
            in_addr_t value;
            if (inet_pton(AF_INET, word, &value) == 1)
                servers[count++] = value;
        }
        line = end ? end + 1 : nullptr;
    }
    return count;
}

// `name`, as lengths and labels. 0 if it won't do as a name.
size_t encode_name(char const* name, uint8_t* out, size_t room)
{
    size_t at = 0;
    char const* label = name;
    while (*label) {
        char const* dot = strchr(label, '.');
        size_t length = dot ? static_cast<size_t>(dot - label) : strlen(label);
        if (length == 0 || length > 63 || at + length + 2 > room)
            return 0;
        out[at++] = static_cast<uint8_t>(length);
        memcpy(out + at, label, length);
        at += length;
        if (!dot)
            break;
        label = dot + 1;
    }
    if (at + 1 > room)
        return 0;
    out[at++] = 0;
    return at;
}

// Moves past a name in a message, which may end in a pointer. 0 for nonsense.
size_t skip_name(uint8_t const* message, size_t size, size_t at)
{
    while (at < size) {
        uint8_t length = message[at];
        if (length == 0)
            return at + 1;
        if ((length & 0xc0) == 0xc0)
            return at + 2 <= size ? at + 2 : 0;
        if (length > 63)
            return 0;
        at += 1 + static_cast<size_t>(length);
    }
    return 0;
}

// Unpacks a name, following pointers, as dotted text.
bool read_name(uint8_t const* message, size_t size, size_t at, char* out, size_t room)
{
    size_t written = 0;
    int jumps = 0;
    while (at < size) {
        uint8_t length = message[at];
        if (length == 0) {
            if (written == 0) {
                if (room == 0)
                    return false;
                out[0] = '\0';
            } else {
                out[written - 1] = '\0';
            }
            return true;
        }
        if ((length & 0xc0) == 0xc0) {
            if (at + 2 > size || ++jumps > largest_redirects * 4)
                return false;
            at = static_cast<size_t>(length & 0x3f) << 8 | message[at + 1];
            continue;
        }
        if (length > 63 || at + 1 + length > size || written + length + 1 > room)
            return false;
        memcpy(out + written, message + at + 1, length);
        written += length;
        out[written++] = '.';
        at += 1 + static_cast<size_t>(length);
    }
    return false;
}

constexpr uint16_t type_a = 1;
constexpr uint16_t type_cname = 5;
constexpr uint16_t class_in = 1;

inline uint16_t read16(uint8_t const* at)
{
    return static_cast<uint16_t>(at[0] << 8 | at[1]);
}

enum class Answer {
    Found,
    NoSuchName,
    TryAnother,
};

// One answer: its A records for `name`, following CNAMEs within it.
Answer parse_answer(uint8_t const* message, size_t size, uint16_t identifier, char const* name, Addresses& addresses, char* redirected_to, size_t redirect_room)
{
    if (size < 12 || read16(message) != identifier || !(message[2] & 0x80))
        return Answer::TryAnother;
    int const code = message[3] & 0xf;
    if (code == 3)
        return Answer::NoSuchName;
    if (code != 0)
        return Answer::TryAnother;
    uint16_t const questions = read16(message + 4);
    uint16_t const answers = read16(message + 6);
    size_t at = 12;
    for (uint16_t i = 0; i < questions; ++i) {
        at = skip_name(message, size, at);
        if (at == 0 || at + 4 > size)
            return Answer::TryAnother;
        at += 4;
    }
    static char current[256];
    if (strlen(name) >= sizeof(current))
        return Answer::TryAnother;
    strcpy(current, name);
    redirected_to[0] = '\0';
    for (uint16_t i = 0; i < answers; ++i) {
        static char owner[256];
        if (!read_name(message, size, at, owner, sizeof(owner)))
            return Answer::TryAnother;
        at = skip_name(message, size, at);
        if (at == 0 || at + 10 > size)
            return Answer::TryAnother;
        uint16_t const type = read16(message + at);
        uint16_t const record_class = read16(message + at + 2);
        uint16_t const data_size = read16(message + at + 8);
        at += 10;
        if (at + data_size > size)
            return Answer::TryAnother;
        bool const for_current = same_name(owner, strlen(owner), current, strlen(current));
        if (record_class == class_in && for_current) {
            if (type == type_a && data_size == 4 && addresses.count < largest_addresses) {
                memcpy(&addresses.values[addresses.count++], message + at, 4);
            } else if (type == type_cname) {
                static char target[256];
                if (!read_name(message, size, at, target, sizeof(target)))
                    return Answer::TryAnother;
                strcpy(current, target);
                if (strlen(target) < redirect_room)
                    strcpy(redirected_to, target);
            }
        }
        at += data_size;
    }
    return addresses.count > 0 ? Answer::Found : Answer::TryAnother;
}

Answer ask_server(in_addr_t server, char const* name, Addresses& addresses, char* redirected_to, size_t redirect_room)
{
    uint8_t query[512];
    uint16_t identifier;
    if (getentropy(&identifier, sizeof(identifier)) != 0)
        return Answer::TryAnother;
    query[0] = static_cast<uint8_t>(identifier >> 8);
    query[1] = static_cast<uint8_t>(identifier);
    query[2] = 0x01; // Recursion, please.
    query[3] = 0;
    query[4] = 0;
    query[5] = 1;
    memset(query + 6, 0, 6);
    size_t encoded = encode_name(name, query + 12, sizeof(query) - 16);
    if (encoded == 0)
        return Answer::NoSuchName;
    size_t at = 12 + encoded;
    query[at++] = 0;
    query[at++] = static_cast<uint8_t>(type_a);
    query[at++] = 0;
    query[at++] = static_cast<uint8_t>(class_in);

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return Answer::TryAnother;
    sockaddr_in address {};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = server;
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return Answer::TryAnother;
    }
    Answer result = Answer::TryAnother;
    for (int attempt = 0; attempt < attempts_per_server && result == Answer::TryAnother; ++attempt) {
        if (send(fd, query, at, 0) < 0)
            break;
        pollfd poller { fd, POLLIN, 0 };
        if (poll(&poller, 1, milliseconds_per_attempt) <= 0)
            continue;
        uint8_t reply[1500];
        ssize_t got = recv(fd, reply, sizeof(reply), 0);
        if (got <= 0)
            continue;
        result = parse_answer(reply, static_cast<size_t>(got), identifier, name, addresses, redirected_to, redirect_room);
    }
    close(fd);
    return result;
}

int resolve_with_dns(char const* name, Addresses& addresses)
{
    in_addr_t servers[3];
    int server_count = read_name_servers(servers, 3);
    if (server_count == 0)
        return EAI_AGAIN;
    for (int i = 0; i < server_count; ++i) {
        char redirected[256];
        Answer answer = ask_server(servers[i], name, addresses, redirected, sizeof(redirected));
        if (answer == Answer::Found)
            return 0;
        if (answer == Answer::NoSuchName)
            return EAI_NONAME;
    }
    return EAI_AGAIN;
}

in_port_t port_for_service(char const* service, bool& ok)
{
    ok = true;
    if (!service)
        return 0;
    char* end;
    long number = strtol(service, &end, 10);
    if (*service && !*end && number >= 0 && number <= 65535)
        return htons(static_cast<uint16_t>(number));
    struct KnownService {
        char const* name;
        uint16_t port;
    };
    static KnownService const services[] = { { "http", 80 }, { "https", 443 }, { "domain", 53 }, { "ftp", 21 }, { "ssh", 22 }, { "smtp", 25 }, { "ntp", 123 } };
    for (auto const& known : services) {
        if (strcmp(known.name, service) == 0)
            return htons(known.port);
    }
    ok = false;
    return 0;
}

}

extern "C" {

int getaddrinfo(char const* node, char const* service, addrinfo const* hints, addrinfo** result)
{
    if (!node && !service)
        return EAI_NONAME;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    int flags = hints ? hints->ai_flags : 0;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;
    if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
        return EAI_SOCKTYPE;

    bool ok;
    in_port_t port = port_for_service(service, ok);
    if (!ok)
        return EAI_SERVICE;

    Addresses addresses;
    if (!node) {
        addresses.values[addresses.count++] = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    } else {
        in_addr_t numeric;
        if (inet_pton(AF_INET, node, &numeric) == 1) {
            addresses.values[addresses.count++] = numeric;
        } else if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        } else if (strcasecmp(node, "localhost") == 0) {
            addresses.values[addresses.count++] = htonl(INADDR_LOOPBACK);
        } else if (!look_in_hosts_file(node, addresses)) {
            int error = resolve_with_dns(node, addresses);
            if (error != 0)
                return error;
        }
    }

    // Each address once per kind of socket asked for: no kind means both.
    addrinfo* first = nullptr;
    addrinfo** tail = &first;
    int const kinds[] = { SOCK_STREAM, SOCK_DGRAM };
    for (int i = 0; i < addresses.count; ++i) {
        for (int kind : kinds) {
            if (socktype != 0 && socktype != kind)
                continue;
            auto* info = static_cast<addrinfo*>(calloc(1, sizeof(addrinfo) + sizeof(sockaddr_in)));
            if (!info) {
                freeaddrinfo(first);
                return EAI_MEMORY;
            }
            auto* address = reinterpret_cast<sockaddr_in*>(info + 1);
            address->sin_len = sizeof(sockaddr_in);
            address->sin_family = AF_INET;
            address->sin_port = port;
            address->sin_addr.s_addr = addresses.values[i];
            info->ai_family = AF_INET;
            info->ai_socktype = kind;
            info->ai_protocol = protocol ? protocol : (kind == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
            info->ai_addrlen = sizeof(sockaddr_in);
            info->ai_addr = reinterpret_cast<sockaddr*>(address);
            if ((flags & AI_CANONNAME) && !first && node)
                info->ai_canonname = strdup(node);
            *tail = info;
            tail = &info->ai_next;
        }
    }
    *result = first;
    return 0;
}

void freeaddrinfo(addrinfo* info)
{
    while (info) {
        addrinfo* next = info->ai_next;
        free(info->ai_canonname);
        free(info);
        info = next;
    }
}

char const* gai_strerror(int error)
{
    switch (error) {
    case 0:
        return "Success";
    case EAI_AGAIN:
        return "temporary failure in name resolution";
    case EAI_BADFLAGS:
        return "invalid value for ai_flags";
    case EAI_FAIL:
        return "non-recoverable failure in name resolution";
    case EAI_FAMILY:
        return "ai_family not supported";
    case EAI_MEMORY:
        return "memory allocation failure";
    case EAI_NONAME:
        return "nodename nor servname provided, or not known";
    case EAI_SERVICE:
        return "servname not supported for ai_socktype";
    case EAI_SOCKTYPE:
        return "ai_socktype not supported";
    case EAI_SYSTEM:
        return "system error";
    default:
        return "unknown error";
    }
}

hostent* gethostbyname(char const* name)
{
    static hostent entry;
    static char stored_name[256];
    static char* aliases[1];
    static in_addr_t values[largest_addresses];
    static char* list[largest_addresses + 1];
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &results) != 0)
        return nullptr;
    int count = 0;
    for (auto* info = results; info && count < largest_addresses; info = info->ai_next) {
        values[count] = reinterpret_cast<sockaddr_in*>(info->ai_addr)->sin_addr.s_addr;
        list[count] = reinterpret_cast<char*>(&values[count]);
        ++count;
    }
    list[count] = nullptr;
    freeaddrinfo(results);
    strncpy(stored_name, name, sizeof(stored_name) - 1);
    entry.h_name = stored_name;
    entry.h_aliases = aliases;
    entry.h_addrtype = AF_INET;
    entry.h_length = 4;
    entry.h_addr_list = list;
    return &entry;
}

}
