/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Kernel/API/POSIX/auxv.h>
#include <Kernel/API/POSIX/files.h>
#include <Kernel/API/POSIX/memory.h>
#include <Kernel/API/POSIX/signal_numbers.h>
#include <LibELF/ELF.h>
#include <LibSystem/Syscall.h>
#include <LibSystem/ThreadPointer.h>

// dyld, the dynamic linker (/usr/lib/dyld). The kernel loads a dynamically
// linked program and this, and starts this. It loads the libraries the
// program needs (and they need) from /usr/lib, maps their segments from
// the files (so every process shares the pages), relocates everything, sets
// up the first thread's thread-local storage, runs the initializers, and
// jumps to the program.
//
// It has no C library to use (it is what loads it): raw system calls, no
// allocation, everything in fixed tables. It is linked for its own
// address, so it needs no relocating itself.
//
// Errors are reported as macOS's dyld reports them, then the program is
// aborted:
//
//   dyld[123]: Library not loaded: libfoo.dylib
//     Referenced from: /usr/bin/tool
//     Reason: tried: '/usr/lib/libfoo.dylib' (no such file), ...

using namespace ELF;
using System::argument;
using System::Syscall;

namespace {

constexpr size_t max_objects = 64;
constexpr size_t max_needed = 32;
constexpr u64 page_size = 4096;

// Small helpers (no C library here).

size_t string_length(char const* text)
{
    size_t length = 0;
    while (text[length])
        ++length;
    return length;
}

bool strings_equal(char const* a, char const* b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool starts_with(char const* text, char const* prefix)
{
    while (*prefix) {
        if (*text++ != *prefix++)
            return false;
    }
    return true;
}

void copy_string(char* destination, size_t size, char const* source)
{
    size_t i = 0;
    for (; i + 1 < size && source[i]; ++i)
        destination[i] = source[i];
    destination[i] = '\0';
}

void append_string(char* destination, size_t size, char const* source)
{
    size_t length = string_length(destination);
    copy_string(destination + length, size - length, source);
}

void append_number(char* destination, size_t size, long number)
{
    char digits[24];
    size_t count = 0;
    unsigned long value = number < 0 ? static_cast<unsigned long>(-number) : static_cast<unsigned long>(number);
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    char text[26];
    size_t length = 0;
    if (number < 0)
        text[length++] = '-';
    while (count)
        text[length++] = digits[--count];
    text[length] = '\0';
    append_string(destination, size, text);
}

u64 align_down(u64 value) { return value & ~(page_size - 1); }
u64 align_up(u64 value) { return (value + page_size - 1) & ~(page_size - 1); }

// System calls.

long call(Syscall number, unsigned long a0 = 0, unsigned long a1 = 0, unsigned long a2 = 0, unsigned long a3 = 0, unsigned long a4 = 0, unsigned long a5 = 0)
{
    return System::syscall(number, a0, a1, a2, a3, a4, a5);
}

void write_error(char const* text)
{
    call(Syscall::write, 2, argument(text), string_length(text));
}

[[noreturn]] void abort_program()
{
    call(Syscall::kill, static_cast<unsigned long>(call(Syscall::getpid)), SIGABRT);
    call(Syscall::exit, 134);
    __builtin_unreachable();
}

// "dyld[pid]: " and the message, then the program is aborted.
[[noreturn]] void fail(char const* message)
{
    char line[1536] = "dyld[";
    append_number(line, sizeof(line), call(Syscall::getpid));
    append_string(line, sizeof(line), "]: ");
    append_string(line, sizeof(line), message);
    append_string(line, sizeof(line), "\n");
    write_error(line);
    abort_program();
}

// One loaded ELF object: the program or a library.
struct Object {
    char path[256];
    char const* name { nullptr }; // Its DT_SONAME, or its file name.
    u64 bias { 0 };
    Elf64_Dyn const* dynamic { nullptr };
    Elf64_Sym const* symbols { nullptr };
    char const* strings { nullptr };
    u32 const* hash { nullptr };
    Elf64_Rela const* relocations { nullptr };
    u64 relocations_size { 0 };
    Elf64_Rela const* plt_relocations { nullptr };
    u64 plt_relocations_size { 0 };
    u64 init { 0 };
    u64 const* init_array { nullptr };
    u64 init_count { 0 };
    u64 fini { 0 };
    u64 const* fini_array { nullptr };
    u64 fini_count { 0 };
    u64 relro_start { 0 };
    u64 relro_size { 0 };
    u64 needed[max_needed];
    size_t needed_count { 0 };
    // Its thread-local storage (PT_TLS), and where that is from the thread
    // pointer in every thread.
    u64 tls_address { 0 };
    u64 tls_file_size { 0 };
    u64 tls_memory_size { 0 };
    u64 tls_alignment { 1 };
    u64 tls_offset { 0 };
};

Object s_objects[max_objects];
size_t s_object_count = 0;
bool s_secure = false;
char const* s_library_path = nullptr; // DYLD_LIBRARY_PATH, unless secure.

void read_dynamic(Object& object)
{
    if (!object.dynamic)
        return;
    u64 soname = 0;
    bool has_soname = false;
    for (auto const* entry = object.dynamic; entry->d_tag != DT_NULL; ++entry) {
        u64 address = object.bias + entry->d_val;
        switch (entry->d_tag) {
        case DT_NEEDED:
            if (object.needed_count < max_needed)
                object.needed[object.needed_count++] = entry->d_val;
            break;
        case DT_SONAME:
            soname = entry->d_val;
            has_soname = true;
            break;
        case DT_SYMTAB:
            object.symbols = reinterpret_cast<Elf64_Sym const*>(address);
            break;
        case DT_STRTAB:
            object.strings = reinterpret_cast<char const*>(address);
            break;
        case DT_HASH:
            object.hash = reinterpret_cast<u32 const*>(address);
            break;
        case DT_RELA:
            object.relocations = reinterpret_cast<Elf64_Rela const*>(address);
            break;
        case DT_RELASZ:
            object.relocations_size = entry->d_val;
            break;
        case DT_JMPREL:
            object.plt_relocations = reinterpret_cast<Elf64_Rela const*>(address);
            break;
        case DT_PLTRELSZ:
            object.plt_relocations_size = entry->d_val;
            break;
        case DT_INIT:
            object.init = address;
            break;
        case DT_INIT_ARRAY:
            object.init_array = reinterpret_cast<u64 const*>(address);
            break;
        case DT_INIT_ARRAYSZ:
            object.init_count = entry->d_val / sizeof(u64);
            break;
        case DT_FINI:
            object.fini = address;
            break;
        case DT_FINI_ARRAY:
            object.fini_array = reinterpret_cast<u64 const*>(address);
            break;
        case DT_FINI_ARRAYSZ:
            object.fini_count = entry->d_val / sizeof(u64);
            break;
        case DT_TEXTREL:
            fail("text relocations are not supported");
        default:
            break;
        }
    }
    if (has_soname && object.strings)
        object.name = object.strings + soname;
}

// "/usr/lib/libSystem.B.dylib" -> "libSystem.B.dylib"
char const* file_name(char const* path)
{
    char const* name = path;
    for (char const* c = path; *c; ++c) {
        if (*c == '/')
            name = c + 1;
    }
    return name;
}

Object* find_loaded(char const* name)
{
    for (size_t i = 0; i < s_object_count; ++i) {
        auto& object = s_objects[i];
        if ((object.name && strings_equal(object.name, name)) || strings_equal(object.path, name))
            return &object;
    }
    return nullptr;
}

void note_tls_segment(Object& object, Elf64_Phdr const& segment)
{
    object.tls_address = object.bias + segment.p_vaddr;
    object.tls_file_size = segment.p_filesz;
    object.tls_memory_size = segment.p_memsz;
    object.tls_alignment = segment.p_align ? segment.p_align : 1;
}

// Maps the library's segments from `fd`; false (and `reason`) if it is
// not one.
bool map_library(int fd, Object& object, char* reason, size_t reason_size)
{
    Elf64_Ehdr header;
    if (call(Syscall::read, static_cast<unsigned long>(fd), argument(&header), sizeof(header)) != sizeof(header)
        || header.e_ident[0] != 0x7f || header.e_ident[1] != 'E' || header.e_ident[2] != 'L' || header.e_ident[3] != 'F'
        || header.e_ident[EI_CLASS] != ELFCLASS64) {
        copy_string(reason, reason_size, "not an ELF file");
        return false;
    }
    if (header.e_type != ET_DYN || header.e_machine != EM_AARCH64 || header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum > 64) {
        copy_string(reason, reason_size, "incompatible architecture or not a library");
        return false;
    }
    Elf64_Phdr segments[64];
    call(Syscall::lseek, static_cast<unsigned long>(fd), header.e_phoff, 0);
    long wanted = static_cast<long>(header.e_phnum * sizeof(Elf64_Phdr));
    if (call(Syscall::read, static_cast<unsigned long>(fd), argument(segments), static_cast<unsigned long>(wanted)) != wanted) {
        copy_string(reason, reason_size, "truncated");
        return false;
    }

    // One reservation for the whole image, then each segment in its place.
    u64 lowest = ~u64(0), highest = 0;
    for (size_t i = 0; i < header.e_phnum; ++i) {
        if (segments[i].p_type != PT_LOAD || segments[i].p_memsz == 0)
            continue;
        if (align_down(segments[i].p_vaddr) < lowest)
            lowest = align_down(segments[i].p_vaddr);
        if (align_up(segments[i].p_vaddr + segments[i].p_memsz) > highest)
            highest = align_up(segments[i].p_vaddr + segments[i].p_memsz);
    }
    if (lowest >= highest) {
        copy_string(reason, reason_size, "no segments");
        return false;
    }
    long base = call(Syscall::mmap, 0, highest - lowest, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, static_cast<unsigned long>(-1), 0);
    if (base < 0) {
        copy_string(reason, reason_size, "out of memory");
        return false;
    }
    object.bias = static_cast<u64>(base) - lowest;

    for (size_t i = 0; i < header.e_phnum; ++i) {
        auto const& segment = segments[i];
        if (segment.p_type == PT_DYNAMIC)
            object.dynamic = reinterpret_cast<Elf64_Dyn const*>(object.bias + segment.p_vaddr);
        if (segment.p_type == PT_TLS)
            note_tls_segment(object, segment);
        if (segment.p_type == PT_GNU_RELRO) {
            object.relro_start = align_down(object.bias + segment.p_vaddr);
            object.relro_size = align_down(object.bias + segment.p_vaddr + segment.p_memsz) - object.relro_start;
        }
        if (segment.p_type != PT_LOAD || segment.p_memsz == 0)
            continue;
        int protection = ((segment.p_flags & PF_R) ? PROT_READ : 0) | ((segment.p_flags & PF_W) ? PROT_WRITE : 0) | ((segment.p_flags & PF_X) ? PROT_EXEC : 0);
        u64 start = align_down(object.bias + segment.p_vaddr);
        u64 file_end = object.bias + segment.p_vaddr + segment.p_filesz;
        u64 memory_end = align_up(object.bias + segment.p_vaddr + segment.p_memsz);
        if (segment.p_filesz > 0) {
            long mapped = call(Syscall::mmap, start, align_up(file_end) - start, static_cast<unsigned long>(protection), MAP_PRIVATE | MAP_FIXED,
                static_cast<unsigned long>(fd), align_down(segment.p_offset));
            if (mapped < 0) {
                copy_string(reason, reason_size, "could not map a segment");
                return false;
            }
        }
        // Zero-filled memory (.bss): the rest of the last file page, then
        // fresh pages.
        if (segment.p_memsz > segment.p_filesz) {
            u64 file_page_end = segment.p_filesz > 0 ? align_up(file_end) : start;
            if (segment.p_filesz > 0 && file_end < file_page_end) {
                if (!(protection & PROT_WRITE)) {
                    copy_string(reason, reason_size, "zero-filled memory in a read-only segment");
                    return false;
                }
                for (u64 byte = file_end; byte < file_page_end; ++byte)
                    *reinterpret_cast<volatile u8*>(byte) = 0;
            }
            if (memory_end > file_page_end
                && call(Syscall::mmap, file_page_end, memory_end - file_page_end, static_cast<unsigned long>(protection), MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
                       static_cast<unsigned long>(-1), 0)
                    < 0) {
                copy_string(reason, reason_size, "out of memory");
                return false;
            }
        }
    }
    if (!object.dynamic) {
        copy_string(reason, reason_size, "not a dynamic library");
        return false;
    }
    return true;
}

// Loads `name` (needed by `referrer`) unless it is loaded already.
void load_library(char const* name, Object const& referrer)
{
    if (find_loaded(name))
        return;
    if (s_object_count == max_objects)
        fail("too many libraries");

    // Where to look: DYLD_LIBRARY_PATH (not for set-ID programs), then the
    // system's places; a name with a slash is a path.
    char tried[1024] = "";
    char candidates[8][256];
    size_t candidate_count = 0;
    if (name[0] == '/' || starts_with(name, "./")) {
        copy_string(candidates[candidate_count++], 256, name);
    } else {
        if (s_library_path) {
            char const* cursor = s_library_path;
            while (*cursor && candidate_count < 6) {
                size_t length = 0;
                while (cursor[length] && cursor[length] != ':')
                    ++length;
                if (length > 0 && length < 200) {
                    auto* candidate = candidates[candidate_count++];
                    for (size_t i = 0; i < length; ++i)
                        candidate[i] = cursor[i];
                    candidate[length] = '\0';
                    append_string(candidate, 256, "/");
                    append_string(candidate, 256, name);
                }
                cursor += length;
                if (*cursor == ':')
                    ++cursor;
            }
        }
        char const* const system_places[] = { "/usr/lib/", "/usr/local/lib/" };
        for (auto const* place : system_places) {
            copy_string(candidates[candidate_count], 256, place);
            append_string(candidates[candidate_count++], 256, name);
        }
    }

    for (size_t i = 0; i < candidate_count; ++i) {
        char const* path = candidates[i];
        long fd = call(Syscall::open, argument(path), O_RDONLY | O_CLOEXEC, 0);
        char reason[96] = "no such file";
        if (fd >= 0) {
            auto& object = s_objects[s_object_count];
            bool mapped = map_library(static_cast<int>(fd), object, reason, sizeof(reason));
            call(Syscall::close, static_cast<unsigned long>(fd));
            if (mapped) {
                copy_string(object.path, sizeof(object.path), path);
                read_dynamic(object);
                if (!object.name)
                    object.name = file_name(object.path);
                ++s_object_count;
                return;
            }
            object = Object {};
        }
        if (tried[0])
            append_string(tried, sizeof(tried), ", ");
        append_string(tried, sizeof(tried), "'");
        append_string(tried, sizeof(tried), path);
        append_string(tried, sizeof(tried), "' (");
        append_string(tried, sizeof(tried), reason);
        append_string(tried, sizeof(tried), ")");
    }
    char message[1400] = "Library not loaded: ";
    append_string(message, sizeof(message), name);
    append_string(message, sizeof(message), "\n  Referenced from: ");
    append_string(message, sizeof(message), referrer.path);
    append_string(message, sizeof(message), "\n  Reason: tried: ");
    append_string(message, sizeof(message), tried);
    fail(message);
}

u32 elf_hash(char const* name)
{
    u32 hash = 0;
    while (*name) {
        hash = (hash << 4) + static_cast<u8>(*name++);
        u32 high = hash & 0xf0000000;
        if (high)
            hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

// The symbol `name` as `object` defines it (DT_HASH lookup): an ordinary
// one, or a thread-local one.
Elf64_Sym const* defined_in(Object const& object, char const* name, u32 hash, bool thread_local_symbol = false)
{
    if (!object.hash || !object.symbols || !object.strings)
        return nullptr;
    u32 bucket_count = object.hash[0];
    u32 const* buckets = object.hash + 2;
    u32 const* chains = buckets + bucket_count;
    for (u32 index = buckets[hash % bucket_count]; index; index = chains[index]) {
        auto const& symbol = object.symbols[index];
        u8 binding = elf64_st_bind(symbol.st_info);
        if (symbol.st_shndx == SHN_UNDEF || (binding != STB_GLOBAL && binding != STB_WEAK) || (elf64_st_type(symbol.st_info) == STT_TLS) != thread_local_symbol)
            continue;
        if (strings_equal(object.strings + symbol.st_name, name))
            return &symbol;
    }
    return nullptr;
}

// Where `name` is, searching the program first, then the libraries in the
// order they were loaded (one flat namespace, as ELF has it).
bool resolve(char const* name, u64& address, Object const** where = nullptr, Object const* skip = nullptr)
{
    u32 hash = elf_hash(name);
    for (size_t i = 0; i < s_object_count; ++i) {
        if (&s_objects[i] == skip)
            continue;
        if (auto const* symbol = defined_in(s_objects[i], name, hash)) {
            address = s_objects[i].bias + symbol->st_value;
            if (where)
                *where = &s_objects[i];
            return true;
        }
    }
    return false;
}

// --- Thread-local storage ---
//
// All of it is static: every library is loaded before the program starts,
// and each module's block sits at a fixed distance from the thread pointer,
// after the two words of the control block. A variable's address is the
// thread pointer plus its module's offset plus its offset in the module.

extern "C" u8 dyld_tlsdesc_static[];

System::ThreadLocalImage s_tls_image { nullptr, 0, 16 };

// The usual size, kept here: no system calls for it.
alignas(64) u8 s_small_tls_image[1024];
alignas(64) u8 s_small_tls_block[1024];

// Each module's offset, from what the static linker assumes for the program:
// 16 bytes on, then up to an address that agrees with the segment's own
// modulo its alignment.
void assign_tls_offsets()
{
    u64 end = sizeof(System::ThreadControlBlock);
    u64 alignment = 16;
    for (size_t i = 0; i < s_object_count; ++i) {
        auto& object = s_objects[i];
        if (!object.tls_memory_size)
            continue;
        u64 virtual_address = object.tls_address - object.bias;
        object.tls_offset = end + ((virtual_address - end) & (object.tls_alignment - 1));
        end = object.tls_offset + object.tls_memory_size;
        if (object.tls_alignment > alignment)
            alignment = object.tls_alignment;
    }
    s_tls_image.size = end;
    s_tls_image.alignment = alignment;
}

// Once relocated: the image every thread's block is copied from, and the
// first thread's block.
void set_up_first_thread()
{
    u8* image = s_small_tls_image;
    u8* block = s_small_tls_block;
    u64 size = s_tls_image.size;
    u64 alignment = s_tls_image.alignment;
    if (size > sizeof(s_small_tls_image) || alignment > 64) {
        u64 room = align_up(size * 2 + alignment * 2);
        long mapped = call(Syscall::mmap, 0, room, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, static_cast<unsigned long>(-1), 0);
        if (mapped < 0)
            fail("out of memory setting up thread-local storage");
        auto base = static_cast<u64>(mapped);
        image = reinterpret_cast<u8*>(base);
        block = reinterpret_cast<u8*>((base + size + alignment - 1) & ~(alignment - 1));
    }
    for (size_t i = 0; i < s_object_count; ++i) {
        auto const& object = s_objects[i];
        auto const* source = reinterpret_cast<u8 const*>(object.tls_address);
        for (u64 byte = 0; byte < object.tls_file_size; ++byte)
            image[object.tls_offset + byte] = source[byte];
    }
    s_tls_image.data = image;
    for (u64 byte = 0; byte < size; ++byte)
        block[byte] = image[byte];
    auto* control = reinterpret_cast<System::ThreadControlBlock*>(block);
    control->image = &s_tls_image;
    control->thread = nullptr;
    System::set_thread_control_block(control);
}

// The offset from the thread pointer of a thread-local symbol that `object`
// refers to (by index; 0 is `object`'s own block).
u64 tls_offset_of(Object const& object, u32 symbol_index)
{
    if (symbol_index == 0)
        return object.tls_offset;
    auto const& symbol = object.symbols[symbol_index];
    char const* name = object.strings + symbol.st_name;
    if (elf64_st_bind(symbol.st_info) == STB_LOCAL)
        return object.tls_offset + symbol.st_value;
    u32 hash = elf_hash(name);
    for (size_t i = 0; i < s_object_count; ++i) {
        if (auto const* found = defined_in(s_objects[i], name, hash, true))
            return s_objects[i].tls_offset + found->st_value;
    }
    char message[600] = "Symbol not found: ";
    append_string(message, sizeof(message), name);
    append_string(message, sizeof(message), "\n  Referenced from: ");
    append_string(message, sizeof(message), object.path);
    append_string(message, sizeof(message), "\n  Expected in: flat namespace");
    fail(message);
}

void relocate_table(Object const& object, Elf64_Rela const* table, u64 size)
{
    if (!table)
        return;
    size_t count = size / sizeof(Elf64_Rela);
    for (size_t i = 0; i < count; ++i) {
        auto const& relocation = table[i];
        u32 type = elf64_r_type(relocation.r_info);
        u32 symbol_index = elf64_r_sym(relocation.r_info);
        auto* place = reinterpret_cast<u64*>(object.bias + relocation.r_offset);
        if (type == R_AARCH64_NONE)
            continue;
        if (type == R_AARCH64_RELATIVE) {
            *place = object.bias + static_cast<u64>(relocation.r_addend);
            continue;
        }
        if (type == R_AARCH64_TLS_TPREL64) {
            *place = tls_offset_of(object, symbol_index) + static_cast<u64>(relocation.r_addend);
            continue;
        }
        if (type == R_AARCH64_TLSDESC) {
            // Two words: what to call, and what it hands back from its
            // descriptor. All the storage is static, so that is the offset.
            place[1] = tls_offset_of(object, symbol_index) + static_cast<u64>(relocation.r_addend);
            place[0] = reinterpret_cast<u64>(dyld_tlsdesc_static);
            continue;
        }
        if (type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_ABS64 && type != R_AARCH64_COPY) {
            char message[300] = "unsupported relocation type ";
            append_number(message, sizeof(message), type);
            append_string(message, sizeof(message), " in ");
            append_string(message, sizeof(message), object.path);
            fail(message);
        }
        u64 value = 0;
        if (symbol_index != 0) {
            auto const& symbol = object.symbols[symbol_index];
            char const* name = object.strings + symbol.st_name;
            Object const* where = nullptr;
            // A copy relocation takes its data from wherever else it is.
            bool found = resolve(name, value, &where, type == R_AARCH64_COPY ? &object : nullptr);
            if (!found && elf64_st_bind(symbol.st_info) == STB_LOCAL && symbol.st_shndx != SHN_UNDEF) {
                value = object.bias + symbol.st_value;
                found = true;
            }
            if (!found) {
                if (elf64_st_bind(symbol.st_info) == STB_WEAK) {
                    value = 0;
                } else {
                    char message[600] = "Symbol not found: ";
                    append_string(message, sizeof(message), name);
                    append_string(message, sizeof(message), "\n  Referenced from: ");
                    append_string(message, sizeof(message), object.path);
                    append_string(message, sizeof(message), "\n  Expected in: flat namespace");
                    fail(message);
                }
            }
            if (type == R_AARCH64_COPY) {
                auto const* source = reinterpret_cast<u8 const*>(value);
                auto* destination = reinterpret_cast<u8*>(place);
                for (u64 byte = 0; byte < symbol.st_size; ++byte)
                    destination[byte] = source[byte];
                continue;
            }
        }
        *place = value + static_cast<u64>(relocation.r_addend);
    }
}

// The finalizers, run by the C library's exit() once registered with it:
// the program's first, then the libraries', in the reverse of the order
// they were initialized.
void run_finalizers(void*)
{
    for (size_t i = 0; i < s_object_count; ++i) {
        auto const& object = s_objects[i];
        for (u64 entry = object.fini_count; entry > 0; --entry)
            reinterpret_cast<void (*)()>(object.fini_array[entry - 1])();
        if (object.fini)
            reinterpret_cast<void (*)()>(object.fini)();
    }
}

}

// Where the kernel put us, the addresses in our data are wrong by that much: only the relative
// relocations a position-independent, self-contained program has. This runs before anything may
// use such an address, so it touches nothing but what it is given.
extern "C" void dyld_relocate_self(u64 base, u64 const* dynamic)
{
    u64 const* table = nullptr;
    u64 size = 0;
    for (u64 const* entry = dynamic; entry[0] != 0; entry += 2) {
        if (entry[0] == 7) // DT_RELA
            table = reinterpret_cast<u64 const*>(base + entry[1]);
        else if (entry[0] == 8) // DT_RELASZ
            size = entry[1];
    }
    for (u64 i = 0; i < size / 24; ++i) {
        u64 const offset = table[i * 3];
        u64 const info = table[i * 3 + 1];
        u64 const addend = table[i * 3 + 2];
        if ((info & 0xffffffff) == 1027) // R_AARCH64_RELATIVE
            *reinterpret_cast<u64*>(base + offset) = base + addend;
    }
}

extern "C" u64 dyld_main(u64* stack)
{
    int argc = static_cast<int>(stack[0]);
    char** argv = reinterpret_cast<char**>(stack + 1);
    char** envp = argv + argc + 1;
    char** cursor = envp;
    while (*cursor)
        ++cursor;
    auto const* auxiliary = reinterpret_cast<u64 const*>(cursor + 1);

    u64 program_headers = 0, header_count = 0, entry = 0;
    char const* executable_path = argc > 0 ? argv[0] : "?";
    for (auto const* pair = auxiliary; pair[0] != AT_NULL; pair += 2) {
        switch (pair[0]) {
        case AT_PHDR:
            program_headers = pair[1];
            break;
        case AT_PHNUM:
            header_count = pair[1];
            break;
        case AT_ENTRY:
            entry = pair[1];
            break;
        case AT_SECURE:
            s_secure = pair[1] != 0;
            break;
        case AT_EXECFN:
            executable_path = reinterpret_cast<char const*>(pair[1]);
            break;
        default:
            break;
        }
    }
    if (!s_secure) {
        for (char** variable = envp; *variable; ++variable) {
            if (starts_with(*variable, "DYLD_LIBRARY_PATH="))
                s_library_path = *variable + 18;
        }
    }

    // The program: where the kernel put it, found from its headers.
    auto& program = s_objects[s_object_count++];
    copy_string(program.path, sizeof(program.path), executable_path);
    program.name = program.path;
    auto const* headers = reinterpret_cast<Elf64_Phdr const*>(program_headers);
    for (u64 i = 0; i < header_count; ++i) {
        if (headers[i].p_type == PT_PHDR)
            program.bias = program_headers - headers[i].p_vaddr;
    }
    for (u64 i = 0; i < header_count; ++i) {
        if (headers[i].p_type == PT_DYNAMIC)
            program.dynamic = reinterpret_cast<Elf64_Dyn const*>(program.bias + headers[i].p_vaddr);
        if (headers[i].p_type == PT_TLS)
            note_tls_segment(program, headers[i]);
        if (headers[i].p_type == PT_GNU_RELRO) {
            program.relro_start = align_down(program.bias + headers[i].p_vaddr);
            program.relro_size = align_down(program.bias + headers[i].p_vaddr + headers[i].p_memsz) - program.relro_start;
        }
    }
    read_dynamic(program);

    // Every library needed, breadth first.
    for (size_t i = 0; i < s_object_count; ++i) {
        for (size_t n = 0; n < s_objects[i].needed_count; ++n)
            load_library(s_objects[i].strings + s_objects[i].needed[n], s_objects[i]);
    }

    assign_tls_offsets();

    // Libraries before the program (the program's copy relocations take
    // what the libraries' data holds once relocated).
    for (size_t i = s_object_count; i > 0; --i) {
        auto const& object = s_objects[i - 1];
        relocate_table(object, object.relocations, object.relocations_size);
        relocate_table(object, object.plt_relocations, object.plt_relocations_size);
        if (object.relro_size)
            call(Syscall::mprotect, object.relro_start, object.relro_size, PROT_READ);
    }

    set_up_first_thread();

    // Initializers: the libraries most depended on first, the program last;
    // each called as the C library's (argc, argv, environ).
    using Initializer = void (*)(int, char**, char**);
    bool any_finalizers = false;
    for (size_t i = s_object_count; i > 0; --i) {
        auto const& object = s_objects[i - 1];
        if (object.init)
            reinterpret_cast<Initializer>(object.init)(argc, argv, envp);
        for (u64 n = 0; n < object.init_count; ++n) {
            u64 function = object.init_array[n];
            if (function != 0 && function != ~u64(0))
                reinterpret_cast<Initializer>(function)(argc, argv, envp);
        }
        any_finalizers = any_finalizers || object.fini || object.fini_count;
    }
    if (any_finalizers) {
        u64 register_handler = 0;
        if (resolve("__libc_register_exit_handler", register_handler))
            reinterpret_cast<int (*)(void (*)(void*), void*)>(register_handler)(run_finalizers, nullptr);
    }
    return entry;
}
