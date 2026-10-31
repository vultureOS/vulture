#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

set(linker_script ${CMAKE_CURRENT_SOURCE_DIR}/Arch/aarch64/linker.ld)

add_executable(Kernel
    Arch/aarch64/Boot.S
    Arch/aarch64/CPU.cpp
    Arch/aarch64/ContextSwitch.S
    Arch/aarch64/Exceptions.cpp
    Arch/aarch64/GICv3.cpp
    Arch/aarch64/GenericTimer.cpp
    Arch/aarch64/PSCI.cpp
    Arch/aarch64/PageTable.cpp
    Arch/aarch64/SMP.cpp
    Arch/aarch64/ThreadContext.cpp
    Arch/aarch64/UserAccess.S
    Arch/aarch64/UserContext.cpp
    Arch/aarch64/Vectors.S
    BootArguments.cpp
    Console/BootScreen.cpp
    Console/Font.cpp
    Console/Framebuffer.cpp
    Console/TextConsole.cpp
    Debug/KernelLog.cpp
    Debug/Panic.cpp
    Debug/Symbols.cpp
    Devices/Serial/PL011.cpp
    FileSystem/BootArchive.cpp
    FileSystem/OpenFile.cpp
    Init.cpp
    Interrupts/InterruptHandlers.cpp
    Library/CxxRuntime.cpp
    Locking/Mutex.cpp
    Locking/SpinLock.cpp
    Locking/WaitQueue.cpp
    Memory/AddressSpace.cpp
    Memory/AnonymousVMObject.cpp
    Memory/BuddyAllocator.cpp
    Memory/Heap.cpp
    Memory/MemoryManager.cpp
    Memory/PhysicalMemory.cpp
    Memory/UserAccess.cpp
    Syscalls/Dispatch.cpp
    Syscalls/FileCalls.cpp
    Syscalls/MemoryCalls.cpp
    Syscalls/ProcessCalls.cpp
    Syscalls/SignalCalls.cpp
    Syscalls/TimeCalls.cpp
    Tasks/Process.cpp
    Tasks/Processor.cpp
    Tasks/Scheduler.cpp
    Tasks/Thread.cpp
    Tests/KernelTest.cpp
    Tests/TestBenchmarks.cpp
    Tests/TestHeap.cpp
    Tests/TestLocks.cpp
    Tests/TestMemoryManager.cpp
    Tests/TestPhysicalMemory.cpp
    Tests/TestProcessors.cpp
    Tests/TestScheduler.cpp
    Tests/TestSpinLock.cpp
    Tests/TestThreads.cpp
    Tests/TestTimer.cpp
    Tests/VectorRegisters.S
    Time/Time.cpp
    Time/TimerQueue.cpp)

set_target_properties(Kernel PROPERTIES
    OUTPUT_NAME vulture
    SUFFIX ""
    LINK_DEPENDS ${linker_script})

target_link_options(Kernel PRIVATE
    -T ${linker_script}
    -Wl,-z,max-page-size=4096
    -Wl,--build-id=none)

target_link_libraries(Kernel PRIVATE LibArchive LibDeviceTree LibELF LibBase LibBaseFreestanding)

install(TARGETS Kernel RUNTIME DESTINATION System/Library/Kernels)
