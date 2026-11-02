# What things are called

vultureOS looks and behaves like a Mac, but its applications do not take Apple's product
names. Each is called what it plainly is, close to the name people already know, so nobody
has to learn a new word to find it.

| Name | What it is |
|---|---|
| **Files** | The file manager |
| **Dock** | The row of applications along the bottom of the screen |
| **Search** | Search, from anywhere |
| **Launcher** | Every application, in a grid |
| **Overview** | All windows and desktops at once |
| **Quick Settings** | The panel in the menu bar |
| **Notifications** | Banners, and the list of them |
| **Settings** | System settings |
| **Terminal** | The command line |
| **Text Editor** | Plain text |
| **Viewer** | Pictures |
| **Calculator** | Arithmetic |
| **Monitor** | Processes and what they use |
| **Console** | The system's log |
| **Disks** | Disks and volumes |
| **Archiver** | Archives: compress and expand |
| **Software** | What is installed, and what can be |
| **Installer** | Puts the system on a disk |

Words like Terminal, Calculator, Console and Dock are the ordinary names for these things
everywhere, and stay. An application's `KIND` (in its `CMakeLists.txt`) is what else Search
and Launcher find it by, and what Software shows under its name.

Services that show up in Monitor follow suit: `DesktopShell` (the menu bar, Search and
notification banners), `indexd` (Search's index), `notificationd`, `clipboardd`, `privacyd`
(what people have said applications may have).

## What is deliberately left alone

- **Programming interfaces and file layouts**: `/System/Library`, `.app` bundles, `dyld`,
  `os_log`, launchd's plist keys, and so on. These are how programs talk to the system;
  they are there to be compatible, not as a brand.
- **Ordinary Unix names**: `vlaunchd`, `logd`, `configd`, `notifyd`, `pbcopy`.
- **Plain words**: "Window Server", "login window", "menu bar", "Desktop".
- **Mentions of macOS in the documentation**, which say what the look is measured against.

## A note on the design

The names were changed; the look was not, on purpose. All the code and artwork here is
original (the fonts are Inter and JetBrains Mono, under the OFL), and nothing of Apple's is
in the repository: `Meta/Calibration` measures how AppKit draws and keeps only numbers.

This is an engineering note, not legal advice. Names are a question of trademark, not of
copyright or the DMCA; and how closely an interface may resemble another (so-called trade
dress) is a separate question again. Anyone distributing this widely should ask a lawyer.
