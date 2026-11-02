/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Test.h"
#include <Kernel/API/POSIX/graphics.h>
#include <Kernel/API/POSIX/input.h>
#include <LibIPC/Message.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/vulture.h>
#include <sys/wait.h>
#include <unistd.h>

// The display, the input devices and shared memory, as the window server and
// its clients use them.

namespace {

void test_display()
{
    int gpu = open("/dev/gpu0", O_RDWR | O_CLOEXEC);
    CHECK(gpu >= 0);
    if (gpu < 0)
        return;
    vulture_gpu_mode display {};
    CHECK(ioctl(gpu, VULTURE_GPU_GET_DISPLAY, &display) == 0);
    CHECK(display.width > 0 && display.height > 0);
    CHECK(display.pitch == display.width * 4);

    // Flushing before there is a mode: refused. Unknown requests too.
    vulture_gpu_rect all { 0, 0, 16, 16 };
    CHECK(ioctl(gpu, VULTURE_GPU_FLUSH, &all) == -1 && errno == EINVAL);
    CHECK(ioctl(gpu, 0x4799, nullptr) == -1 && errno == ENOTTY);
    vulture_gpu_mode bad { 0, 480, 0, 0 };
    CHECK(ioctl(gpu, VULTURE_GPU_SET_MODE, &bad) == -1 && errno == EINVAL);

    // Most displays take any size. What the firmware left has the size it's in, and nothing else.
    vulture_gpu_mode mode { 640, 480, 0, 0 };
    bool const has_one_size = ioctl(gpu, VULTURE_GPU_SET_MODE, &mode) == -1 && errno == EINVAL;
    if (has_one_size) {
        mode = { display.width, display.height, 0, 0 };
        CHECK(ioctl(gpu, VULTURE_GPU_SET_MODE, &mode) == 0);
    }
    unsigned const width = mode.width;
    unsigned const height = mode.height;
    CHECK(mode.pitch == width * 4 && mode.size == width * 4 * height);
    auto* pixels = static_cast<unsigned*>(mmap(nullptr, mode.size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, 0));
    CHECK(pixels != MAP_FAILED);
    if (pixels != MAP_FAILED) {
        CHECK(pixels[0] == 0);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x)
                pixels[y * width + x] = 0xff000000 | (x & 255) << 16 | (y & 255) << 8;
        }
        vulture_gpu_rect whole { 0, 0, width, height };
        CHECK(ioctl(gpu, VULTURE_GPU_FLUSH, &whole) == 0);
        // Partly outside is cut down; wholly outside does nothing.
        vulture_gpu_rect partly { width - 40, height - 80, 500, 500 };
        CHECK(ioctl(gpu, VULTURE_GPU_FLUSH, &partly) == 0);
        vulture_gpu_rect outside { 5000, 5000, 5, 5 };
        CHECK(ioctl(gpu, VULTURE_GPU_FLUSH, &outside) == 0);
    }

    // A second mode: the old mapping stays good, if no longer shown.
    if (!has_one_size) {
        vulture_gpu_mode second { 320, 200, 0, 0 };
        CHECK(ioctl(gpu, VULTURE_GPU_SET_MODE, &second) == 0);
    }
    if (pixels != MAP_FAILED) {
        CHECK(pixels[5 * width + 7] == (0xff000000u | 7 << 16 | 5 << 8));
        munmap(pixels, mode.size);
    }

    static unsigned cursor[VULTURE_GPU_CURSOR_SIZE * VULTURE_GPU_CURSOR_SIZE];
    cursor[0] = 0xffffffff;
    vulture_gpu_cursor image { cursor, 0, 0 };
    CHECK(ioctl(gpu, VULTURE_GPU_SET_CURSOR, &image) == 0);
    vulture_gpu_position position { 10, 20 };
    CHECK(ioctl(gpu, VULTURE_GPU_MOVE_CURSOR, &position) == 0);
    vulture_gpu_cursor hot_outside { cursor, 64, 0 };
    CHECK(ioctl(gpu, VULTURE_GPU_SET_CURSOR, &hot_outside) == -1 && errno == EINVAL);
    vulture_gpu_cursor bad_pointer { reinterpret_cast<void const*>(8), 0, 0 };
    CHECK(ioctl(gpu, VULTURE_GPU_SET_CURSOR, &bad_pointer) == -1 && errno == EFAULT);

    // Beyond the framebuffer a mapping faults rather than reaching anything.
    int status = 0;
    pid_t child = fork();
    if (child == 0) {
        size_t const past_the_end = mode.size + 1024 * 1024;
        auto* beyond = static_cast<volatile unsigned*>(mmap(nullptr, past_the_end, PROT_READ, MAP_SHARED, gpu, 0));
        unsigned value = beyond[past_the_end / 4 - 1];
        _exit(value == 12345 ? 1 : 2);
    }
    waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status));
    close(gpu);
}

void test_input()
{
    int input = open("/dev/input", O_RDONLY | O_CLOEXEC);
    CHECK(input >= 0);
    if (input < 0)
        return;
    // One client at a time.
    CHECK(open("/dev/input", O_RDONLY) == -1 && errno == EBUSY);
    vulture_input_event event;
    // Nothing typed yet: no events, and poll agrees.
    CHECK(read(input, &event, sizeof(event)) == -1 && errno == EAGAIN);
    CHECK(read(input, &event, 3) == -1 && errno == EINVAL);
    pollfd watched { input, POLLIN, 0 };
    CHECK(poll(&watched, 1, 0) == 0);
    close(input);
    // Closed, it can be opened again.
    int again = open("/dev/input", O_RDONLY | O_CLOEXEC);
    CHECK(again >= 0);
    close(again);
}

void test_shared_memory()
{
    CHECK(vulture_shared_memory_create(0, O_CLOEXEC) == -1 && errno == EINVAL);
    constexpr size_t size = 1024 * 1024;
    int fd = vulture_shared_memory_create(size, O_CLOEXEC);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    auto* memory = static_cast<volatile unsigned*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    CHECK(memory != MAP_FAILED);
    CHECK(memory[100] == 0);

    // Across a fork: writes are seen both ways, unlike a private mapping.
    pid_t child = fork();
    if (child == 0) {
        memory[100] = 111;
        while (memory[200] != 222)
            ;
        _exit(0);
    }
    while (memory[100] != 111)
        ;
    memory[200] = 222;
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // To a process that did not inherit it: through a port, then mapped there.
    auto ports = IPC::create_port();
    CHECK(!ports.is_error());
    if (!ports.is_error()) {
        child = fork();
        if (child == 0) {
            auto message = IPC::receive(ports.value().receive);
            if (message.is_error() || message.value().rights.size() != 1)
                _exit(1);
            auto* mapped = static_cast<volatile unsigned*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, message.value().rights[0].fd(), 0));
            if (mapped == MAP_FAILED || mapped[100] != 111)
                _exit(2);
            // At the far end, for a page not touched so far.
            mapped[size / 4 - 1] = 333;
            _exit(0);
        }
        IPC::Encoder encoder(1, 1);
        int copy = dup(fd);
        encoder.encode(IPC::Port(copy));
        CHECK(!IPC::send(ports.value().send, encoder).is_error());
        waitpid(child, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(memory[size / 4 - 1] == 333);
    }

    CHECK(lseek(fd, 0, SEEK_END) == static_cast<off_t>(size));
    munmap(const_cast<unsigned*>(memory), size);
    close(fd);
}

}

int main()
{
    test_display();
    test_input();
    test_shared_memory();
    return finish();
}
