//
// Copyright (c) 2022-2026, Krisna Pranav
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// A macOS virtual machine, on Apple's Virtualization framework, for measuring a real Sequoia from this Mac. It stays on this
// machine, and outside the repository: see README.md. On the host only; nothing in the OS or its build depends on this.

import AppKit
import Foundation
import Virtualization

let defaultDirectory = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("vulture-calibration-vm")
let neededFreeBytes: Int64 = 60 * 1024 * 1024 * 1024

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(("macos-vm: " + message + "\n").data(using: .utf8)!)
    exit(1)
}

func usage() -> Never {
    print("""
        usage: macos-vm check
               macos-vm install --ipsw FILE [--directory DIR] [--disk-gb N]
               macos-vm run [--directory DIR]
        """)
    exit(2)
}

struct Paths {
    let directory: URL
    var disk: URL { directory.appendingPathComponent("disk.img") }
    var auxiliary: URL { directory.appendingPathComponent("auxiliary.img") }
    var hardwareModel: URL { directory.appendingPathComponent("hardware-model") }
    var machineIdentifier: URL { directory.appendingPathComponent("machine-identifier") }
    var share: URL { directory.appendingPathComponent("share") }
}

func freeBytes(at url: URL) -> Int64 {
    var place = url
    while !FileManager.default.fileExists(atPath: place.path) && place.path != "/" {
        place = place.deletingLastPathComponent()
    }
    let values = try? place.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
    return values?.volumeAvailableCapacityForImportantUsage ?? 0
}

func gigabytes(_ bytes: Int64) -> String {
    String(format: "%.1f GB", Double(bytes) / 1_073_741_824)
}

func makeConfiguration(paths: Paths, hardwareModel: VZMacHardwareModel, machineIdentifier: VZMacMachineIdentifier) throws -> VZVirtualMachineConfiguration {
    let configuration = VZVirtualMachineConfiguration()
    let platform = VZMacPlatformConfiguration()
    platform.hardwareModel = hardwareModel
    platform.machineIdentifier = machineIdentifier
    platform.auxiliaryStorage = VZMacAuxiliaryStorage(contentsOf: paths.auxiliary)
    configuration.platform = platform
    configuration.bootLoader = VZMacOSBootLoader()
    configuration.cpuCount = max(2, min(ProcessInfo.processInfo.processorCount - 1, 6))
    configuration.memorySize = 8 * 1024 * 1024 * 1024

    // The same shape and density as the 2x display everything is compared at.
    let graphics = VZMacGraphicsDeviceConfiguration()
    graphics.displays = [VZMacGraphicsDisplayConfiguration(widthInPixels: 2560, heightInPixels: 1600, pixelsPerInch: 220)]
    configuration.graphicsDevices = [graphics]

    let disk = try VZDiskImageStorageDeviceAttachment(url: paths.disk, readOnly: false)
    configuration.storageDevices = [VZVirtioBlockDeviceConfiguration(attachment: disk)]

    let network = VZVirtioNetworkDeviceConfiguration()
    network.attachment = VZNATNetworkDeviceAttachment()
    configuration.networkDevices = [network]

    configuration.pointingDevices = [VZUSBScreenCoordinatePointingDeviceConfiguration()]
    configuration.keyboards = [VZUSBKeyboardConfiguration()]

    // Appears in the guest as "/Volumes/My Shared Files".
    try FileManager.default.createDirectory(at: paths.share.appendingPathComponent("calibration"), withIntermediateDirectories: true)
    let share = VZVirtioFileSystemDeviceConfiguration(tag: VZVirtioFileSystemDeviceConfiguration.macOSGuestAutomountTag)
    share.share = VZSingleDirectoryShare(directory: VZSharedDirectory(url: paths.share, readOnly: false))
    configuration.directorySharingDevices = [share]

    try configuration.validate()
    return configuration
}

func check() -> Never {
    print("This Mac: macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
    print("Virtualization supported: \(VZVirtualMachine.isSupported ? "yes" : "NO")")
    let free = freeBytes(at: defaultDirectory)
    print("Free where the virtual machine would go (\(defaultDirectory.path)): \(gigabytes(free))")
    print("Needed: about \(gigabytes(neededFreeBytes)): a restore image of about 15 GB, and an installed system.")
    if !VZVirtualMachine.isSupported {
        fail("this Mac can't run virtual machines")
    }
    if free < neededFreeBytes {
        fail("there isn't room: free up some space, or use --directory for another disk")
    }
    print("macos-vm: ready")
    exit(0)
}

func install(ipsw: URL, paths: Paths, diskGigabytes: Int) {
    if FileManager.default.fileExists(atPath: paths.disk.path) {
        fail("there's a virtual machine in \(paths.directory.path) already")
    }
    let free = freeBytes(at: paths.directory)
    if free < Int64(diskGigabytes / 2) * 1024 * 1024 * 1024 {
        fail("only \(gigabytes(free)) free at \(paths.directory.path): that's not enough")
    }
    VZMacOSRestoreImage.load(from: ipsw) { result in
        DispatchQueue.main.async {
            let image: VZMacOSRestoreImage
            switch result {
            case let .failure(error): fail("couldn't read \(ipsw.path): \(error)")
            case let .success(loaded): image = loaded
            }
            let major = image.operatingSystemVersion.majorVersion
            print("Restore image: macOS \(major).\(image.operatingSystemVersion.minorVersion), build \(image.buildVersion)")
            if major != 15 {
                fail("this is for measuring Sequoia, which is macOS 15: this restore image is macOS \(major)")
            }
            guard let requirements = image.mostFeaturefulSupportedConfiguration else {
                fail("this Mac can't run that restore image")
            }
            do {
                try FileManager.default.createDirectory(at: paths.directory, withIntermediateDirectories: true)
                try requirements.hardwareModel.dataRepresentation.write(to: paths.hardwareModel)
                let machineIdentifier = VZMacMachineIdentifier()
                try machineIdentifier.dataRepresentation.write(to: paths.machineIdentifier)
                _ = try VZMacAuxiliaryStorage(creatingStorageAt: paths.auxiliary, hardwareModel: requirements.hardwareModel, options: [])
                // A hole, for the most part: it takes up room only as it's used.
                FileManager.default.createFile(atPath: paths.disk.path, contents: nil)
                let handle = try FileHandle(forWritingTo: paths.disk)
                try handle.truncate(atOffset: UInt64(diskGigabytes) * 1024 * 1024 * 1024)
                try handle.close()

                let configuration = try makeConfiguration(paths: paths, hardwareModel: requirements.hardwareModel, machineIdentifier: machineIdentifier)
                let machine = VZVirtualMachine(configuration: configuration)
                let installer = VZMacOSInstaller(virtualMachine: machine, restoringFromImageAt: ipsw)
                print("Installing. This takes some minutes.")
                let observation = installer.progress.observe(\.fractionCompleted, options: [.new]) { _, change in
                    if let fraction = change.newValue {
                        print(String(format: "%.0f%%", fraction * 100))
                    }
                }
                installer.install { result in
                    observation.invalidate()
                    switch result {
                    case let .failure(error): fail("the installation failed: \(error)")
                    case .success:
                        print("Installed. Next: Meta/Calibration/macos-vm.sh run, and see README.md.")
                        exit(0)
                    }
                }
            } catch {
                fail("\(error)")
            }
        }
    }
    dispatchMain()
}

final class Delegate: NSObject, NSApplicationDelegate, VZVirtualMachineDelegate {
    var machine: VZVirtualMachine!
    var window: NSWindow!

    func applicationDidFinishLaunching(_ notification: Notification) {
        let view = VZVirtualMachineView()
        view.virtualMachine = machine
        view.capturesSystemKeys = true
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 800), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "macOS 15, for measuring"
        window.contentView = view
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        machine.delegate = self
        machine.start { result in
            if case let .failure(error) = result {
                fail("it wouldn't start: \(error)")
            }
        }
    }

    func guestDidStop(_ virtualMachine: VZVirtualMachine) {
        exit(0)
    }

    func virtualMachine(_ virtualMachine: VZVirtualMachine, didStopWithError error: Error) {
        fail("it stopped: \(error)")
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

func run(paths: Paths) -> Never {
    guard FileManager.default.fileExists(atPath: paths.disk.path) else {
        fail("there's no virtual machine in \(paths.directory.path): install one first")
    }
    do {
        guard let hardwareModel = VZMacHardwareModel(dataRepresentation: try Data(contentsOf: paths.hardwareModel)) else {
            fail("its hardware model file isn't readable")
        }
        guard let machineIdentifier = VZMacMachineIdentifier(dataRepresentation: try Data(contentsOf: paths.machineIdentifier)) else {
            fail("its machine identifier file isn't readable")
        }
        let configuration = try makeConfiguration(paths: paths, hardwareModel: hardwareModel, machineIdentifier: machineIdentifier)
        let application = NSApplication.shared
        application.setActivationPolicy(.regular)
        let delegate = Delegate()
        delegate.machine = VZVirtualMachine(configuration: configuration)
        application.delegate = delegate
        application.run()
    } catch {
        fail("\(error)")
    }
    exit(0)
}

var arguments = Array(CommandLine.arguments.dropFirst())
guard let command = arguments.first else {
    usage()
}
arguments.removeFirst()

var directory = defaultDirectory
var ipsw: URL?
var diskGigabytes = 80
while !arguments.isEmpty {
    let argument = arguments.removeFirst()
    guard !arguments.isEmpty else {
        usage()
    }
    let value = arguments.removeFirst()
    switch argument {
    case "--directory": directory = URL(fileURLWithPath: value)
    case "--ipsw": ipsw = URL(fileURLWithPath: value)
    case "--disk-gb": diskGigabytes = Int(value) ?? diskGigabytes
    default: usage()
    }
}

let paths = Paths(directory: directory)
switch command {
case "check":
    check()
case "install":
    guard let ipsw else {
        usage()
    }
    install(ipsw: ipsw, paths: paths, diskGigabytes: diskGigabytes)
case "run":
    run(paths: paths)
default:
    usage()
}
