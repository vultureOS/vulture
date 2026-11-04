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
#include <LibSystem/DyldServices.h>
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
//
// And it stays around for the program's life: dlopen() and the rest come here, through the table the C library is
// given at the start (LibSystem/DyldServices.h). What can go wrong then is an answer for dlerror(), not the end of
// the program.

using namespace ELF;
using System::argument;
using System::Syscall;

namespace {

constexpr size_t max_objects = 128;
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
    // Where it's mapped, whole.
    u64 map_start { 0 };
    u64 map_size { 0 };
    // Each one it needed, once loaded.
    Object* dependencies[max_needed];
    size_t dependency_count { 0 };
    // dlopen()'s. What was there from the start stays, is seen by everything, and owes nobody anything.
    // (All of these start as zero: nothing runs this program's own initializers, so that's what they are.)
    bool opened_later { false };
    bool opened_privately { false };
    bool never_unload { false };
    u32 group { 0 }; // Those opened together see each other, however they were opened.
    u32 references { 0 };
};

Object s_objects[max_objects];
size_t s_object_count = 0;
bool s_secure = false;
char const* s_library_path = nullptr; // DYLD_LIBRARY_PATH, unless secure.

// What went wrong: what the first program's start gives up with, or dlerror() is told.
char s_error[1536];
// What load_library() tried.
char s_tried[1024];

void set_error(char const* message)
{
    copy_string(s_error, sizeof(s_error), message);
}

void set_symbol_not_found(char const* name, Object const& object)
{
    copy_string(s_error, sizeof(s_error), "Symbol not found: ");
    append_string(s_error, sizeof(s_error), name);
    append_string(s_error, sizeof(s_error), "\n  Referenced from: ");
    append_string(s_error, sizeof(s_error), object.path);
    append_string(s_error, sizeof(s_error), "\n  Expected in: flat namespace");
}

[[nodiscard]] bool read_dynamic(Object& object)
{
    if (!object.dynamic)
        return true;
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
            set_error("text relocations are not supported");
            return false;
        default:
            break;
        }
    }
    if (has_soname && object.strings)
        object.name = object.strings + soname;
    return true;
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
    if (header.e_type != ET_DYN || header.e_machine != EM_NATIVE || header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum > 64) {
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
    object.map_start = static_cast<u64>(base);
    object.map_size = highest - lowest;

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

// Somewhere for another object: where one's gone, or at the end.
Object* free_object_slot()
{
    for (size_t i = 0; i < s_object_count; ++i) {
        if (!s_objects[i].path[0])
            return &s_objects[i];
    }
    return s_object_count == max_objects ? nullptr : &s_objects[s_object_count];
}

// Loads `name` (needed by `referrer`) unless it is loaded already. With nothing to return, s_error says why.
Object* load_library(char const* name, Object const& referrer)
{
    if (auto* loaded = find_loaded(name))
        return loaded;
    auto* slot = free_object_slot();
    if (!slot) {
        set_error("too many libraries");
        return nullptr;
    }

    // Where to look: DYLD_LIBRARY_PATH (not for set-ID programs), then the
    // system's places; a name with a slash is a path.
    s_tried[0] = '\0';
    char candidates[8][256];
    size_t candidate_count = 0;
    bool is_path = false;
    for (char const* c = name; *c; ++c)
        is_path = is_path || *c == '/';
    if (starts_with(name, "@loader_path/")) {
        // Beside whatever needed it, wherever that is.
        auto* candidate = candidates[candidate_count++];
        copy_string(candidate, 256, referrer.path);
        const_cast<char*>(file_name(candidate))[0] = '\0';
        append_string(candidate, 256, name + 13);
    } else if (is_path) {
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

    bool const at_the_end = slot == &s_objects[s_object_count];
    for (size_t i = 0; i < candidate_count; ++i) {
        char const* path = candidates[i];
        long fd = call(Syscall::open, argument(path), O_RDONLY | O_CLOEXEC, 0);
        char reason[96] = "no such file";
        if (fd >= 0) {
            auto& object = *slot;
            bool mapped = map_library(static_cast<int>(fd), object, reason, sizeof(reason));
            call(Syscall::close, static_cast<unsigned long>(fd));
            if (mapped) {
                copy_string(object.path, sizeof(object.path), path);
                if (!read_dynamic(object)) {
                    s_tried[0] = '\0';
                    call(Syscall::munmap, object.map_start, object.map_size);
                    object = Object {};
                    return nullptr;
                }
                if (!object.name)
                    object.name = file_name(object.path);
                if (at_the_end)
                    ++s_object_count;
                return &object;
            }
            if (object.map_size)
                call(Syscall::munmap, object.map_start, object.map_size);
            object = Object {};
        }
        if (s_tried[0])
            append_string(s_tried, sizeof(s_tried), ", ");
        append_string(s_tried, sizeof(s_tried), "'");
        append_string(s_tried, sizeof(s_tried), path);
        append_string(s_tried, sizeof(s_tried), "' (");
        append_string(s_tried, sizeof(s_tried), reason);
        append_string(s_tried, sizeof(s_tried), ")");
    }
    copy_string(s_error, sizeof(s_error), "Library not loaded: ");
    append_string(s_error, sizeof(s_error), name);
    append_string(s_error, sizeof(s_error), "\n  Referenced from: ");
    append_string(s_error, sizeof(s_error), referrer.path);
    append_string(s_error, sizeof(s_error), "\n  Reason: tried: ");
    append_string(s_error, sizeof(s_error), s_tried);
    return nullptr;
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

// Can `requester` see what's in `object`? What was opened privately only shows to what was opened with it.
bool in_scope(Object const& object, Object const* requester)
{
    return !object.opened_privately || (requester && object.group == requester->group);
}

// Where `name` is, searching the program first, then the libraries in the
// order they were loaded (one flat namespace, as ELF has it).
bool resolve(char const* name, u64& address, Object const* requester, Object const** where = nullptr, Object const* skip = nullptr)
{
    u32 hash = elf_hash(name);
    for (size_t i = 0; i < s_object_count; ++i) {
        if (&s_objects[i] == skip || !in_scope(s_objects[i], requester))
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
// All of it is static: each module's block sits at a fixed distance from the
// thread pointer, after the two words of the control block. A variable's
// address is the thread pointer plus its module's offset plus its offset in
// the module.
//
// So what dlopen() brings has to fit in what was set aside at the start. It gets this
// thread's copy of its initial values at once, and every thread that starts later gets
// them too; threads already running see zeroes until they write something. That's what
// nearly all thread-local variables start as anyway.

#if defined(__aarch64__)
extern "C" u8 dyld_tlsdesc_static[];
#endif

System::ThreadLocalImage s_tls_image { nullptr, 0, 16 };
u64 s_tls_end = 0;
constexpr u64 tls_set_aside = 2048;

// The usual size, kept here: no system calls for it.
alignas(64) u8 s_small_tls_image[4096];
alignas(64) u8 s_small_tls_block[4096];

// Each module's offset, from what the static linker assumes for the program.
void assign_tls_offsets()
{
    u64 end = 0;
    u64 alignment = 16;
    for (size_t i = 0; i < s_object_count; ++i) {
        auto& object = s_objects[i];
        if (!object.tls_memory_size)
            continue;
        u64 virtual_address = object.tls_address - object.bias;
        u64 const place = System::next_tls_offset(end, virtual_address, object.tls_memory_size, object.tls_alignment);
        object.tls_offset = System::tls_pointer_offset(place);
#if defined(__x86_64__)
        end = place;
#else
        end = place + object.tls_memory_size;
#endif
        if (object.tls_alignment > alignment)
            alignment = object.tls_alignment;
    }
    s_tls_end = end;
    s_tls_image.size = System::tls_image_size(end + tls_set_aside, alignment);
    s_tls_image.alignment = alignment;
}

// dlopen()'s, into what's left of what was set aside.
[[nodiscard]] bool assign_later_tls_offset(Object& object)
{
    if (!object.tls_memory_size)
        return true;
    u64 const virtual_address = object.tls_address - object.bias;
    u64 const place = System::next_tls_offset(s_tls_end, virtual_address, object.tls_memory_size, object.tls_alignment);
#if defined(__x86_64__)
    u64 const end = place;
#else
    u64 const end = place + object.tls_memory_size;
#endif
    if (end > s_tls_image.size || object.tls_alignment > s_tls_image.alignment) {
        set_error("it has more thread-local storage than there is room for");
        return false;
    }
    object.tls_offset = System::tls_pointer_offset(place);
    s_tls_end = end;
    auto* image = const_cast<u8*>(static_cast<u8 const*>(s_tls_image.data));
    auto const* source = reinterpret_cast<u8 const*>(object.tls_address);
    auto* mine = reinterpret_cast<u8*>(System::thread_control_block()) + static_cast<i64>(object.tls_offset);
    u64 const index = System::tls_image_index(object.tls_offset, s_tls_image.size);
    for (u64 byte = 0; byte < object.tls_file_size; ++byte) {
        image[index + byte] = source[byte];
        mine[byte] = source[byte];
    }
    return true;
}

// Once relocated: the image every thread's block is copied from, and the
// first thread's block.
void set_up_first_thread()
{
    u8* image = s_small_tls_image;
    u8* block = s_small_tls_block;
    u64 size = s_tls_image.size;
    u64 alignment = s_tls_image.alignment;
    u64 const storage = System::thread_storage_size(size);
    if (size > sizeof(s_small_tls_image) || storage > sizeof(s_small_tls_block) || alignment > 64) {
        u64 room = align_up(size + storage + alignment * 2);
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
        u64 const place = System::tls_image_index(object.tls_offset, size);
        for (u64 byte = 0; byte < object.tls_file_size; ++byte)
            image[place + byte] = source[byte];
    }
    s_tls_image.data = image;
    System::set_thread_control_block(System::initialize_thread_storage(block, s_tls_image));
}

// The offset from the thread pointer of a thread-local symbol that `object`
// refers to (by index; 0 is `object`'s own block).
[[nodiscard]] bool tls_offset_of(Object const& object, u32 symbol_index, u64& offset)
{
    if (symbol_index == 0) {
        offset = object.tls_offset;
        return true;
    }
    auto const& symbol = object.symbols[symbol_index];
    char const* name = object.strings + symbol.st_name;
    if (elf64_st_bind(symbol.st_info) == STB_LOCAL) {
        offset = object.tls_offset + symbol.st_value;
        return true;
    }
    u32 hash = elf_hash(name);
    for (size_t i = 0; i < s_object_count; ++i) {
        if (!in_scope(s_objects[i], &object))
            continue;
        if (auto const* found = defined_in(s_objects[i], name, hash, true)) {
            offset = s_objects[i].tls_offset + found->st_value;
            return true;
        }
    }
    set_symbol_not_found(name, object);
    return false;
}

[[nodiscard]] bool relocate_table(Object const& object, Elf64_Rela const* table, u64 size)
{
    if (!table)
        return true;
    size_t count = size / sizeof(Elf64_Rela);
    for (size_t i = 0; i < count; ++i) {
        auto const& relocation = table[i];
        u32 type = elf64_r_type(relocation.r_info);
        u32 symbol_index = elf64_r_sym(relocation.r_info);
        auto* place = reinterpret_cast<u64*>(object.bias + relocation.r_offset);
        if (type == R_NATIVE_NONE)
            continue;
        if (type == R_NATIVE_RELATIVE) {
            *place = object.bias + static_cast<u64>(relocation.r_addend);
            continue;
        }
        if (type == R_NATIVE_TPOFF) {
            u64 offset = 0;
            if (!tls_offset_of(object, symbol_index, offset))
                return false;
            *place = offset + static_cast<u64>(relocation.r_addend);
            continue;
        }
#if defined(__aarch64__)
        if (type == R_AARCH64_TLSDESC) {
            // Two words: what to call, and what it hands back from its
            // descriptor. All the storage is static, so that is the offset.
            u64 offset = 0;
            if (!tls_offset_of(object, symbol_index, offset))
                return false;
            place[1] = offset + static_cast<u64>(relocation.r_addend);
            place[0] = reinterpret_cast<u64>(dyld_tlsdesc_static);
            continue;
        }
#endif
        if (type != R_NATIVE_GLOB_DAT && type != R_NATIVE_JUMP_SLOT && type != R_NATIVE_ABS64 && type != R_NATIVE_COPY) {
            char message[300] = "unsupported relocation type ";
            append_number(message, sizeof(message), type);
            append_string(message, sizeof(message), " in ");
            append_string(message, sizeof(message), object.path);
            set_error(message);
            return false;
        }
        u64 value = 0;
        if (symbol_index != 0) {
            auto const& symbol = object.symbols[symbol_index];
            char const* name = object.strings + symbol.st_name;
            Object const* where = nullptr;
            // A copy relocation takes its data from wherever else it is.
            bool found = resolve(name, value, &object, &where, type == R_NATIVE_COPY ? &object : nullptr);
            if (!found && elf64_st_bind(symbol.st_info) == STB_LOCAL && symbol.st_shndx != SHN_UNDEF) {
                value = object.bias + symbol.st_value;
                found = true;
            }
            if (!found) {
                if (elf64_st_bind(symbol.st_info) == STB_WEAK) {
                    value = 0;
                } else {
                    set_symbol_not_found(name, object);
                    return false;
                }
            }
            if (type == R_NATIVE_COPY) {
                auto const* source = reinterpret_cast<u8 const*>(value);
                auto* destination = reinterpret_cast<u8*>(place);
                for (u64 byte = 0; byte < symbol.st_size; ++byte)
                    destination[byte] = source[byte];
                continue;
            }
        }
        *place = value + static_cast<u64>(relocation.r_addend);
    }
    return true;
}

void run_finalizers_of(Object const& object)
{
    for (u64 entry = object.fini_count; entry > 0; --entry)
        reinterpret_cast<void (*)()>(object.fini_array[entry - 1])();
    if (object.fini)
        reinterpret_cast<void (*)()>(object.fini)();
}

// The finalizers, run by the C library's exit() once registered with it:
// what dlopen() brought, then the program's, then the libraries', in the
// reverse of the order they were initialized.
void run_finalizers(void*)
{
    for (size_t i = s_object_count; i > 0; --i) {
        if (s_objects[i - 1].opened_later)
            run_finalizers_of(s_objects[i - 1]);
    }
    for (size_t i = 0; i < s_object_count; ++i) {
        if (!s_objects[i].opened_later)
            run_finalizers_of(s_objects[i]);
    }
}

int s_argc = 0;
char** s_argv = nullptr;
char** s_envp = nullptr;

using Initializer = void (*)(int, char**, char**);

void run_initializers_of(Object const& object)
{
    if (object.init)
        reinterpret_cast<Initializer>(object.init)(s_argc, s_argv, s_envp);
    for (u64 n = 0; n < object.init_count; ++n) {
        u64 function = object.init_array[n];
        if (function != 0 && function != ~u64(0))
            reinterpret_cast<Initializer>(function)(s_argc, s_argv, s_envp);
    }
}

// --- What the C library comes here for ---
//
// It holds a lock around all of these, so that one thread at a time is in here. It's a lock that the same thread can take
// again, for an initializer that calls dlopen().

u32 s_next_group = 1;
bool s_has_error = false;

void append_hex(char* destination, size_t size, u64 value, int digits)
{
    char text[20];
    size_t length = 0;
    text[length++] = '0';
    text[length++] = 'x';
    bool started = false;
    for (int i = 15; i >= 0; --i) {
        auto const nibble = static_cast<int>((value >> (i * 4)) & 0xf);
        if (nibble || i < digits || started) {
            text[length++] = "0123456789abcdef"[nibble];
            started = true;
        }
    }
    text[length] = '\0';
    append_string(destination, size, text);
}

// As a Mac says it: dlopen(/no/such.dylib, 0x0002): tried: ...
void report_open_failure(char const* path, int mode, char const* detail)
{
    char message[sizeof(s_error)] = "dlopen(";
    append_string(message, sizeof(message), path);
    append_string(message, sizeof(message), ", ");
    append_hex(message, sizeof(message), static_cast<u64>(mode), 4);
    append_string(message, sizeof(message), "): ");
    append_string(message, sizeof(message), detail);
    set_error(message);
    s_has_error = true;
}

bool is_valid_handle(void const* handle)
{
    auto const address = reinterpret_cast<u64>(handle);
    auto const first = reinterpret_cast<u64>(&s_objects[0]);
    if (address < first || address >= first + sizeof(Object) * s_object_count || (address - first) % sizeof(Object) != 0)
        return false;
    return reinterpret_cast<Object const*>(handle)->path[0] != '\0';
}

Object* object_containing(u64 address)
{
    for (size_t i = 0; i < s_object_count; ++i) {
        auto& object = s_objects[i];
        if (object.path[0] && address >= object.map_start && address - object.map_start < object.map_size)
            return &object;
    }
    return nullptr;
}

void unmap_and_forget(Object& object)
{
    if (object.map_size)
        call(Syscall::munmap, object.map_start, object.map_size);
    object = Object {};
}

void* open_library(char const* path, int mode)
{
    if (!path)
        return &s_objects[0];
    bool const opened_privately = mode & System::dyld_mode_local;
    if (auto* loaded = find_loaded(path)) {
        ++loaded->references;
        if (mode & System::dyld_mode_global)
            loaded->opened_privately = false;
        if (mode & System::dyld_mode_no_delete)
            loaded->never_unload = true;
        return loaded;
    }
    if (mode & System::dyld_mode_no_load)
        return nullptr;

    u32 const group = s_next_group++;
    Object* opened[max_objects];
    size_t opened_count = 0;

    auto give_up = [&] {
        // Nothing's run yet, so they can just go, and what they counted on is no longer counted on.
        for (size_t i = 0; i < opened_count; ++i) {
            for (size_t n = 0; n < opened[i]->dependency_count; ++n) {
                auto* dependency = opened[i]->dependencies[n];
                if (dependency->opened_later && dependency->group != group)
                    --dependency->references;
            }
        }
        for (size_t i = 0; i < opened_count; ++i)
            unmap_and_forget(*opened[i]);
    };

    auto* root = load_library(path, s_objects[0]);
    if (!root) {
        char detail[sizeof(s_tried) + 8] = "tried: ";
        append_string(detail, sizeof(detail), s_tried);
        report_open_failure(path, mode, s_tried[0] ? detail : s_error);
        return nullptr;
    }
    auto note_new = [&](Object& object) {
        object.opened_later = true;
        object.opened_privately = opened_privately;
        object.group = group;
        opened[opened_count++] = &object;
    };
    note_new(*root);
    root->references = 1;

    for (size_t i = 0; i < opened_count; ++i) {
        auto& object = *opened[i];
        for (size_t n = 0; n < object.needed_count; ++n) {
            auto const* needed = object.strings + object.needed[n];
            bool const already = find_loaded(needed);
            auto* dependency = load_library(needed, object);
            if (!dependency) {
                report_open_failure(path, mode, s_error);
                give_up();
                return nullptr;
            }
            if (!already)
                note_new(*dependency);
            ++dependency->references;
            object.dependencies[object.dependency_count++] = dependency;
        }
    }

    for (size_t i = 0; i < opened_count; ++i) {
        if (!assign_later_tls_offset(*opened[i])) {
            report_open_failure(path, mode, s_error);
            give_up();
            return nullptr;
        }
    }
    for (size_t i = opened_count; i > 0; --i) {
        auto const& object = *opened[i - 1];
        if (!relocate_table(object, object.relocations, object.relocations_size) || !relocate_table(object, object.plt_relocations, object.plt_relocations_size)) {
            report_open_failure(path, mode, s_error);
            give_up();
            return nullptr;
        }
        if (object.relro_size)
            call(Syscall::mprotect, object.relro_start, object.relro_size, PROT_READ);
    }
    if (mode & System::dyld_mode_no_delete)
        root->never_unload = true;
    for (size_t i = opened_count; i > 0; --i)
        run_initializers_of(*opened[i - 1]);
    return root;
}

void release_library(Object& object)
{
    // Thread-local storage can't be given back: it's in every thread.
    if (!object.opened_later || object.never_unload || object.tls_memory_size)
        return;
    if (object.references > 1) {
        --object.references;
        return;
    }
    // What it's registered with the C library at exit, it's run now, as the code won't be there then.
    u64 forget = 0;
    if (resolve("__libc_run_exit_handlers_in", forget, nullptr))
        reinterpret_cast<void (*)(u64, u64)>(forget)(object.map_start, object.map_size);
    run_finalizers_of(object);
    Object* dependencies[max_needed];
    size_t const dependency_count = object.dependency_count;
    for (size_t i = 0; i < dependency_count; ++i)
        dependencies[i] = object.dependencies[i];
    unmap_and_forget(object);
    for (size_t i = 0; i < dependency_count; ++i)
        release_library(*dependencies[i]);
}

int close_library(void* handle)
{
    if (!is_valid_handle(handle)) {
        char message[64] = "dlclose(";
        append_hex(message, sizeof(message), reinterpret_cast<u64>(handle), 1);
        append_string(message, sizeof(message), "): invalid handle");
        set_error(message);
        s_has_error = true;
        return -1;
    }
    release_library(*static_cast<Object*>(handle));
    return 0;
}

// What `object` defines, as an address.
void* symbol_in(Object const& object, char const* name, u32 hash)
{
    if (auto const* symbol = defined_in(object, name, hash))
        return reinterpret_cast<void*>(object.bias + symbol->st_value);
    if (auto const* symbol = defined_in(object, name, hash, true))
        return reinterpret_cast<u8*>(System::thread_control_block()) + static_cast<i64>(object.tls_offset + symbol->st_value);
    return nullptr;
}

// In `root` and whatever it depends on, nearest first.
void* symbol_under(Object& root, char const* name, u32 hash)
{
    Object* queue[max_objects];
    bool seen[max_objects] {};
    size_t count = 0;
    queue[count++] = &root;
    seen[&root - s_objects] = true;
    for (size_t i = 0; i < count; ++i) {
        if (auto* found = symbol_in(*queue[i], name, hash))
            return found;
        for (size_t n = 0; n < queue[i]->dependency_count; ++n) {
            auto* dependency = queue[i]->dependencies[n];
            if (!seen[dependency - s_objects]) {
                seen[dependency - s_objects] = true;
                queue[count++] = dependency;
            }
        }
    }
    return nullptr;
}

void* find_symbol(void* handle, char const* name, void const* caller)
{
    u32 const hash = elf_hash(name);
    void* found = nullptr;
    auto const special = reinterpret_cast<long>(handle);
    auto* calling = object_containing(reinterpret_cast<u64>(caller));
    if (special == System::dyld_handle_default) {
        for (size_t i = 0; i < s_object_count && !found; ++i) {
            if (in_scope(s_objects[i], calling))
                found = symbol_in(s_objects[i], name, hash);
        }
    } else if (special == System::dyld_handle_next || special == System::dyld_handle_self) {
        // What comes after the caller in load order; or, for "self", the caller first.
        size_t const start = calling ? static_cast<size_t>(calling - s_objects) + (special == System::dyld_handle_next ? 1 : 0) : 0;
        for (size_t i = start; i < s_object_count && !found; ++i) {
            if (in_scope(s_objects[i], calling) || &s_objects[i] == calling)
                found = symbol_in(s_objects[i], name, hash);
        }
    } else if (special == System::dyld_handle_main_only) {
        found = symbol_in(s_objects[0], name, hash);
    } else if (is_valid_handle(handle)) {
        found = symbol_under(*static_cast<Object*>(handle), name, hash);
    } else {
        char message[64] = "dlsym(";
        append_hex(message, sizeof(message), reinterpret_cast<u64>(handle), 1);
        append_string(message, sizeof(message), "): invalid handle");
        set_error(message);
        s_has_error = true;
        return nullptr;
    }
    if (found)
        return found;
    char message[300] = "dlsym(";
    append_hex(message, sizeof(message), reinterpret_cast<u64>(handle), 1);
    append_string(message, sizeof(message), ", ");
    append_string(message, sizeof(message), name);
    append_string(message, sizeof(message), "): symbol not found");
    set_error(message);
    s_has_error = true;
    return nullptr;
}

int describe_address(void const* address, char const** file, void** base, char const** symbol_name, void** symbol_address)
{
    auto const value = reinterpret_cast<u64>(address);
    auto const* owner = object_containing(value);
    if (!owner)
        return 0;
    *file = owner->path;
    *base = reinterpret_cast<void*>(owner->map_start);
    *symbol_name = nullptr;
    *symbol_address = nullptr;
    // There's no other way to do this but to look at them all; nobody's in a hurry when they ask.
    u32 const count = owner->hash ? owner->hash[1] : 0;
    u64 best = 0;
    for (u32 i = 1; i < count; ++i) {
        auto const& symbol = owner->symbols[i];
        if (symbol.st_shndx == SHN_UNDEF || elf64_st_type(symbol.st_info) == STT_TLS)
            continue;
        u64 const place = owner->bias + symbol.st_value;
        if (place <= value && place > best) {
            best = place;
            *symbol_name = owner->strings + symbol.st_name;
            *symbol_address = reinterpret_cast<void*>(place);
        }
    }
    return 1;
}

char const* take_error()
{
    if (!s_has_error)
        return nullptr;
    s_has_error = false;
    return s_error;
}

constinit System::DyldServices const s_services {
    open_library,
    [](void* handle) { return close_library(handle); },
    find_symbol,
    describe_address,
    take_error,
};

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
        if ((info & 0xffffffff) == R_NATIVE_RELATIVE)
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
    s_argc = argc;
    s_argv = argv;
    s_envp = envp;

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
    u64 lowest = ~u64(0), highest = 0;
    for (u64 i = 0; i < header_count; ++i) {
        if (headers[i].p_type == PT_LOAD) {
            if (align_down(headers[i].p_vaddr) < lowest)
                lowest = align_down(headers[i].p_vaddr);
            if (align_up(headers[i].p_vaddr + headers[i].p_memsz) > highest)
                highest = align_up(headers[i].p_vaddr + headers[i].p_memsz);
        }
        if (headers[i].p_type == PT_DYNAMIC)
            program.dynamic = reinterpret_cast<Elf64_Dyn const*>(program.bias + headers[i].p_vaddr);
        if (headers[i].p_type == PT_TLS)
            note_tls_segment(program, headers[i]);
        if (headers[i].p_type == PT_GNU_RELRO) {
            program.relro_start = align_down(program.bias + headers[i].p_vaddr);
            program.relro_size = align_down(program.bias + headers[i].p_vaddr + headers[i].p_memsz) - program.relro_start;
        }
    }
    if (lowest < highest) {
        program.map_start = program.bias + lowest;
        program.map_size = highest - lowest;
    }
    if (!read_dynamic(program))
        fail(s_error);

    // Every library needed, breadth first.
    for (size_t i = 0; i < s_object_count; ++i) {
        auto& object = s_objects[i];
        for (size_t n = 0; n < object.needed_count; ++n) {
            auto* dependency = load_library(object.strings + object.needed[n], object);
            if (!dependency)
                fail(s_error);
            object.dependencies[object.dependency_count++] = dependency;
        }
    }

    assign_tls_offsets();

    // Libraries before the program (the program's copy relocations take
    // what the libraries' data holds once relocated).
    for (size_t i = s_object_count; i > 0; --i) {
        auto const& object = s_objects[i - 1];
        if (!relocate_table(object, object.relocations, object.relocations_size) || !relocate_table(object, object.plt_relocations, object.plt_relocations_size))
            fail(s_error);
        if (object.relro_size)
            call(Syscall::mprotect, object.relro_start, object.relro_size, PROT_READ);
    }

    set_up_first_thread();

    // Before any initializer: one might call dlopen().
    u64 connect = 0;
    if (resolve("__libc_set_dyld_services", connect, nullptr))
        reinterpret_cast<void (*)(System::DyldServices const*)>(connect)(&s_services);

    // Initializers: the libraries most depended on first, the program last;
    // each called as the C library's (argc, argv, environ).
    size_t const objects_at_start = s_object_count;
    for (size_t i = objects_at_start; i > 0; --i)
        run_initializers_of(s_objects[i - 1]);
    // Whatever's here now, or dlopen() brings later.
    u64 register_handler = 0;
    if (resolve("__libc_register_exit_handler", register_handler, nullptr))
        reinterpret_cast<int (*)(void (*)(void*), void*)>(register_handler)(run_finalizers, nullptr);
    return entry;
}
