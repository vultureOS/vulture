/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <LibBase/Types.h>

// UEFI definitions written from the UEFI 2.10 specification. Structure and
// member names mirror the specification so they can be looked up directly.
// Only the services VultureBoot calls are typed; the other table slots stay
// opaque pointers so the layout is preserved.
//
// The loader is compiled for a Windows target, whose calling convention is the
// one UEFI uses on both AArch64 and x86_64, so no annotations are needed.

namespace UEFI {

using UIntN = size_t;
using Status = UIntN;
using Handle = void*;
using Event = void*;
using Boolean = u8;
using PhysicalAddress = u64;
using VirtualAddress = u64;

constexpr Status error_bit = Status(1) << 63;

namespace StatusCode {
constexpr Status Success = 0;
constexpr Status LoadError = error_bit | 1;
constexpr Status InvalidParameter = error_bit | 2;
constexpr Status Unsupported = error_bit | 3;
constexpr Status BadBufferSize = error_bit | 4;
constexpr Status BufferTooSmall = error_bit | 5;
constexpr Status NotReady = error_bit | 6;
constexpr Status DeviceError = error_bit | 7;
constexpr Status OutOfResources = error_bit | 9;
constexpr Status NotFound = error_bit | 14;
}

[[nodiscard]] constexpr bool is_error(Status status) { return (status & error_bit) != 0; }

struct Guid {
    u32 Data1;
    u16 Data2;
    u16 Data3;
    u8 Data4[8];

    constexpr bool operator==(Guid const&) const = default;
};

struct TableHeader {
    u64 Signature;
    u32 Revision;
    u32 HeaderSize;
    u32 CRC32;
    u32 Reserved;
};

// Text console

enum class TextColor : u8 {
    Black = 0x0,
    Blue = 0x1,
    Green = 0x2,
    Cyan = 0x3,
    Red = 0x4,
    Magenta = 0x5,
    Brown = 0x6,
    LightGray = 0x7,
    DarkGray = 0x8,
    LightBlue = 0x9,
    LightGreen = 0xa,
    LightCyan = 0xb,
    LightRed = 0xc,
    LightMagenta = 0xd,
    Yellow = 0xe,
    White = 0xf,
};

struct InputKey {
    u16 ScanCode;
    char16_t UnicodeChar;
};

struct SimpleTextInputProtocol {
    Status (*Reset)(SimpleTextInputProtocol* self, Boolean extended_verification);
    Status (*ReadKeyStroke)(SimpleTextInputProtocol* self, InputKey* key);
    Event WaitForKey;
};

struct SimpleTextOutputMode {
    i32 MaxMode;
    i32 Mode;
    i32 Attribute;
    i32 CursorColumn;
    i32 CursorRow;
    Boolean CursorVisible;
};

struct SimpleTextOutputProtocol {
    Status (*Reset)(SimpleTextOutputProtocol* self, Boolean extended_verification);
    Status (*OutputString)(SimpleTextOutputProtocol* self, char16_t const* string);
    Status (*TestString)(SimpleTextOutputProtocol* self, char16_t const* string);
    Status (*QueryMode)(SimpleTextOutputProtocol* self, UIntN mode_number, UIntN* columns, UIntN* rows);
    Status (*SetMode)(SimpleTextOutputProtocol* self, UIntN mode_number);
    Status (*SetAttribute)(SimpleTextOutputProtocol* self, UIntN attribute);
    Status (*ClearScreen)(SimpleTextOutputProtocol* self);
    Status (*SetCursorPosition)(SimpleTextOutputProtocol* self, UIntN column, UIntN row);
    Status (*EnableCursor)(SimpleTextOutputProtocol* self, Boolean visible);
    SimpleTextOutputMode* Mode;
};

// Memory

enum class AllocateType : u32 {
    AnyPages,
    MaxAddress,
    Address,
};

enum class MemoryType : u32 {
    ReservedMemoryType,
    LoaderCode,
    LoaderData,
    BootServicesCode,
    BootServicesData,
    RuntimeServicesCode,
    RuntimeServicesData,
    ConventionalMemory,
    UnusableMemory,
    ACPIReclaimMemory,
    ACPIMemoryNVS,
    MemoryMappedIO,
    MemoryMappedIOPortSpace,
    PalCode,
    PersistentMemory,
    UnacceptedMemoryType,
};

// Walk a memory map with the DescriptorSize returned by GetMemoryMap(), never
// sizeof(MemoryDescriptor): firmware may append fields.
struct MemoryDescriptor {
    u32 Type;
    PhysicalAddress PhysicalStart;
    VirtualAddress VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
};

constexpr u64 page_size = 4096;

// Graphics

enum class GraphicsPixelFormat : u32 {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
};

struct PixelBitmask {
    u32 RedMask;
    u32 GreenMask;
    u32 BlueMask;
    u32 ReservedMask;
};

struct GraphicsOutputModeInformation {
    u32 Version;
    u32 HorizontalResolution;
    u32 VerticalResolution;
    GraphicsPixelFormat PixelFormat;
    PixelBitmask PixelInformation;
    u32 PixelsPerScanLine;
};

struct GraphicsOutputProtocolMode {
    u32 MaxMode;
    u32 Mode;
    GraphicsOutputModeInformation* Info;
    UIntN SizeOfInfo;
    PhysicalAddress FrameBufferBase;
    UIntN FrameBufferSize;
};

struct GraphicsOutputProtocol {
    Status (*QueryMode)(GraphicsOutputProtocol* self, u32 mode_number, UIntN* size_of_info, GraphicsOutputModeInformation** info);
    Status (*SetMode)(GraphicsOutputProtocol* self, u32 mode_number);
    void* Blt;
    GraphicsOutputProtocolMode* Mode;
};

constexpr Guid graphics_output_protocol_guid { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };

// Files

struct FileProtocol {
    u64 Revision;
    Status (*Open)(FileProtocol* self, FileProtocol** new_handle, char16_t const* file_name, u64 open_mode, u64 attributes);
    Status (*Close)(FileProtocol* self);
    Status (*Delete)(FileProtocol* self);
    Status (*Read)(FileProtocol* self, UIntN* buffer_size, void* buffer);
    Status (*Write)(FileProtocol* self, UIntN* buffer_size, void const* buffer);
    Status (*GetPosition)(FileProtocol* self, u64* position);
    Status (*SetPosition)(FileProtocol* self, u64 position);
    void* GetInfo;
    void* SetInfo;
    void* Flush;
};

constexpr u64 file_mode_read = 1;
// SetPosition() to this moves to the end of the file.
constexpr u64 file_position_end = ~u64(0);

struct SimpleFileSystemProtocol {
    u64 Revision;
    Status (*OpenVolume)(SimpleFileSystemProtocol* self, FileProtocol** root);
};

constexpr Guid simple_file_system_protocol_guid { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

struct LoadedImageProtocol {
    u32 Revision;
    Handle ParentHandle;
    void* SystemTable;
    Handle DeviceHandle;
    void* FilePath;
    void* Reserved;
    u32 LoadOptionsSize;
    void* LoadOptions;
    void* ImageBase;
    u64 ImageSize;
    MemoryType ImageCodeType;
    MemoryType ImageDataType;
    void* Unload;
};

constexpr Guid loaded_image_protocol_guid { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

// Random numbers

struct RngProtocol {
    Status (*GetInfo)(RngProtocol* self, UIntN* algorithm_list_size, Guid* algorithm_list);
    Status (*GetRNG)(RngProtocol* self, Guid const* algorithm, UIntN value_length, u8* value);
};

constexpr Guid rng_protocol_guid { 0x3152bca5, 0xeade, 0x433d, { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } };

// Configuration tables

constexpr Guid acpi_20_table_guid { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
constexpr Guid device_tree_table_guid { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
constexpr Guid smbios3_table_guid { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } };

struct ConfigurationTable {
    Guid VendorGuid;
    void* VendorTable;
};

// Services

enum class ResetType : u32 {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific,
};

struct RuntimeServices {
    TableHeader Hdr;
    void* GetTime;
    void* SetTime;
    void* GetWakeupTime;
    void* SetWakeupTime;
    void* SetVirtualAddressMap;
    void* ConvertPointer;
    void* GetVariable;
    void* GetNextVariableName;
    void* SetVariable;
    void* GetNextHighMonotonicCount;
    void (*ResetSystem)(ResetType reset_type, Status reset_status, UIntN data_size, void const* reset_data);
    void* UpdateCapsule;
    void* QueryCapsuleCapabilities;
    void* QueryVariableInfo;
};

struct BootServices {
    TableHeader Hdr;

    // Task priority
    void* RaiseTPL;
    void* RestoreTPL;

    // Memory
    Status (*AllocatePages)(AllocateType type, MemoryType memory_type, UIntN pages, PhysicalAddress* memory);
    Status (*FreePages)(PhysicalAddress memory, UIntN pages);
    Status (*GetMemoryMap)(UIntN* memory_map_size, MemoryDescriptor* memory_map, UIntN* map_key, UIntN* descriptor_size, u32* descriptor_version);
    Status (*AllocatePool)(MemoryType pool_type, UIntN size, void** buffer);
    Status (*FreePool)(void* buffer);

    // Events and timers
    void* CreateEvent;
    void* SetTimer;
    Status (*WaitForEvent)(UIntN number_of_events, Event* events, UIntN* index);
    void* SignalEvent;
    void* CloseEvent;
    void* CheckEvent;

    // Protocol handlers
    void* InstallProtocolInterface;
    void* ReinstallProtocolInterface;
    void* UninstallProtocolInterface;
    Status (*HandleProtocol)(Handle handle, Guid const* protocol, void** interface);
    void* Reserved;
    void* RegisterProtocolNotify;
    void* LocateHandle;
    void* LocateDevicePath;
    void* InstallConfigurationTable;

    // Images
    void* LoadImage;
    void* StartImage;
    void* Exit;
    void* UnloadImage;
    Status (*ExitBootServices)(Handle image_handle, UIntN map_key);

    // Miscellaneous
    void* GetNextMonotonicCount;
    Status (*Stall)(UIntN microseconds);
    Status (*SetWatchdogTimer)(UIntN timeout, u64 watchdog_code, UIntN data_size, char16_t const* watchdog_data);

    // Driver support
    void* ConnectController;
    void* DisconnectController;

    // Open and close protocol
    void* OpenProtocol;
    void* CloseProtocol;
    void* OpenProtocolInformation;

    // Library
    void* ProtocolsPerHandle;
    Status (*LocateHandleBuffer)(u32 search_type, Guid const* protocol, void* search_key, UIntN* handle_count, Handle** handles);
    Status (*LocateProtocol)(Guid const* protocol, void* registration, void** interface);
    void* InstallMultipleProtocolInterfaces;
    void* UninstallMultipleProtocolInterfaces;

    // CRC and memory helpers
    void* CalculateCrc32;
    void* CopyMem;
    void* SetMem;
    void* CreateEventEx;
};

struct SystemTable {
    TableHeader Hdr;
    char16_t const* FirmwareVendor;
    u32 FirmwareRevision;
    Handle ConsoleInHandle;
    SimpleTextInputProtocol* ConIn;
    Handle ConsoleOutHandle;
    SimpleTextOutputProtocol* ConOut;
    Handle StandardErrorHandle;
    SimpleTextOutputProtocol* StdErr;
    RuntimeServices* RuntimeServices;
    BootServices* BootServices;
    UIntN NumberOfTableEntries;
    ConfigurationTable* ConfigurationTable;
};

// Layout checks against the offsets in the specification (64-bit).
static_assert(sizeof(MemoryDescriptor) == 40);
static_assert(sizeof(GraphicsOutputModeInformation) == 36);
static_assert(__builtin_offsetof(SystemTable, ConOut) == 0x40);
static_assert(__builtin_offsetof(SystemTable, RuntimeServices) == 0x58);
static_assert(__builtin_offsetof(SystemTable, BootServices) == 0x60);
static_assert(__builtin_offsetof(SystemTable, ConfigurationTable) == 0x70);
static_assert(__builtin_offsetof(RuntimeServices, ResetSystem) == 0x68);
static_assert(__builtin_offsetof(BootServices, GetMemoryMap) == 0x38);
static_assert(__builtin_offsetof(BootServices, HandleProtocol) == 0x98);
static_assert(__builtin_offsetof(BootServices, ExitBootServices) == 0xe8);
static_assert(__builtin_offsetof(BootServices, Stall) == 0xf8);
static_assert(__builtin_offsetof(BootServices, LocateProtocol) == 0x140);
static_assert(sizeof(BootServices) == 0x178);
static_assert(__builtin_offsetof(LoadedImageProtocol, DeviceHandle) == 0x18);
static_assert(__builtin_offsetof(LoadedImageProtocol, ImageBase) == 0x40);
static_assert(sizeof(LoadedImageProtocol) == 0x60);
static_assert(__builtin_offsetof(FileProtocol, Read) == 0x20);
static_assert(__builtin_offsetof(FileProtocol, SetPosition) == 0x38);

}
