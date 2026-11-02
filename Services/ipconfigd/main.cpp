/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/socket.h>
#include <LibBase/Vector.h>
#include <LibCore/EventLoop.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <notify.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

// DHCP (RFC 2131) for en0: ask, take the offer, keep renewing. What it learns goes to the
// interface, and to /etc/resolv.conf for the resolver; and interested programs are told by the
// notification com.vulture.network.changed. With nothing to talk to, it keeps asking, less
// and less often.

namespace {

constexpr u16 server_port = 67;
constexpr u16 client_port = 68;
constexpr u32 magic_cookie = 0x63825363;

constexpr u8 option_subnet_mask = 1;
constexpr u8 option_router = 3;
constexpr u8 option_dns = 6;
constexpr u8 option_hostname = 12;
constexpr u8 option_requested_address = 50;
constexpr u8 option_lease_time = 51;
constexpr u8 option_message_type = 53;
constexpr u8 option_server_identifier = 54;
constexpr u8 option_parameter_list = 55;
constexpr u8 option_end = 255;

constexpr u8 message_discover = 1;
constexpr u8 message_offer = 2;
constexpr u8 message_request = 3;
constexpr u8 message_ack = 5;
constexpr u8 message_nak = 6;

struct [[gnu::packed]] Message {
    u8 operation;
    u8 hardware_type;
    u8 hardware_length;
    u8 hops;
    u32 transaction;
    u16 seconds;
    u16 flags;
    u32 client_address;
    u32 your_address;
    u32 server_address;
    u32 gateway_address;
    u8 client_hardware[16];
    u8 server_name[64];
    u8 boot_file[128];
    u32 cookie;
    u8 options[312];
};

struct Lease {
    u32 address { 0 };
    u32 netmask { 0 };
    u32 router { 0 };
    u32 server { 0 };
    u32 seconds { 0 };
    Vector<u32> name_servers;
};

class Client {
public:
    Client(Core::EventLoop& loop, char const* interface_name, u8 const* mac, unsigned interface_index)
        : m_loop(loop)
        , m_log(os_log_create("com.vulture.ipconfigd", "dhcp"))
        , m_interface_index(interface_index)
    {
        strncpy(m_name, interface_name, sizeof(m_name) - 1);
        memcpy(m_mac, mac, 6);
    }

    bool start()
    {
        m_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (m_fd < 0)
            return false;
        int on = 1;
        setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        int index = static_cast<int>(m_interface_index);
        setsockopt(m_fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index));
        sockaddr_in address {};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_port = htons(client_port);
        if (bind(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            os_log_error(m_log, "bind: %s", strerror(errno));
            return false;
        }
        m_loop.watch(m_fd, Core::EventLoop::Interest::Read, [this] { receive(); });
        discover();
        return true;
    }

private:
    enum class State {
        Selecting,
        Requesting,
        Bound,
        Renewing,
    };

    void cancel_timer()
    {
        if (m_timer >= 0)
            m_loop.cancel_timer(m_timer);
        m_timer = -1;
    }

    void again_after(u64 milliseconds, void (Client::*what)())
    {
        cancel_timer();
        m_timer = m_loop.add_timer(milliseconds, false, [this, what] {
            m_timer = -1;
            (this->*what)();
        });
    }

    void discover()
    {
        m_state = State::Selecting;
        getentropy(&m_transaction, sizeof(m_transaction));
        Message message;
        size_t size = begin_message(message, message_discover);
        append_parameter_list(message, size);
        finish_message(message, size);
        send(message, size, INADDR_BROADCAST);
        back_off(&Client::discover);
    }

    void request()
    {
        Message message;
        size_t size = begin_message(message, message_request);
        append_option(message, size, option_requested_address, &m_offered_address, 4);
        append_option(message, size, option_server_identifier, &m_offered_server, 4);
        append_parameter_list(message, size);
        finish_message(message, size);
        send(message, size, INADDR_BROADCAST);
        if (++m_request_attempts > 4) {
            discover();
            return;
        }
        back_off(&Client::request);
    }

    void renew()
    {
        m_state = State::Renewing;
        m_request_attempts = 0;
        getentropy(&m_transaction, sizeof(m_transaction));
        Message message;
        size_t size = begin_message(message, message_request);
        message.client_address = m_lease.address;
        finish_message(message, size);
        send(message, size, m_lease.server ? ntohl(m_lease.server) : INADDR_BROADCAST);
        // Half of what is left, as long as that is worth it; then start over.
        m_renew_wait_seconds /= 2;
        if (m_renew_wait_seconds < 60) {
            os_log_info(m_log, "%s: the lease could not be renewed", m_name);
            discover();
            return;
        }
        again_after(static_cast<u64>(m_renew_wait_seconds) * 1000, &Client::renew);
    }

    void back_off(void (Client::*what)())
    {
        if (m_retry_seconds == 0)
            m_retry_seconds = 2;
        again_after(static_cast<u64>(m_retry_seconds) * 1000, what);
        if (m_retry_seconds < 64)
            m_retry_seconds *= 2;
    }

    size_t begin_message(Message& message, u8 type)
    {
        memset(&message, 0, sizeof(message));
        message.operation = 1;
        message.hardware_type = 1;
        message.hardware_length = 6;
        message.transaction = m_transaction;
        // Ask for replies by broadcast: there's no address yet to reply to.
        message.flags = htons(0x8000);
        memcpy(message.client_hardware, m_mac, 6);
        message.cookie = htonl(magic_cookie);
        size_t size = 0;
        append_option(message, size, option_message_type, &type, 1);
        char hostname[64];
        if (gethostname(hostname, sizeof(hostname)) == 0 && hostname[0])
            append_option(message, size, option_hostname, hostname, strlen(hostname));
        return size;
    }

    static void append_option(Message& message, size_t& size, u8 code, void const* data, size_t length)
    {
        if (size + 2 + length >= sizeof(message.options))
            return;
        message.options[size++] = code;
        message.options[size++] = static_cast<u8>(length);
        memcpy(message.options + size, data, length);
        size += length;
    }

    static void append_parameter_list(Message& message, size_t& size)
    {
        u8 const wanted[] = { option_subnet_mask, option_router, option_dns, option_lease_time };
        append_option(message, size, option_parameter_list, wanted, sizeof(wanted));
    }

    static void finish_message(Message& message, size_t& size)
    {
        message.options[size++] = option_end;
    }

    void send(Message const& message, size_t options_size, u32 destination)
    {
        sockaddr_in address {};
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_port = htons(server_port);
        address.sin_addr.s_addr = htonl(destination);
        size_t size = sizeof(Message) - sizeof(message.options) + options_size;
        // At least as large as BOOTP's message, as some servers insist.
        size = max<size_t>(size, 300);
        if (sendto(m_fd, &message, size, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            os_log_debug(m_log, "%s: send: %s", m_name, strerror(errno));
    }

    void receive()
    {
        Message reply {};
        ssize_t got = recv(m_fd, &reply, sizeof(reply), 0);
        size_t const fixed = sizeof(Message) - sizeof(reply.options);
        if (got < static_cast<ssize_t>(fixed) || reply.operation != 2 || reply.transaction != m_transaction || ntohl(reply.cookie) != magic_cookie)
            return;
        if (memcmp(reply.client_hardware, m_mac, 6) != 0)
            return;
        u8 type = 0;
        Lease lease;
        lease.address = reply.your_address;
        size_t const options_size = static_cast<size_t>(got) - fixed;
        for (size_t at = 0; at < options_size;) {
            u8 code = reply.options[at];
            if (code == option_end)
                break;
            if (code == 0) {
                ++at;
                continue;
            }
            if (at + 2 > options_size)
                break;
            u8 length = reply.options[at + 1];
            if (at + 2 + length > options_size)
                break;
            u8 const* data = reply.options + at + 2;
            switch (code) {
            case option_message_type:
                if (length == 1)
                    type = data[0];
                break;
            case option_subnet_mask:
                if (length == 4)
                    memcpy(&lease.netmask, data, 4);
                break;
            case option_router:
                if (length >= 4)
                    memcpy(&lease.router, data, 4);
                break;
            case option_dns:
                for (u8 i = 0; i + 4 <= length; i += 4) {
                    u32 server;
                    memcpy(&server, data + i, 4);
                    lease.name_servers.append(server);
                }
                break;
            case option_lease_time:
                if (length == 4) {
                    memcpy(&lease.seconds, data, 4);
                    lease.seconds = ntohl(lease.seconds);
                }
                break;
            case option_server_identifier:
                if (length == 4)
                    memcpy(&lease.server, data, 4);
                break;
            default:
                break;
            }
            at += 2 + static_cast<size_t>(length);
        }

        if (m_state == State::Selecting && type == message_offer) {
            m_offered_address = lease.address;
            m_offered_server = lease.server;
            m_state = State::Requesting;
            m_retry_seconds = 0;
            m_request_attempts = 0;
            request();
            return;
        }
        if ((m_state == State::Requesting || m_state == State::Renewing) && type == message_ack) {
            bound(lease);
            return;
        }
        if ((m_state == State::Requesting || m_state == State::Renewing) && type == message_nak) {
            os_log(m_log, "%s: the server refused the address", m_name);
            m_retry_seconds = 0;
            discover();
        }
    }

    void bound(Lease& lease)
    {
        bool const changed = lease.address != m_lease.address || lease.router != m_lease.router;
        m_lease = move(lease);
        m_state = State::Bound;
        m_retry_seconds = 0;
        if (changed)
            apply();
        // Renew at half the lease. (A lease forever needs no renewing.)
        cancel_timer();
        if (m_lease.seconds != 0xffffffff) {
            u32 seconds = m_lease.seconds ? m_lease.seconds : 3600;
            m_renew_wait_seconds = seconds / 2;
            again_after(static_cast<u64>(m_renew_wait_seconds) * 1000, &Client::renew);
        }
    }

    void apply()
    {
        vulture_interface_config config {};
        strncpy(config.name, m_name, sizeof(config.name) - 1);
        config.address = m_lease.address;
        config.netmask = m_lease.netmask;
        config.gateway = m_lease.router;
        if (ioctl(m_fd, VULTURE_SIOCSIFCONFIG, &config) != 0) {
            os_log_error(m_log, "%s: can't set the address: %s", m_name, strerror(errno));
            return;
        }
        write_resolver_file();
        char address_text[INET_ADDRSTRLEN];
        char router_text[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &m_lease.address, address_text, sizeof(address_text));
        inet_ntop(AF_INET, &m_lease.router, router_text, sizeof(router_text));
        printf("ipconfigd: %s: address %s, router %s, lease %u seconds\n", m_name, address_text, router_text, m_lease.seconds);
        fflush(stdout);
        os_log(m_log, "%s: address %s, router %s, lease %u seconds", m_name, address_text, router_text, m_lease.seconds);
        notify_post("com.vulture.network.changed");
    }

    void write_resolver_file()
    {
        char const* temporary = "/etc/resolv.conf.new";
        int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            os_log_error(m_log, "/etc/resolv.conf: %s", strerror(errno));
            return;
        }
        dprintf(fd, "#\n# Written by ipconfigd: changes here last until the next lease.\n#\n");
        for (auto server : m_lease.name_servers) {
            char text[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &server, text, sizeof(text));
            dprintf(fd, "nameserver %s\n", text);
        }
        close(fd);
        rename(temporary, "/etc/resolv.conf");
    }

    Core::EventLoop& m_loop;
    os_log_t m_log;
    char m_name[VULTURE_IFNAMSIZ] {};
    u8 m_mac[6] {};
    unsigned m_interface_index;
    int m_fd { -1 };
    int m_timer { -1 };
    State m_state { State::Selecting };
    u32 m_transaction { 0 };
    u32 m_offered_address { 0 };
    u32 m_offered_server { 0 };
    u32 m_retry_seconds { 0 };
    u32 m_renew_wait_seconds { 0 };
    int m_request_attempts { 0 };
    Lease m_lease;
};

}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    vulture_interface_info interfaces[8];
    size_t size = sizeof(interfaces);
    if (sysctlbyname("net.interfaces", interfaces, &size, nullptr, 0) != 0)
        return 1;

    Core::EventLoop loop;
    Vector<Client*> clients;
    for (size_t i = 0; i < size / sizeof(interfaces[0]); ++i) {
        if (interfaces[i].flags & VULTURE_IFF_LOOPBACK)
            continue;
        auto* client = new Client(loop, interfaces[i].name, interfaces[i].mac, static_cast<unsigned>(i + 1));
        if (client->start())
            clients.append(client);
    }
    // Nothing to do without an interface. (They don't come and go, yet.)
    return loop.exec();
}
