/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <Boot/Arch/aarch64/Handoff.h>
#include <Boot/Arch/aarch64/PageTableBuilder.h>
#include <Boot/KernelLoader.h>
#include <Boot/Memory.h>
#include <Boot/RaspberryPi/Payload.h>
#include <Kernel/API/BootInfo.h>
#include <LibBase/Assertions.h>
#include <LibBase/Format.h>
#include <LibDeviceTree/FlattenedDeviceTree.h>

// What VultureBoot does, for a machine whose firmware has no UEFI: from the device tree and its mailbox, it makes
// the same BootInfo and page tables, and starts the kernel the same way. See Documentation/BootProtocol.md.
//
// Everything it allocates comes from the first 1 GiB of memory, which is what Entry.S mapped. The loader's shared
// parts (loading the kernel, building page tables) get their memory from allocate_pages(), which is defined here
// instead of asking UEFI; they only pass this struct on to it.

extern "C" u8 __image_start[];
extern "C" u8 __image_end[];

extern "C" {
// What the libraries' stack protectors check; nothing here is ever under attack.
uintptr_t __stack_chk_guard = 0x5bd1e995;
}

namespace Boot {

namespace API = Kernel::API;
using AArch64::Access;
using AArch64::MemoryAttribute;
using AArch64::PageTableBuilder;
using DeviceTree::FlattenedDeviceTree;

static UEFI::BootServices s_no_firmware {};

// ---- Saying what's happening.

static u32 volatile* s_serial = nullptr;

class Serial final : public FormatSink {
public:
    void write(StringView text) override
    {
        for (char character : text) {
            if (character == '\n')
                put('\r');
            put(character);
        }
    }

private:
    static void put(char character)
    {
        if (!s_serial)
            return;
        constexpr size_t flags = 0x18 / 4;
        constexpr u32 transmit_full = 1 << 5;
        // Nobody may have turned it on. Then it stays full, and there's no output.
        for (u32 wait = 0; s_serial[flags] & transmit_full; ++wait) {
            if (wait == 1'000'000) {
                s_serial = nullptr;
                return;
            }
        }
        s_serial[0] = static_cast<u8>(character);
    }
};

static Serial s_output;

[[noreturn]] static void stop()
{
    for (;;)
        asm volatile("wfi");
}

[[noreturn]] static void fail(StringView step, Error const& error)
{
    format_to(s_output, "VultureBoot (Raspberry Pi): {} failed: {}\n", step, error);
    stop();
}

template<typename T>
static T unwrap(StringView step, ErrorOr<T>&& result)
{
    if (result.is_error())
        fail(step, result.error());
    return result.release_value();
}

static void unwrap(StringView step, ErrorOr<void>&& result)
{
    if (result.is_error())
        fail(step, result.error());
}

// ---- Memory.

static constexpr u64 page = UEFI::page_size;
// All Entry.S has mapped as memory.
static constexpr u64 mapped_memory_end = 1024 * MiB;

struct Range {
    u64 base { 0 };
    u64 size { 0 };

    [[nodiscard]] u64 end() const { return base + size; }
    [[nodiscard]] bool overlaps(u64 other_base, u64 other_size) const { return base < other_base + other_size && other_base < end(); }
};

struct Allocation {
    Range range;
    LoaderMemoryType type { LoaderMemoryType::LoaderReclaimable };
};

static Range s_avoid[64];
static size_t s_avoid_count = 0;
static Range s_reserved[64];
static size_t s_reserved_count = 0;
static Allocation s_allocations[64];
static size_t s_allocation_count = 0;
static u64 s_next_free = 0;
static u64 s_free_end = 0;

static void avoid(u64 base, u64 size)
{
    if (s_avoid_count == array_size(s_avoid))
        fail("reading the device tree"_sv, Error::from_string_literal("too many reserved regions"));
    s_avoid[s_avoid_count++] = { align_down<u64>(base, page), align_up<u64>(base + size, page) - align_down<u64>(base, page) };
}

// What the firmware keeps: the kernel can't have it either.
static void reserve(u64 base, u64 size)
{
    if (s_reserved_count == array_size(s_reserved))
        fail("reading the device tree"_sv, Error::from_string_literal("too many reserved regions"));
    s_reserved[s_reserved_count++] = { align_down<u64>(base, page), align_up<u64>(base + size, page) - align_down<u64>(base, page) };
    avoid(base, size);
}

ErrorOr<UEFI::PhysicalAddress> allocate_pages(UEFI::BootServices&, size_t size, LoaderMemoryType type)
{
    size = align_up<size_t>(size, page);
    for (;;) {
        u64 const base = align_up<u64>(s_next_free, page);
        bool clashed = false;
        for (size_t i = 0; i < s_avoid_count; ++i) {
            if (s_avoid[i].overlaps(base, size)) {
                s_next_free = s_avoid[i].end();
                clashed = true;
                break;
            }
        }
        if (clashed)
            continue;
        if (base + size > s_free_end)
            return Error::from_string_literal("out of memory");
        s_next_free = base + size;
        if (s_allocation_count > 0) {
            auto& last = s_allocations[s_allocation_count - 1];
            if (last.type == type && last.range.end() == base) {
                last.range.size += size;
                __builtin_memset(reinterpret_cast<void*>(base), 0, size);
                return base;
            }
        }
        if (s_allocation_count == array_size(s_allocations))
            return Error::from_string_literal("too many allocations");
        s_allocations[s_allocation_count++] = { { base, size }, type };
        __builtin_memset(reinterpret_cast<void*>(base), 0, size);
        return base;
    }
}

// The memory map being put together: no two overlap, as a later one takes what it needs from those before.
static API::MemoryRegion s_map[256];
static size_t s_map_count = 0;

static void set_memory_type(u64 base, u64 size, API::MemoryRegionType type)
{
    if (size == 0)
        return;
    for (size_t i = 0; i < s_map_count;) {
        auto const old = s_map[i];
        u64 const old_end = old.base + old.length;
        if (old_end <= base || base + size <= old.base) {
            ++i;
            continue;
        }
        // What's left of it on either side.
        s_map[i] = s_map[--s_map_count];
        if (old.base < base)
            s_map[s_map_count++] = { old.base, base - old.base, old.type, 0 };
        if (old_end > base + size)
            s_map[s_map_count++] = { base + size, old_end - (base + size), old.type, 0 };
    }
    if (s_map_count == array_size(s_map))
        fail("making the memory map"_sv, Error::from_string_literal("too many regions"));
    s_map[s_map_count++] = { base, size, type, 0 };
}

static void collect_memory(FlattenedDeviceTree const& tree)
{
    bool found = false;
    auto result = tree.for_each_node([&](DeviceTree::Node const& node) {
        if (node.depth != 1)
            return DeviceTree::IterationDecision::Continue;
        auto kind = tree.find_property(node, "device_type"_sv);
        if (!kind.has_value() || kind->as_string() != "memory"_sv)
            return DeviceTree::IterationDecision::Continue;
        for (size_t i = 0;; ++i) {
            auto region = tree.reg(node, i);
            if (!region.has_value())
                break;
            if (region->size == 0)
                continue;
            found = true;
            u64 const base = align_up<u64>(region->address, page);
            u64 const end = align_down<u64>(region->address + region->size, page);
            if (end > base)
                set_memory_type(base, end - base, API::MemoryRegionType::Usable);
            if (base == 0)
                s_free_end = min(end, mapped_memory_end);
        }
        return DeviceTree::IterationDecision::Continue;
    });
    if (result.is_error())
        fail("reading the device tree"_sv, result.error());
    if (!found || s_free_end == 0)
        fail("reading the device tree"_sv, Error::from_string_literal("it doesn't say where memory is"));
}

// What the firmware says it's still using, or that's used by what it left.
static void collect_reservations(FlattenedDeviceTree const& tree)
{
    // Where the other processors wait, whatever else is said.
    reserve(0, page);
    for (size_t i = 0;; ++i) {
        auto region = tree.memory_reservation(i);
        if (!region.has_value())
            break;
        reserve(region->address, region->size);
    }
    bool in_reserved_memory = false;
    auto result = tree.for_each_node([&](DeviceTree::Node const& node) {
        if (node.depth == 1) {
            in_reserved_memory = node.name == "reserved-memory"_sv;
        } else if (in_reserved_memory && node.depth == 2) {
            if (auto region = tree.reg(node, 0); region.has_value())
                reserve(region->address, region->size);
        }
        return DeviceTree::IterationDecision::Continue;
    });
    if (result.is_error())
        fail("reading the device tree"_sv, result.error());
}

// ---- The serial port: the first PL011, or the one the tree says to use.

static void find_serial_port(FlattenedDeviceTree const& tree, API::BootInfo& boot_info)
{
    auto is_pl011 = [&](DeviceTree::Node const& node) {
        auto compatible = tree.find_property(node, "compatible"_sv);
        return compatible.has_value() && (compatible->string_list_contains("arm,pl011"_sv) || compatible->string_list_contains("arm,pl011-axi"_sv));
    };
    auto serial = tree.find_stdout_node();
    if (!serial.has_value() || !is_pl011(serial.value()))
        serial = tree.find_compatible_node("arm,pl011"_sv);
    if (!serial.has_value())
        serial = tree.find_compatible_node("arm,pl011-axi"_sv);
    if (!serial.has_value())
        return;
    if (auto region = tree.reg(serial.value(), 0); region.has_value()) {
        boot_info.serial = { region->address, API::SerialType::PL011, 0 };
        s_serial = reinterpret_cast<u32 volatile*>(region->address);
        // The firmware only sets it up if config.txt says so. Then: 115200 baud, from the 48 MHz it runs on by then.
        constexpr size_t integer_divisor = 0x24 / 4;
        constexpr size_t fractional_divisor = 0x28 / 4;
        constexpr size_t line_control = 0x2c / 4;
        constexpr size_t control = 0x30 / 4;
        constexpr u32 enabled_with_both_directions = (1 << 0) | (1 << 8) | (1 << 9);
        if (!(s_serial[control] & 1)) {
            s_serial[integer_divisor] = 26;
            s_serial[fractional_divisor] = 3;
            s_serial[line_control] = (3 << 5) | (1 << 4); // Eight bits, and its queues.
            s_serial[control] = enabled_with_both_directions;
        }
    }
}

// ---- Asking the firmware, through the mailbox.

class Mailbox {
public:
    explicit Mailbox(FlattenedDeviceTree const& tree)
    {
        auto node = tree.find_compatible_node("brcm,bcm2835-mbox"_sv);
        if (!node.has_value())
            return;
        if (auto region = tree.reg(node.value(), 0); region.has_value())
            m_registers = region->address;
    }

    [[nodiscard]] bool is_present() const { return m_registers != 0; }

    // A message: words[0] the size, words[1] request or answer, then the tags. Answers come back in it.
    bool call(u32* words, size_t size)
    {
        if (!m_registers)
            return false;
        constexpr FlatPtr read_register = 0x00;
        constexpr FlatPtr read_status = 0x18;
        constexpr FlatPtr write_register = 0x20;
        constexpr FlatPtr write_status = 0x38;
        constexpr u32 is_full = 1u << 31;
        constexpr u32 is_empty = 1u << 30;
        constexpr u32 channel = 8;
        // The firmware's way of saying "this address, not through the cache". It's what's asked for: this does the
        // cache's part here.
        constexpr u32 uncached = 0xc000'0000;
        constexpr u32 longest_wait = 10'000'000;
        constexpr u32 answer_is_good = 0x8000'0000;

        words[0] = static_cast<u32>(size);
        words[1] = 0;
        flush_cache(words, size);

        auto const address = static_cast<u32>(reinterpret_cast<u64>(words)) | uncached;
        for (u32 wait = 0; register_at(write_status) & is_full; ++wait) {
            if (wait == longest_wait)
                return false;
        }
        asm volatile("dsb sy" ::: "memory");
        register_at(write_register) = address | channel;
        for (u32 wait = 0;; ++wait) {
            if (!(register_at(read_status) & is_empty)) {
                if (register_at(read_register) == (address | channel))
                    break;
                continue;
            }
            if (wait == longest_wait)
                return false;
        }
        asm volatile("dsb sy" ::: "memory");
        flush_cache(words, size);
        return words[1] == answer_is_good;
    }

private:
    [[nodiscard]] u32 volatile& register_at(FlatPtr offset) const { return *reinterpret_cast<u32 volatile*>(m_registers + offset); }

    // Written back, and gone: whatever the firmware writes next is what's read.
    static void flush_cache(void const* start, size_t size)
    {
        constexpr u64 smallest_line = 32;
        auto const first = align_down<u64>(reinterpret_cast<u64>(start), smallest_line);
        for (u64 address = first; address < reinterpret_cast<u64>(start) + size; address += smallest_line)
            asm volatile("dc civac, %0" ::"r"(address) : "memory");
        asm volatile("dsb sy" ::: "memory");
    }

    u64 m_registers { 0 };
};

// Message buffers are at least this aligned, by the mailbox's rule.
alignas(64) static u32 s_message[64];

static API::FramebufferInfo ask_for_framebuffer(Mailbox& mailbox)
{
    constexpr u32 tag_get_physical_size = 0x0004'0003;
    constexpr u32 tag_set_physical_size = 0x0004'8003;
    constexpr u32 tag_set_virtual_size = 0x0004'8004;
    constexpr u32 tag_set_virtual_offset = 0x0004'8009;
    constexpr u32 tag_set_depth = 0x0004'8005;
    constexpr u32 tag_set_pixel_order = 0x0004'8006;
    constexpr u32 tag_set_alpha_mode = 0x0004'8007;
    constexpr u32 tag_allocate = 0x0004'0001;
    constexpr u32 tag_get_pitch = 0x0004'0008;
    constexpr u32 pixel_order_bgr = 0;
    constexpr u32 alpha_ignored = 2;
    constexpr u32 fallback_width = 1280;
    constexpr u32 fallback_height = 800;
    // Addresses the firmware gives are for its own view of memory: this takes off which alias of it they're in.
    constexpr u32 alias_mask = 0x3fff'ffff;

    // What the screen is, as far as the firmware knows.
    u32 width = fallback_width;
    u32 height = fallback_height;
    size_t at = 2;
    s_message[at++] = tag_get_physical_size;
    s_message[at++] = 8;
    s_message[at++] = 0;
    s_message[at++] = 0;
    s_message[at++] = 0;
    s_message[at++] = 0;
    if (mailbox.call(s_message, (at) * 4) && s_message[5] >= 640 && s_message[6] >= 480) {
        width = s_message[5];
        height = s_message[6];
    }

    at = 2;
    auto tag = [&](u32 name, u32 first, Optional<u32> second = {}) {
        s_message[at++] = name;
        s_message[at++] = second.has_value() ? 8 : 4;
        s_message[at++] = 0;
        s_message[at++] = first;
        if (second.has_value())
            s_message[at++] = second.value();
    };
    tag(tag_set_physical_size, width, height);
    tag(tag_set_virtual_size, width, height);
    tag(tag_set_virtual_offset, 0, 0);
    tag(tag_set_depth, 32);
    size_t const order_answer = at + 3;
    tag(tag_set_pixel_order, pixel_order_bgr);
    tag(tag_set_alpha_mode, alpha_ignored);
    size_t const buffer_answer = at + 3;
    tag(tag_allocate, static_cast<u32>(page), 0);
    size_t const pitch_answer = at + 3;
    tag(tag_get_pitch, 0);
    s_message[at++] = 0;
    if (!mailbox.call(s_message, at * 4) || s_message[buffer_answer] == 0 || s_message[pitch_answer] == 0)
        return {};

    API::FramebufferInfo framebuffer {};
    framebuffer.physical_base = s_message[buffer_answer] & alias_mask;
    framebuffer.size = s_message[buffer_answer + 1];
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.pitch = s_message[pitch_answer];
    framebuffer.format = s_message[order_answer] == pixel_order_bgr ? API::PixelFormat::BGRX8888 : API::PixelFormat::RGBX8888;
    return framebuffer;
}

// What drives the SD controller, on a Pi whose controller doesn't say.
static u64 ask_for_sd_clock(Mailbox& mailbox)
{
    constexpr u32 tag_get_clock_rate = 0x0003'0002;
    constexpr u32 clock_emmc2 = 12;
    size_t at = 2;
    s_message[at++] = tag_get_clock_rate;
    s_message[at++] = 8;
    s_message[at++] = 0;
    s_message[at++] = clock_emmc2;
    s_message[at++] = 0;
    s_message[at++] = 0;
    if (!mailbox.call(s_message, at * 4))
        return 0;
    return s_message[6];
}

// ---- Randomness: the hardware's generator, and whatever the firmware has passed on.

static size_t add_entropy(API::BootInfo& boot_info, size_t& next, ReadonlyBytes bytes)
{
    size_t used = 0;
    for (u8 byte : bytes) {
        boot_info.entropy[next % sizeof(boot_info.entropy)] ^= byte;
        ++next;
        ++used;
    }
    return used;
}

static void collect_entropy(FlattenedDeviceTree const& tree, API::BootInfo& boot_info)
{
    size_t next = 0;
    size_t real_bytes = 0;
    if (auto chosen = tree.find_node_by_path("/chosen"_sv); chosen.has_value()) {
        static constexpr StringView seeds[] = { "rng-seed"_sv, "kaslr-seed"_sv };
        for (auto name : seeds) {
            if (auto seed = tree.find_property(chosen.value(), name); seed.has_value())
                real_bytes += add_entropy(boot_info, next, seed->value);
        }
    }
    Optional<DeviceTree::Node> generator;
    static constexpr StringView generators[] = { "brcm,bcm2711-rng200"_sv, "brcm,bcm2712-rng200"_sv, "brcm,iproc-rng200"_sv };
    for (auto name : generators) {
        generator = tree.find_compatible_node(name);
        if (generator.has_value())
            break;
    }
    if (generator.has_value()) {
        if (auto region = tree.reg(generator.value(), 0); region.has_value()) {
            constexpr size_t control = 0x00 / 4;
            constexpr size_t fifo_data = 0x20 / 4;
            constexpr size_t fifo_count = 0x24 / 4;
            auto* registers = reinterpret_cast<u32 volatile*>(region->address);
            registers[control] = registers[control] | 1;
            for (size_t word = 0; word < sizeof(boot_info.entropy) / 4; ++word) {
                u32 wait = 0;
                while ((registers[fifo_count] & 0xff) == 0 && wait < 100'000)
                    ++wait;
                if (wait == 100'000)
                    break;
                u32 const value = registers[fifo_data];
                real_bytes += add_entropy(boot_info, next, { reinterpret_cast<u8 const*>(&value), sizeof(value) });
            }
        }
    }
    boot_info.entropy_size = min(real_bytes, sizeof(boot_info.entropy));
}

static u64 choose_kernel_slide(API::BootInfo const& boot_info)
{
    if (StringView(boot_info.command_line).contains("nokaslr"_sv))
        return 0;
    constexpr u64 alignment = 64 * KiB;
    constexpr u64 choices = 16384;
    u64 random;
    asm volatile("mrs %0, cntpct_el0" : "=r"(random));
    for (size_t i = 0; i + 8 <= sizeof(boot_info.entropy); i += 8) {
        u64 word;
        __builtin_memcpy(&word, boot_info.entropy + i, 8);
        random ^= word;
    }
    // (It's the low bits that are used: the counter only starts to count for much after a while.)
    random ^= random >> 32;
    return (random % choices) * alignment;
}

// The firmware puts cmdline.txt last, after what it adds itself, so if something must go, it's the start.
static void set_command_line(FlattenedDeviceTree const& tree, API::BootInfo& boot_info)
{
    auto chosen = tree.find_node_by_path("/chosen"_sv);
    if (!chosen.has_value())
        return;
    auto arguments = tree.find_property(chosen.value(), "bootargs"_sv);
    if (!arguments.has_value())
        return;
    auto text = arguments->as_string().trim_whitespace();
    size_t const capacity = sizeof(boot_info.command_line) - 1;
    if (text.length() > capacity)
        text = text.substring_view(text.length() - capacity);
    for (size_t i = 0; i < text.length(); ++i) {
        char const character = text[i];
        boot_info.command_line[i] = is_ascii_printable(static_cast<u8>(character)) ? character : ' ';
    }
    boot_info.command_line[text.length()] = '\0';
}

// ---- Handing over.

static ErrorOr<void> map_kernel(PageTableBuilder& tables, ELF::Image const& image, LoadedKernel const& kernel)
{
    for (auto const& segment : image.program_headers()) {
        if (segment.p_type != ELF::PT_LOAD || segment.p_memsz == 0)
            continue;
        auto access = Access::ReadOnly;
        if (segment.p_flags & ELF::PF_X)
            access = Access::ReadExecute;
        else if (segment.p_flags & ELF::PF_W)
            access = Access::ReadWrite;
        u64 const address = segment.p_vaddr + kernel.slide;
        u64 const physical = kernel.physical_base + (address - kernel.virtual_base);
        TRY(tables.map(address, physical, align_up<u64>(segment.p_memsz, page), MemoryAttribute::Normal, access));
    }
    return {};
}

static API::MemoryRegionType type_of(LoaderMemoryType type)
{
    switch (type) {
    case LoaderMemoryType::KernelImage:
        return API::MemoryRegionType::KernelImage;
    case LoaderMemoryType::KernelFile:
        return API::MemoryRegionType::KernelFile;
    case LoaderMemoryType::BootArchive:
        return API::MemoryRegionType::BootArchive;
    case LoaderMemoryType::LowMemory:
        return API::MemoryRegionType::LowMemory;
    case LoaderMemoryType::LoaderReclaimable:
        break;
    }
    return API::MemoryRegionType::LoaderReclaimable;
}

// Sorted by address, adjacent regions of the same type merged, as the kernel expects.
static size_t finish_memory_map(API::MemoryRegion* output, size_t capacity)
{
    for (size_t i = 1; i < s_map_count; ++i) {
        auto const moving = s_map[i];
        size_t at = i;
        while (at > 0 && s_map[at - 1].base > moving.base) {
            s_map[at] = s_map[at - 1];
            --at;
        }
        s_map[at] = moving;
    }
    size_t count = 0;
    for (size_t i = 0; i < s_map_count; ++i) {
        auto const& region = s_map[i];
        if (count > 0 && output[count - 1].type == region.type && output[count - 1].base + output[count - 1].length == region.base) {
            output[count - 1].length += region.length;
            continue;
        }
        if (count == capacity)
            fail("making the memory map"_sv, Error::from_string_literal("too many regions"));
        output[count++] = region;
    }
    return count;
}

[[noreturn]] static void boot(u64 device_tree_address, u64 load_base)
{
    u64 const image_size = reinterpret_cast<u64>(__image_end) - reinterpret_cast<u64>(__image_start);
    u64 const stub_base = reinterpret_cast<u64>(__image_start);
    u64 const stub_end = reinterpret_cast<u64>(__image_end);

    // The device tree: a header, then what it says.
    if (device_tree_address == 0)
        stop();
    auto read_be32 = [](u64 address) {
        auto const* bytes = reinterpret_cast<u8 const*>(address);
        return (u32(bytes[0]) << 24) | (u32(bytes[1]) << 16) | (u32(bytes[2]) << 8) | u32(bytes[3]);
    };
    constexpr u32 most_device_tree = 2 * MiB;
    u32 const device_tree_size = read_be32(device_tree_address + 4);
    if (device_tree_size > most_device_tree)
        stop();
    auto tree = unwrap("reading the device tree"_sv, FlattenedDeviceTree::parse(ReadonlyBytes(reinterpret_cast<u8 const*>(device_tree_address), device_tree_size)));

    static API::BootInfo s_early_info {};
    find_serial_port(tree, s_early_info);
    format_to(s_output, "\nvultureOS {} - VultureBoot (Raspberry Pi)\n", VULTURE_VERSION_STRING);

    // What's in the file after the stub.
    u64 const payload_base = load_base + image_size;
    auto const& payload = *reinterpret_cast<RaspberryPi::PayloadHeader const*>(payload_base);
    if (__builtin_memcmp(payload.magic, RaspberryPi::payload_magic, sizeof(payload.magic)) != 0)
        fail("reading kernel8.img"_sv, Error::from_string_literal("there's nothing after the stub"));
    Range const kernel_file { payload_base + payload.kernel_offset, payload.kernel_size };
    Range const archive_file { payload_base + payload.archive_offset, payload.archive_size };
    u64 const everything_loaded_end = align_up<u64>(archive_file.end(), page);

    collect_memory(tree);
    collect_reservations(tree);
    avoid(stub_base, stub_end - stub_base);
    avoid(load_base, everything_loaded_end - load_base);
    avoid(device_tree_address, device_tree_size);
    s_next_free = stub_end;

    Mailbox mailbox(tree);
    auto framebuffer = ask_for_framebuffer(mailbox);
    // It's in the memory the video core keeps. If the tree says otherwise (as QEMU's -dtb does), it's not ours.
    if (framebuffer.format != API::PixelFormat::None)
        reserve(framebuffer.physical_base, framebuffer.size);
    u64 const sd_clock = ask_for_sd_clock(mailbox);

    // BootInfo, and the memory map after it.
    size_t const regions_offset = align_up<size_t>(sizeof(API::BootInfo), 16);
    constexpr size_t region_capacity = 256;
    size_t const info_size = regions_offset + region_capacity * sizeof(API::MemoryRegion);
    auto const info_address = unwrap("allocating boot information"_sv, allocate_pages(s_no_firmware, info_size, LoaderMemoryType::LoaderReclaimable));
    auto& boot_info = *reinterpret_cast<API::BootInfo*>(info_address);
    auto* regions = reinterpret_cast<API::MemoryRegion*>(info_address + regions_offset);
    boot_info.magic = API::boot_info_magic;
    boot_info.version = API::boot_info_version;
    boot_info.size = sizeof(API::BootInfo);
    boot_info.memory_map = info_address + regions_offset;
    boot_info.serial = s_early_info.serial;
    boot_info.framebuffer = framebuffer;
    boot_info.storage_clock_hz = sd_clock;
    boot_info.kernel_file = { kernel_file.base, kernel_file.size };
    boot_info.boot_archive = { archive_file.base, archive_file.size };

    auto const tree_copy = unwrap("copying the device tree"_sv, allocate_pages(s_no_firmware, device_tree_size, LoaderMemoryType::LoaderReclaimable));
    __builtin_memcpy(reinterpret_cast<void*>(tree_copy), reinterpret_cast<void const*>(device_tree_address), device_tree_size);
    boot_info.device_tree = { tree_copy, device_tree_size };

    set_command_line(tree, boot_info);
    collect_entropy(tree, boot_info);

    auto image = unwrap("reading the kernel"_sv, ELF::Image::parse(ReadonlyBytes(reinterpret_cast<u8 const*>(kernel_file.base), kernel_file.size)));
    auto kernel = unwrap("loading the kernel"_sv, load_kernel(image, s_no_firmware, choose_kernel_slide(boot_info)));
    boot_info.kernel_image = { kernel.physical_base, kernel.size };
    boot_info.kernel_virtual_base = kernel.virtual_base;
    boot_info.kernel_slide = kernel.slide;

    format_to(s_output, "Kernel    {} KiB at {:#x}, entry {:#x}\n", kernel.size / KiB, kernel.physical_base, kernel.entry);
    if (framebuffer.format != API::PixelFormat::None)
        format_to(s_output, "Display   {}x{} at {:#x}\n", framebuffer.width, framebuffer.height, framebuffer.physical_base);
    else
        format_to(s_output, "Display   none\n");
    format_to(s_output, "Boot args {}\n", boot_info.command_line[0] ? StringView(boot_info.command_line) : "(none)"_sv);
    format_to(s_output, "\nStarting vultureKernel...\n");

    // All the memory there is, without what's the firmware's. What's allocated from here on is in it too.
    for (size_t i = 0; i < s_reserved_count; ++i)
        set_memory_type(s_reserved[i].base, s_reserved[i].size, API::MemoryRegionType::Reserved);

    auto kernel_tables = unwrap("creating page tables"_sv, PageTableBuilder::create(s_no_firmware));
    auto identity_tables = unwrap("creating page tables"_sv, PageTableBuilder::create(s_no_firmware));
    for (size_t i = 0; i < s_map_count; ++i) {
        auto const& region = s_map[i];
        if (region.type != API::MemoryRegionType::Usable)
            continue;
        unwrap("mapping physical memory"_sv, kernel_tables.map(API::direct_map_base + region.base, region.base, region.length, MemoryAttribute::Normal, Access::ReadWrite));
    }
    unwrap("mapping the kernel"_sv, map_kernel(kernel_tables, image, kernel));
    if (framebuffer.format != API::PixelFormat::None) {
        // The video core doesn't look in this cache.
        u64 const base = align_down<u64>(framebuffer.physical_base, page);
        u64 const size = align_up<u64>(framebuffer.physical_base + framebuffer.size, page) - base;
        unwrap("mapping the framebuffer"_sv, kernel_tables.map(API::direct_map_base + base, base, size, MemoryAttribute::NormalUncached, Access::ReadWrite));
    }
    if (boot_info.serial.type == API::SerialType::PL011) {
        u64 const base = align_down<u64>(boot_info.serial.physical_base, page);
        unwrap("mapping the serial port"_sv, kernel_tables.map(API::direct_map_base + base, base, page, MemoryAttribute::Device, Access::ReadWrite));
    }
    u64 const trampoline_page = align_down<u64>(AArch64::trampoline_address(), page);
    unwrap("mapping the handoff code"_sv, identity_tables.map(trampoline_page, trampoline_page, 2 * page, MemoryAttribute::Normal, Access::ReadExecute));

    // Now that everything's been allocated: what became of each part of memory.
    set_memory_type(stub_base, stub_end - stub_base, API::MemoryRegionType::LoaderReclaimable);
    set_memory_type(load_base, everything_loaded_end - load_base, API::MemoryRegionType::LoaderReclaimable);
    set_memory_type(kernel_file.base, align_up<u64>(kernel_file.size, page), API::MemoryRegionType::KernelFile);
    set_memory_type(archive_file.base, align_up<u64>(archive_file.size, page), API::MemoryRegionType::BootArchive);
    for (size_t i = 0; i < s_allocation_count; ++i)
        set_memory_type(s_allocations[i].range.base, s_allocations[i].range.size, type_of(s_allocations[i].type));
    boot_info.memory_map_entries = finish_memory_map(regions, region_capacity);

    // Everything the kernel reads first has to be in memory, as it's turned on with the cache cold, and its code
    // has to be seen by instruction fetch.
    AArch64::synchronize_instruction_cache(kernel.physical_base, kernel.size);
    for (size_t i = 0; i < s_allocation_count; ++i)
        AArch64::clean_data_cache(s_allocations[i].range.base, s_allocations[i].range.size);
    AArch64::clean_data_cache(stub_base, stub_end - stub_base);

    auto state = AArch64::make_handoff_state(identity_tables.root(), kernel_tables.root(), kernel.entry, API::direct_map_base + info_address);
    AArch64::enter_kernel(state);
}

}

extern "C" [[noreturn]] void stub_main(u64 device_tree_address, u64 load_base)
{
    Boot::boot(device_tree_address, load_base);
}

// Runtime hooks: nothing else here has them.

extern "C" size_t strlen(char const* text)
{
    size_t length = 0;
    while (text[length])
        ++length;
    return length;
}

void Base::verification_failed(char const* expression, char const* file, unsigned line)
{
    format_to(Boot::s_output, "VultureBoot (Raspberry Pi): VERIFY failed: {} at {}:{}\n", StringView(expression, __builtin_strlen(expression)), StringView(file, __builtin_strlen(file)), line);
    Boot::stop();
}

extern "C" [[noreturn]] void __stack_chk_fail()
{
    Boot::stop();
}

extern "C" [[noreturn]] void __cxa_pure_virtual()
{
    VERIFY_NOT_REACHED();
}
