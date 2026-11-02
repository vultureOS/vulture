/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Lock.h"
#include <LibSystem/ThreadPointer.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum class Buffering {
    None,
    Line,
    Full,
};

struct FILE {
    int fd;
    Buffering buffering;
    bool error;
    bool eof;
    size_t write_length;
    size_t read_position;
    size_t read_length;
    char write_buffer[BUFSIZ];
    char read_buffer[BUFSIZ];
    // Opened by fopen/fdopen/popen: in the list flushed at exit.
    FILE* next;
    pid_t child; // popen()'s.
    // Each stdio call holds this while it works (flockfile() holds it across
    // calls: it may be taken again by the thread that has it).
    LibC::Lock lock {};
    void* lock_owner { nullptr };
    unsigned lock_depth { 0 };
};

static FILE s_stdin { 0, Buffering::Line, false, false, 0, 0, 0, {}, {}, nullptr, 0 };
static FILE s_stdout { 1, Buffering::Line, false, false, 0, 0, 0, {}, {}, nullptr, 0 };
static FILE s_stderr { 2, Buffering::None, false, false, 0, 0, 0, {}, {}, nullptr, 0 };
static FILE* s_open_files = nullptr;
static LibC::Lock s_open_files_lock;

static void* current_thread_identity()
{
    return System::thread_control_block();
}

static bool file_is_mine(FILE* file)
{
    return __atomic_load_n(&file->lock_owner, __ATOMIC_RELAXED) == current_thread_identity();
}

static void lock_file(FILE* file)
{
    if (!file_is_mine(file)) {
        file->lock.lock();
        __atomic_store_n(&file->lock_owner, current_thread_identity(), __ATOMIC_RELAXED);
    }
    ++file->lock_depth;
}

static bool try_lock_file(FILE* file)
{
    if (!file_is_mine(file)) {
        if (!file->lock.try_lock())
            return false;
        __atomic_store_n(&file->lock_owner, current_thread_identity(), __ATOMIC_RELAXED);
    }
    ++file->lock_depth;
    return true;
}

static void unlock_file(FILE* file)
{
    if (--file->lock_depth == 0) {
        __atomic_store_n(&file->lock_owner, static_cast<void*>(nullptr), __ATOMIC_RELAXED);
        file->lock.unlock();
    }
}

namespace {

class FileLocker {
public:
    explicit FileLocker(FILE* file)
        : m_file(file)
    {
        lock_file(m_file);
    }
    ~FileLocker() { unlock_file(m_file); }
    FileLocker(FileLocker const&) = delete;
    FileLocker& operator=(FileLocker const&) = delete;

private:
    FILE* m_file;
};

}

extern "C" {
FILE* stdin = &s_stdin;
FILE* stdout = &s_stdout;
FILE* stderr = &s_stderr;
}

static bool write_all(int fd, char const* data, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written <= 0)
            return false;
        data += written;
        length -= static_cast<size_t>(written);
    }
    return true;
}

static int flush(FILE* file)
{
    if (file->write_length == 0)
        return 0;
    bool ok = write_all(file->fd, file->write_buffer, file->write_length);
    file->write_length = 0;
    if (!ok) {
        file->error = true;
        return EOF;
    }
    return 0;
}

static void put(FILE* file, char const* data, size_t length)
{
    if (file->buffering == Buffering::None) {
        if (!write_all(file->fd, data, length))
            file->error = true;
        return;
    }
    // What wouldn't fit anyway goes straight out, in one piece: a disk could otherwise see every block in parts.
    if (length >= BUFSIZ) {
        if (flush(file) != 0 || !write_all(file->fd, data, length))
            file->error = true;
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        file->write_buffer[file->write_length++] = data[i];
        if (file->write_length == BUFSIZ || (file->buffering == Buffering::Line && data[i] == '\n'))
            flush(file);
    }
}

// --- The formatting engine behind every printf variant. ---

namespace {

struct Output {
    void (*emit)(void* context, char const* data, size_t length);
    void* context;
    size_t total;

    void write(char const* data, size_t length)
    {
        emit(context, data, length);
        total += length;
    }

    void repeat(char character, int count)
    {
        for (int i = 0; i < count; ++i)
            write(&character, 1);
    }
};

struct Spec {
    bool left = false;
    bool zero = false;
    bool plus = false;
    bool space = false;
    bool alternate = false;
    int width = 0;
    int precision = -1;
};

void write_padded(Output& out, Spec const& spec, char const* prefix, char const* body, size_t body_length, bool numeric)
{
    size_t prefix_length = strlen(prefix);
    int padding = spec.width - static_cast<int>(prefix_length + body_length);
    if (padding < 0)
        padding = 0;
    if (!spec.left && !(spec.zero && numeric))
        out.repeat(' ', padding);
    out.write(prefix, prefix_length);
    if (!spec.left && spec.zero && numeric)
        out.repeat('0', padding);
    out.write(body, body_length);
    if (spec.left)
        out.repeat(' ', padding);
}

void write_unsigned(Output& out, Spec spec, uintmax_t value, unsigned base, bool uppercase, char const* prefix)
{
    char digits[64];
    size_t length = 0;
    char const* alphabet = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        digits[length++] = alphabet[value % base];
        value /= base;
    } while (value);
    // A precision is a minimum digit count (and disables zero padding).
    while (spec.precision >= 0 && length < static_cast<size_t>(spec.precision) && length < sizeof(digits))
        digits[length++] = '0';
    if (spec.precision >= 0)
        spec.zero = false;
    char body[64];
    for (size_t i = 0; i < length; ++i)
        body[i] = digits[length - 1 - i];
    write_padded(out, spec, prefix, body, length, true);
}

void write_double(Output& out, Spec spec, double value)
{
    char const* sign = value < 0 ? "-" : (spec.plus ? "+" : (spec.space ? " " : ""));
    if (value < 0)
        value = -value;
    if (value != value) {
        write_padded(out, spec, sign, "nan", 3, false);
        return;
    }
    if (value > 1.7976931348623157e308) {
        write_padded(out, spec, sign, "inf", 3, false);
        return;
    }
    int precision = spec.precision < 0 ? 6 : (spec.precision > 18 ? 18 : spec.precision);
    double scale = 1;
    for (int i = 0; i < precision; ++i)
        scale *= 10;
    double rounded = value + 0.5 / scale;
    // Values beyond 2^64 print their integer part as an approximation.
    uintmax_t whole = rounded >= 1.8e19 ? UINTMAX_MAX : static_cast<uintmax_t>(rounded);
    double fraction = rounded - static_cast<double>(whole);
    char body[96];
    size_t length = 0;
    char reversed[32];
    size_t digits = 0;
    do {
        reversed[digits++] = static_cast<char>('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (digits)
        body[length++] = reversed[--digits];
    if (precision > 0 || spec.alternate)
        body[length++] = '.';
    for (int i = 0; i < precision; ++i) {
        fraction *= 10;
        int digit = static_cast<int>(fraction);
        if (digit > 9)
            digit = 9;
        body[length++] = static_cast<char>('0' + digit);
        fraction -= digit;
    }
    write_padded(out, spec, sign, body, length, true);
}

int format(Output& out, char const* format, va_list arguments)
{
    for (char const* cursor = format; *cursor; ++cursor) {
        if (*cursor != '%') {
            char const* run = cursor;
            while (cursor[1] && cursor[1] != '%')
                ++cursor;
            out.write(run, static_cast<size_t>(cursor - run + 1));
            continue;
        }
        ++cursor;
        Spec spec;
        for (;; ++cursor) {
            if (*cursor == '-')
                spec.left = true;
            else if (*cursor == '0')
                spec.zero = true;
            else if (*cursor == '+')
                spec.plus = true;
            else if (*cursor == ' ')
                spec.space = true;
            else if (*cursor == '#')
                spec.alternate = true;
            else
                break;
        }
        if (*cursor == '*') {
            spec.width = va_arg(arguments, int);
            if (spec.width < 0) {
                spec.left = true;
                spec.width = -spec.width;
            }
            ++cursor;
        } else {
            while (*cursor >= '0' && *cursor <= '9')
                spec.width = spec.width * 10 + (*cursor++ - '0');
        }
        if (*cursor == '.') {
            ++cursor;
            spec.precision = 0;
            if (*cursor == '*') {
                spec.precision = va_arg(arguments, int);
                ++cursor;
            } else {
                while (*cursor >= '0' && *cursor <= '9')
                    spec.precision = spec.precision * 10 + (*cursor++ - '0');
            }
        }
        int longs = 0;
        bool size_type = false;
        bool half = false;
        for (;; ++cursor) {
            if (*cursor == 'l')
                ++longs;
            else if (*cursor == 'z' || *cursor == 't' || *cursor == 'j')
                size_type = true;
            else if (*cursor == 'h')
                half = true;
            else
                break;
        }
        (void)half;
        bool wide = longs > 0 || size_type;

        switch (*cursor) {
        case 'd':
        case 'i': {
            intmax_t value = wide ? (longs > 1 ? va_arg(arguments, long long) : va_arg(arguments, long)) : va_arg(arguments, int);
            char const* sign = value < 0 ? "-" : (spec.plus ? "+" : (spec.space ? " " : ""));
            uintmax_t magnitude = value < 0 ? uintmax_t(0) - static_cast<uintmax_t>(value) : static_cast<uintmax_t>(value);
            write_unsigned(out, spec, magnitude, 10, false, sign);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uintmax_t value = wide ? (longs > 1 ? va_arg(arguments, unsigned long long) : va_arg(arguments, unsigned long)) : va_arg(arguments, unsigned);
            unsigned base = *cursor == 'u' ? 10 : (*cursor == 'o' ? 8 : 16);
            char const* prefix = "";
            if (spec.alternate && value != 0)
                prefix = *cursor == 'x' ? "0x" : (*cursor == 'X' ? "0X" : (*cursor == 'o' ? "0" : ""));
            write_unsigned(out, spec, value, base, *cursor == 'X', prefix);
            break;
        }
        case 'p': {
            auto value = reinterpret_cast<uintptr_t>(va_arg(arguments, void*));
            write_unsigned(out, spec, value, 16, false, "0x");
            break;
        }
        case 'c': {
            char character = static_cast<char>(va_arg(arguments, int));
            write_padded(out, spec, "", &character, 1, false);
            break;
        }
        case 's': {
            char const* string = va_arg(arguments, char const*);
            if (!string)
                string = "(null)";
            size_t length = spec.precision >= 0 ? strnlen(string, static_cast<size_t>(spec.precision)) : strlen(string);
            write_padded(out, spec, "", string, length, false);
            break;
        }
        case 'f':
        case 'F':
        case 'g':
        case 'e':
            write_double(out, spec, va_arg(arguments, double));
            break;
        case '%':
            out.write("%", 1);
            break;
        case '\0':
            return static_cast<int>(out.total);
        default:
            out.write(cursor - 1, 2);
            break;
        }
    }
    return static_cast<int>(out.total);
}

struct StringOutput {
    char* buffer;
    size_t capacity;
    size_t length;
};

void emit_to_string(void* context, char const* data, size_t length)
{
    auto& output = *static_cast<StringOutput*>(context);
    for (size_t i = 0; i < length; ++i) {
        if (output.length + 1 < output.capacity)
            output.buffer[output.length] = data[i];
        ++output.length;
    }
}

void emit_to_file(void* context, char const* data, size_t length)
{
    put(static_cast<FILE*>(context), data, length);
}

}

extern "C" {

// Some thread may hold a file for long, reading a terminal say: that file
// has to go without rather than this wait for it.
static void flush_unless_held(FILE* file)
{
    for (int attempt = 0; attempt < 256; ++attempt) {
        if (try_lock_file(file)) {
            flush(file);
            unlock_file(file);
            return;
        }
        sched_yield();
    }
}

void __stdio_flush_all()
{
    flush_unless_held(stdout);
    flush_unless_held(stderr);
    LibC::LockGuard guard(s_open_files_lock);
    for (FILE* file = s_open_files; file; file = file->next)
        flush_unless_held(file);
}

void __stdio_fork_lock()
{
    s_open_files_lock.lock();
}

// In the child, the other threads are gone with the files they held.
static void release_if_held_by_others(FILE* file)
{
    if (!file_is_mine(file)) {
        file->lock.reset();
        file->lock_owner = nullptr;
        file->lock_depth = 0;
    }
}

void __stdio_fork_unlock(int child)
{
    if (child) {
        release_if_held_by_others(stdin);
        release_if_held_by_others(stdout);
        release_if_held_by_others(stderr);
        for (FILE* file = s_open_files; file; file = file->next)
            release_if_held_by_others(file);
    }
    s_open_files_lock.unlock();
}

void flockfile(FILE* file)
{
    lock_file(file);
}

int ftrylockfile(FILE* file)
{
    return try_lock_file(file) ? 0 : 1;
}

void funlockfile(FILE* file)
{
    unlock_file(file);
}

int vsnprintf(char* buffer, size_t size, char const* format_string, va_list arguments)
{
    StringOutput output { buffer, size, 0 };
    Output out { emit_to_string, &output, 0 };
    int length = format(out, format_string, arguments);
    if (size > 0)
        buffer[output.length < size ? output.length : size - 1] = '\0';
    return length;
}

int snprintf(char* buffer, size_t size, char const* format_string, ...)
{
    va_list arguments;
    va_start(arguments, format_string);
    int length = vsnprintf(buffer, size, format_string, arguments);
    va_end(arguments);
    return length;
}

int vsprintf(char* buffer, char const* format_string, va_list arguments)
{
    return vsnprintf(buffer, SIZE_MAX, format_string, arguments);
}

int sprintf(char* buffer, char const* format_string, ...)
{
    va_list arguments;
    va_start(arguments, format_string);
    int length = vsprintf(buffer, format_string, arguments);
    va_end(arguments);
    return length;
}

int vfprintf(FILE* file, char const* format_string, va_list arguments)
{
    FileLocker locker(file);
    Output out { emit_to_file, file, 0 };
    return format(out, format_string, arguments);
}

int fprintf(FILE* file, char const* format_string, ...)
{
    va_list arguments;
    va_start(arguments, format_string);
    int length = vfprintf(file, format_string, arguments);
    va_end(arguments);
    return length;
}

int vprintf(char const* format_string, va_list arguments)
{
    return vfprintf(stdout, format_string, arguments);
}

int printf(char const* format_string, ...)
{
    va_list arguments;
    va_start(arguments, format_string);
    int length = vfprintf(stdout, format_string, arguments);
    va_end(arguments);
    return length;
}

int dprintf(int fd, char const* format_string, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format_string);
    int length = vsnprintf(buffer, sizeof(buffer), format_string, arguments);
    va_end(arguments);
    size_t to_write = length < static_cast<int>(sizeof(buffer)) ? static_cast<size_t>(length) : sizeof(buffer) - 1;
    write_all(fd, buffer, to_write);
    return length;
}

int putc_unlocked(int character, FILE* file)
{
    char c = static_cast<char>(character);
    put(file, &c, 1);
    return file->error ? EOF : static_cast<unsigned char>(c);
}

int putchar_unlocked(int character)
{
    return putc_unlocked(character, stdout);
}

int fputc(int character, FILE* file)
{
    FileLocker locker(file);
    return putc_unlocked(character, file);
}

int putc(int character, FILE* file)
{
    return fputc(character, file);
}

int putchar(int character)
{
    return fputc(character, stdout);
}

int fputs(char const* string, FILE* file)
{
    FileLocker locker(file);
    put(file, string, strlen(string));
    return file->error ? EOF : 0;
}

int puts(char const* string)
{
    FileLocker locker(stdout);
    fputs(string, stdout);
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

size_t fwrite(void const* buffer, size_t size, size_t count, FILE* file)
{
    FileLocker locker(file);
    put(file, static_cast<char const*>(buffer), size * count);
    return file->error ? 0 : count;
}

static bool fill(FILE* file)
{
    ssize_t result = read(file->fd, file->read_buffer, BUFSIZ);
    if (result < 0) {
        file->error = true;
        return false;
    }
    if (result == 0) {
        file->eof = true;
        return false;
    }
    file->read_position = 0;
    file->read_length = static_cast<size_t>(result);
    return true;
}

int getc_unlocked(FILE* file)
{
    if (file->read_position == file->read_length) {
        // About to wait for someone at a terminal: show what they are
        // answering.
        if (file == stdin)
            fflush(stdout);
        if (!fill(file))
            return EOF;
    }
    return static_cast<unsigned char>(file->read_buffer[file->read_position++]);
}

int getchar_unlocked()
{
    return getc_unlocked(stdin);
}

int fgetc(FILE* file)
{
    FileLocker locker(file);
    return getc_unlocked(file);
}

int getc(FILE* file)
{
    return fgetc(file);
}

int getchar()
{
    return fgetc(stdin);
}

char* fgets(char* buffer, int size, FILE* file)
{
    if (size <= 0)
        return nullptr;
    FileLocker locker(file);
    int length = 0;
    while (length < size - 1) {
        int character = getc_unlocked(file);
        if (character == EOF)
            break;
        buffer[length++] = static_cast<char>(character);
        if (character == '\n')
            break;
    }
    if (length == 0)
        return nullptr;
    buffer[length] = '\0';
    return buffer;
}

size_t fread(void* buffer, size_t size, size_t count, FILE* file)
{
    auto* bytes = static_cast<char*>(buffer);
    size_t total = size * count;
    size_t done = 0;
    FileLocker locker(file);
    while (done < total) {
        // With nothing kept, what wouldn't fit in the buffer anyway comes straight in, in one piece.
        if (file != stdin && file->read_position == file->read_length && total - done >= BUFSIZ) {
            ssize_t result = read(file->fd, bytes + done, total - done);
            if (result < 0) {
                file->error = true;
                break;
            }
            if (result == 0) {
                file->eof = true;
                break;
            }
            done += static_cast<size_t>(result);
            continue;
        }
        int character = getc_unlocked(file);
        if (character == EOF)
            break;
        bytes[done++] = static_cast<char>(character);
    }
    return size ? done / size : 0;
}

int fflush(FILE* file)
{
    if (!file) {
        __stdio_flush_all();
        return 0;
    }
    FileLocker locker(file);
    return flush(file);
}

int ferror(FILE* file)
{
    return file->error;
}

int feof(FILE* file)
{
    return file->eof;
}

int fileno(FILE* file)
{
    return file->fd;
}

FILE* fopen(char const* path, char const* mode)
{
    int flags = O_RDONLY;
    if (mode[0] == 'w')
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a')
        flags = O_WRONLY | O_CREAT | O_APPEND;
    if (strchr(mode, '+'))
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    int fd = open(path, flags, 0644);
    if (fd < 0)
        return nullptr;
    auto* file = static_cast<FILE*>(calloc(1, sizeof(FILE)));
    if (!file) {
        close(fd);
        errno = ENOMEM;
        return nullptr;
    }
    file->fd = fd;
    file->buffering = Buffering::Full;
    LibC::LockGuard guard(s_open_files_lock);
    file->next = s_open_files;
    s_open_files = file;
    return file;
}

static void forget(FILE* file)
{
    LibC::LockGuard guard(s_open_files_lock);
    for (FILE** link = &s_open_files; *link; link = &(*link)->next) {
        if (*link == file) {
            *link = file->next;
            return;
        }
    }
}

int fclose(FILE* file)
{
    int result;
    {
        FileLocker locker(file);
        result = flush(file);
        if (close(file->fd) < 0)
            result = EOF;
    }
    if (file != stdin && file != stdout && file != stderr) {
        forget(file);
        free(file);
    }
    return result;
}

FILE* fdopen(int fd, char const* mode)
{
    (void)mode;
    auto* file = static_cast<FILE*>(calloc(1, sizeof(FILE)));
    if (!file) {
        errno = ENOMEM;
        return nullptr;
    }
    file->fd = fd;
    // (Opening a file is no reason for errno to change.)
    int saved = errno;
    file->buffering = isatty(fd) ? Buffering::Line : Buffering::Full;
    errno = saved;
    LibC::LockGuard guard(s_open_files_lock);
    file->next = s_open_files;
    s_open_files = file;
    return file;
}

int setvbuf(FILE* file, char* buffer, int mode, size_t size)
{
    (void)buffer;
    (void)size;
    FileLocker locker(file);
    flush(file);
    file->buffering = mode == _IONBF ? Buffering::None : (mode == _IOLBF ? Buffering::Line : Buffering::Full);
    return 0;
}

void setbuf(FILE* file, char* buffer)
{
    setvbuf(file, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ);
}

void setlinebuf(FILE* file)
{
    setvbuf(file, nullptr, _IOLBF, 0);
}

int ungetc(int character, FILE* file)
{
    if (character == EOF)
        return EOF;
    FileLocker locker(file);
    // Back into the read buffer: room at the front, or make some.
    if (file->read_position > 0) {
        file->read_buffer[--file->read_position] = static_cast<char>(character);
    } else {
        if (file->read_length == BUFSIZ)
            return EOF;
        memmove(file->read_buffer + 1, file->read_buffer, file->read_length);
        file->read_buffer[0] = static_cast<char>(character);
        ++file->read_length;
    }
    file->eof = false;
    return static_cast<unsigned char>(character);
}

void clearerr(FILE* file)
{
    FileLocker locker(file);
    file->error = false;
    file->eof = false;
}

off_t ftello(FILE* file)
{
    FileLocker locker(file);
    off_t position = lseek(file->fd, 0, SEEK_CUR);
    if (position < 0)
        return -1;
    return position - static_cast<off_t>(file->read_length - file->read_position) + static_cast<off_t>(file->write_length);
}

long ftell(FILE* file)
{
    return static_cast<long>(ftello(file));
}

int fseeko(FILE* file, off_t offset, int origin)
{
    FileLocker locker(file);
    if (flush(file) == EOF)
        return -1;
    if (origin == SEEK_CUR)
        offset -= static_cast<off_t>(file->read_length - file->read_position);
    file->read_position = file->read_length = 0;
    file->eof = false;
    return lseek(file->fd, offset, origin) < 0 ? -1 : 0;
}

int fseek(FILE* file, long offset, int origin)
{
    return fseeko(file, offset, origin);
}

void rewind(FILE* file)
{
    FileLocker locker(file);
    (void)fseeko(file, 0, SEEK_SET);
    file->error = false;
}

ssize_t getdelim(char** line, size_t* capacity, int delimiter, FILE* file)
{
    if (!line || !capacity) {
        errno = EINVAL;
        return -1;
    }
    size_t length = 0;
    FileLocker locker(file);
    for (;;) {
        int character = getc_unlocked(file);
        if (character == EOF)
            break;
        if (length + 2 > *capacity) {
            size_t grown = *capacity ? *capacity * 2 : 128;
            auto* bigger = static_cast<char*>(realloc(*line, grown));
            if (!bigger) {
                errno = ENOMEM;
                return -1;
            }
            *line = bigger;
            *capacity = grown;
        }
        (*line)[length++] = static_cast<char>(character);
        if (character == delimiter)
            break;
    }
    if (length == 0)
        return -1;
    (*line)[length] = '\0';
    return static_cast<ssize_t>(length);
}

ssize_t getline(char** line, size_t* capacity, FILE* file)
{
    return getdelim(line, capacity, '\n', file);
}

FILE* tmpfile(void)
{
    char path[] = "/tmp/tmpfile.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return nullptr;
    unlink(path);
    return fdopen(fd, "w+");
}

// Runs `command` with /bin/sh, reading its output ("r") or writing its
// input ("w").
FILE* popen(char const* command, char const* mode)
{
    bool reading = mode[0] == 'r';
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return nullptr;
    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return nullptr;
    }
    if (child == 0) {
        dup2(reading ? fds[1] : fds[0], reading ? 1 : 0);
        char* argv[] = { const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command), nullptr };
        execv("/bin/sh", argv);
        _exit(127);
    }
    close(reading ? fds[1] : fds[0]);
    FILE* file = fdopen(reading ? fds[0] : fds[1], mode);
    if (file)
        file->child = child;
    return file;
}

int pclose(FILE* file)
{
    pid_t child = file->child;
    fclose(file);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

int vasprintf(char** result, char const* format, va_list arguments)
{
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (length < 0)
        return -1;
    *result = static_cast<char*>(malloc(static_cast<size_t>(length) + 1));
    if (!*result)
        return -1;
    return vsnprintf(*result, static_cast<size_t>(length) + 1, format, arguments);
}

int asprintf(char** result, char const* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int length = vasprintf(result, format, arguments);
    va_end(arguments);
    return length;
}

void perror(char const* prefix)
{
    if (prefix && *prefix)
        fprintf(stderr, "%s: %s\n", prefix, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}
}
