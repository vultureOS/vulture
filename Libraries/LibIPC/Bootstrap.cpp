/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <BootstrapEndpoint.h>
#include <LibIPC/Bootstrap.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/port.h>

namespace IPC {

ErrorOr<Port> bootstrap_port()
{
    int fd = bootstrap_port_get(O_CLOEXEC);
    if (fd < 0)
        return Error::from_errno(errno);
    return Port(fd);
}

ErrorOr<Port> look_up(StringView name)
{
    BootstrapProxy bootstrap(TRY(bootstrap_port()));
    auto answer = TRY(bootstrap.look_up(name));
    if (answer.error)
        return Error::from_errno(answer.error);
    return move(answer.service);
}

ErrorOr<Port> check_in(StringView name)
{
    BootstrapProxy bootstrap(TRY(bootstrap_port()));
    auto answer = TRY(bootstrap.check_in(name));
    if (answer.error)
        return Error::from_errno(answer.error);
    return move(answer.service);
}

ErrorOr<void> register_service(StringView name, Port send_right)
{
    BootstrapProxy bootstrap(TRY(bootstrap_port()));
    auto error = TRY(bootstrap.register_service(name, move(send_right)));
    if (error)
        return Error::from_errno(error);
    return {};
}

}
