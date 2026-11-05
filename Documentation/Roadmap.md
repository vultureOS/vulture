# Roadmap

vultureOS is a Unix operating system that replicates macOS Sequoia, with every part of it our own code. The first
roadmap, [M0 to M14](Roadmap-1.md), built a real kernel, toolkit and desktop in about 155k lines. The result looks like
a Mac in a screenshot, then stops being one when you try to work. It has no browser, PDF, media or productivity apps,
its text is Latin-only, it has no full screen or tiling, and its filesystem stops at 28 GiB.

This roadmap goes from a Mac that looks right to a Mac you can live on.

See [Architecture](Architecture.md) for the design as it stands.

## Decisions

Made on 2026-11-03. They shape everything below.

| Question | Decision | What it means |
|---|---|---|
| Target look | **Sequoia only** | No Liquid Glass. Full measurement needs a macOS 15 VM or machine, as the development Mac runs 26.4 |
| Third-party software | **Optional packages** | The image stays 100% ours. Software can install ports (clang, git, vim), and self-hosting comes from an LLVM port |
| First focus after foundations | **Mac feel** | Phase 2 is windows, Dock, Finder and the measured look. Apps and the browser follow |
| Test machines | **QEMU only for now** | Every "done when" must be provable in QEMU. Drivers with no QEMU model wait in a gated track |

## Where we start

Checked on 2026-11-03, in the code and on the development Mac.

- **QEMU 10.2.1 has** `virtio-multitouch-pci` (its binary carries the multi-touch event fields, so scripted touches look
  possible), `hda-micro`, `usb-audio`, `usb-net`, `igb`, `virtio-rng` and `virtio-iommu`.
- **QEMU 10.2.1 lacks** `virtio-gpu-gl`, so hardware 3D can't be tested and rendering stays CPU/SIMD. It also lacks
  `usb-video`, and any model of Wi-Fi, Bluetooth, a trackpad, a battery, a lid or a backlight.
- **Missing from the code:** `dlopen`, `ptrace`, `AF_UNIX`, Empty Trash, xattrs, snapshots, clones, a wait cursor. The
  package repository list is empty. Shared anonymous `mmap` returns `ENOTSUP`. A process gets 64 open files. FAT is
  read-only.
- **Ours today, in lines:** Kernel 41k, Libraries 55k (LibUI 13k, LibGfx 2.4k, LibText 1.7k), WindowServer 4.6k,
  Applications 6.5k.
- **`reference/`, for scale:** Serenity LibWeb 274k, LibJS 122k, Kernel 152k, LibGfx 76k, LibGUI 47k, LibPDF 22k,
  WindowServer 16k, LibMedia 13k (VP8, VP9 and Matroska only), LibAudio 10k, HackStudio 10k, LibSQL 9k, LibRegex 9k,
  Profiler 4k; its browser stack is about 425k. Ladybird is about 1.75M first-party lines plus 41 dependencies, so it's
  a source of ideas and never a base.

## Three rulers

"Replicating macOS" stays vague unless it's measured, so the first milestone builds these and every later one is
judged by them.

1. **The Parity Ledger.** Each macOS feature is a row (absent, partial or present) with the test that proves it.
   `ninja parity` generates `Documentation/Parity.md` from real test results, so it can't go stale. ReactOS shows why:
   its hand-kept `apistatus.lst` last changed in 2017 and points at paths that no longer exist.
2. **The Mac is the oracle.** Wherever possible, the same check runs on a real Mac and on vultureOS and the answers are
   compared: POSIX behaviour, command output, CoreText shaping, PDF rendering, AppKit metrics. ReactOS's tests also run
   on real Windows, with `todo_ros` marking its gaps, and that's what keeps them honest.
3. **The Monday test.** One scripted walkthrough, using the real keyboard and pointer, grows each milestone towards an
   ordinary working day: log in, read mail, open a PDF, write a note, browse, play music, use the terminal. Finishing it
   unaided is the end goal.

**"Pixel-perfect" means** every value traces to a measurement, and every remaining difference is listed by name. Inter
is not SF, so text will never match exactly, and the ledger says so.

## Rules for every milestone

Carried over from the first roadmap:

- Our own code only. `reference/` is read-only and never copied.
- Each milestone ends in a bootable demo, and only counts as done when its "done when" passes.
- All lower-level suites rerun before a milestone counts as done.
- Original art only, and nothing of Apple's in the repository.
- Arch-specific code lives only in `Kernel/Arch/<arch>`, behind `Kernel/Arch/*.h` interfaces.

New:

- Each milestone adds its rows to the ledger, a step to the Monday test, and its Settings pane.
- From M23 on, every new control fills in its accessibility role and honours Reduce Motion, so M44 isn't a retrofit.
- Anything parsing untrusted files runs in a sandboxed process (the `ImageDecoder` idea from Serenity and Ladybird), using
  our existing ports and sandbox profiles.
- Toolkit first. Serenity's Terminal is 0.5k lines and its Mail 1.2k because the libraries carry the weight, so if an
  app is getting big, the missing piece probably belongs in a library.
- **A report from a real machine jumps the queue,** whatever milestone is under way.
- CI builds only, by choice, so `ninja check` runs locally before each push.

## Why this order

- **Programs before everything:** almost every later item needs M16's system calls.
- **Unicode before the filesystem:** Mac-style case-insensitive names need folding and normalization.
- **Compositor before the Mac feel:** all of Phase 2 animates, and animation has to survive a busy app.
- **The look last in Phase 2:** it measures the finished shell, not one still changing.
- **Debugger before the big engines:** PDF, codecs and the web shouldn't be built blind.
- **Keychain before Mail and Browser:** both store passwords.
- **Network before the web:** TLS 1.2, HTTP/2 and brotli are what real sites require.
- **Developer Mode arrives with ports:** today's code signing refuses anything clang would build.

## The milestones at a glance

Sizes are rough estimates of new lines, scaled from the reference figures above.

| Phase | # | Milestone | Status | When it's done, you can... | Est. size |
|---|---|---|---|---|---|
| **1 Foundations** | M15 | [Rulers](#m15-rulers) | Done, but for the VM (waiting on a decision) | See exactly how close to a Mac we are, as a generated scoreboard | 6k |
| | M16 | [System completeness](#m16-system-completeness) | Done | Run real programs: plugins, kqueue, Unix sockets, 10,000 open files | 20k |
| | M17 | [Unicode and locale](#m17-unicode-and-locale) | Done | Have text, dates and numbers handled correctly for any language | 15k |
| | M18 | [Shaping and input](#m18-shaping-and-input) | Done | Read and type any language, with emoji, in any control | 30k |
| | M19 | [vultureFS 2](#m19-vulturefs-2) | Done | Use a 1 TiB disk, duplicate files instantly, roll back a snapshot | 20k |
| | M20 | [External media](#m20-external-media) | Done | Plug in a USB stick; erase and repair disks; open disk images | 8k |
| | M21 | [Layers](#m21-layers) | Done | See animations stay smooth even when an app hangs, with the spinning cursor | 18k |
| | M22 | [Displays and gestures](#m22-displays-and-gestures) | | Use two displays, swipe between Spaces, record the screen | 8k |
| **2 Mac feel** | M23 | [Windows and Spaces](#m23-windows-and-spaces) | | Tile, go full screen, use App Exposé and Stage | 10k |
| | M24 | [The desktop](#m24-the-desktop) | | Use the full Dock, menu bar, Control Center, widgets and screenshots | 12k |
| | M25 | [Sessions](#m25-sessions) | | Set up a new machine, lock it, switch between users | 8k |
| | M26 | [Documents and Search](#m26-documents-and-search) | | Have autosave, default apps, Quick View, and fast Search that lasts across reboots | 20k |
| | M27 | [Files](#m27-files) | | Use a real Finder: columns, gallery, tabs, tags, Empty Trash | 12k |
| | M28 | [The look, measured](#m28-the-look-measured) | | Put it next to a Sequoia Mac with no estimated value left | 8k |
| **3 Your work** | M29 | [Debug and profile](#m29-debug-and-profile) | | Debug and profile on the machine itself; read crash reports and man pages | 15k |
| | M30 | [Documents and printing](#m30-documents-and-printing) | | Read and mark up PDFs, Save as PDF, open zip and modern image files | 60k |
| | M31 | [Everyday apps](#m31-everyday-apps) | | Use Notes, Calendar, Reminders, Contacts, Passwords and rich text | 25k |
| | M32 | [Network](#m32-network) | | Use IPv6, HTTP/2 and SSH | 20k |
| | M33 | [Mail and sharing](#m33-mail-and-sharing) | | Read your email, connect to a file server, print over the network | 20k |
| | M34 | [Sound](#m34-sound) | | Play your music library; record a voice memo | 15k |
| | M35 | [Video](#m35-video) | | Watch video files; record and trim | 60k |
| | M36 | [Photos](#m36-photos) | | Browse and edit a photo library | 5k |
| **4 The web** | M37 | [Engine: reading](#phase-4-the-web) | | Read Wikipedia in our own browser | 70k |
| | M38 | [Engine: modern layout](#phase-4-the-web) | | See modern pages laid out correctly, in a Safari-like Browser | 90k |
| | M39 | [JavaScript](#phase-4-the-web) | | Run real JavaScript, passing 90% of test262 | 120k |
| | M40 | [The live web](#phase-4-the-web) | | Use 50 real websites | 110k |
| **5 Developers** | M41 | [Ports and self-hosting](#m41-ports-and-self-hosting) | | Install clang and git; build vultureOS on vultureOS | 5k + ports |
| | M42 | [Studio](#m42-studio) | | Write, build and debug an app in our own Xcode-like IDE | 15k |
| **6 Trust and everyone** | M43 | [Trust](#m43-trust) | | Use disk encryption, Backups, Recovery and Software Update with rollback | 20k |
| | M44 | [Everyone](#m44-everyone) | | Use the whole system with no pointer, by speech, or in right-to-left languages | 15k |
| **7 Machines** | M45 | [Power and devices](#m45-power-and-devices) | | Sleep and wake in QEMU; hot-plug USB audio and network | 10k |
| | H1-H4 | [Gated hardware](#gated-hardware) | Waiting for a machine | Run on real computers, one kind at a time | |

**Total:** about 870k new lines, taking the system from 155k to about 1M. The web alone (about 390k) is more than twice
everything built so far, which is why it's four stages that each deliver something usable.

## Phase 1: Foundations

### M15 Rulers

**Delivers:**

- The Parity Ledger and `ninja parity`, with tests run on the host Mac and compared with vultureOS.
- A macOS 15 VM as a measuring instrument: a small host-only Swift tool in `Meta/Calibration` on Apple's Virtualization
  framework, with no third-party install. The VM stays outside the repository.
- A stronger test harness, because today's would miss too much: `imgdiff` allows 0.1% of pixels (4,096 at 2560×1600,
  enough to lose a tag dot or a checkmark) and scripts click raw coordinates. It gains strict masked regions,
  accessibility-tree assertions, `click "Label"`, per-step timeouts, steps for reboot, hot-plug, touch and display head,
  host-side fixtures, and booting the full set of system jobs, with one script per milestone.
- Boot and panic logs saved to a preallocated file on the EFI partition (written by sector, as FAT is read-only until
  M20), so a real machine with no serial port can still report what happened.
- The blank performance budgets measured, and documentation drift fixed (`POSIX.md`, `Architecture.md`,
  `DesignSystem.md`, the workflow headers).

**Done when:** `ninja parity` prints a scoreboard from real results; the 208 POSIX checks run on the host Mac and
vultureOS with each difference explained; a Sequoia AppKit test program runs in the VM and returns numbers; a
deliberately removed tag-sized dot fails a strict region; a panic leaves a readable log on the EFI partition; every
budget has a value.

**If there's no VM:** M28 falls back to SDK-linked measurements, and the ledger marks what stays estimated.

### M16 System completeness

**Delivers:**

- **Kernel:** Unix-domain sockets with descriptor passing; `kqueue`, `select`, `socketpair`, `shm_open`, shared anonymous
  `mmap`, `madvise`, `setitimer`, the `*at` family, `readv`/`writev`; a growable descriptor table; `mount`/`unmount`;
  `ptrace`; core dumps; a page cache that gives memory back; QoS boosts and priority inheritance.
- **Runtime:** `dlopen`/`dlsym`; `wchar.h`, `iconv`, `fenv`; pthread cancellation and process-shared primitives.
- **Tools:** `awk tar gzip zip unzip diff patch cmp dd file vi make top killall man`, plus macOS's own commands whose
  features exist by now: `open defaults launchctl plutil ditto sw_vers scutil system_profiler`, and read-only
  `networksetup`. Other commands ship with their feature: `xattr` in M19, `diskutil` and `hdiutil` in M20, `screencapture`
  in M22, `mdfind` and `mdls` in M26, `pmset` in M45.

**Done when:** POSIX checks grow from 208 to about 600, with Mac and vultureOS agreeing; `dlopen` loads a plugin; 10,000
descriptors open; the file cache shrinks under `test-compressor`; each command's output format matches a real Mac's once
machine-specific values are normalised.

**Note:** an evicting cache widens a hole `Architecture.md` already documents, as re-read sealed blocks go unchecked.
M19's hash tree closes it.

**Read:** Serenity `Kernel/Syscalls`, Ladybird `Libraries/LibCore`.

### M17 Unicode and locale

**Delivers:** LibUnicode with tables generated from the Unicode Character Database: normalization, case folding, UAX#29
segmentation, UAX#14 line breaking, UAX#9 bidi. CLDR-based dates, numbers and sorting. A real `iconv`. A Language &
Region pane (formats only; translating the interface is M44).

**Done when:** Unicode's published conformance files pass 100% in `test-host`; dates and numbers match the host Mac's for
a named set of locales.

### M18 Shaping and input

**Delivers:**

- **LibText 2:** our own shaping engine (Arabic, Hebrew, Indic, Thai, Hangul, CJK), font fallback, variable fonts, colour
  glyphs; attributed strings, paragraph layout and rich text in `TextView`. Noto families and Noto Color Emoji (OFL, as
  with Inter).
- **Input:** keyboard layouts as data, dead keys, press-and-hold accents, an input-method framework with Pinyin, Kana and
  Hangul, the Emoji & Symbols picker, spell check, autocorrect, text replacements, and a complete Keyboard pane.

**Done when:** glyph IDs and advances over a named corpus match CoreText on the host Mac, with exceptions listed; Arabic,
Hindi, Chinese and emoji show correctly in fields, tables, menus and title bars against goldens; Pinyin typed through the
real keyboard path gives the right characters.

**Note:** this replaces what HarfBuzz and ICU do for Ladybird, so there's no shortcut.

### M19 vultureFS 2

**Delivers:**

- **Format:** 64-bit limits (the 28 GiB cap goes); xattrs, ACLs, clones (`clonefile`), snapshots with rollback;
  case-insensitive and case-preserving by default, as on a Mac; transparent compression; file data checksums; room for
  encryption (switched on in M43); FSEvents; superblock feature flags.
- **Volumes:** separate System and Data volumes with a GPT role for each; firmlinks; apps moved to `/System/Applications`;
  a signed block-level hash tree replacing today's whole-file seal (the model of Apple's signed system volume).

**No upgrade from v1.** Names hash as raw bytes, extents have no checksums and the partition fills the disk, so all of it
would be rewritten anyway. v2 refuses a v1 disk with a clear message; only disposable test images exist today.

**Done when:** a volume fills past 28 GiB and passes `fsck`; 100 power cuts stay clean (`test-crash` and the simulated
disk); a flipped data bit is reported; a flipped sealed bit is caught even after eviction; NFC/NFD names and case
collisions behave as on the host Mac; cloning 1 GiB takes under 50 ms and shares its blocks; a snapshot rollback restores
exactly; FSEvents reports each change.

### M20 External media

**Delivers:** block-device hot-plug and removal; FAT writing, exFAT, ISO 9660; disk images, `hdiutil` and `diskutil`; a
filesystem recogniser (the idea of ReactOS's `fs_rec`); automount and eject. Disks becomes real: erase, partition, repair,
mount. Storage and Startup Disk panes.

**Done when:** a USB stick `device_add`ed in QEMU's monitor appears under `/Volumes` and in Files' sidebar, reads, writes
and ejects; pulling it out unannounced doesn't panic; volumes written here pass the host Mac's `fsck_msdos` and
`fsck_exfat`; Disks erases and repairs in a walkthrough.

### M21 Layers

A rewrite of the compositor, though not of how views draw: the views' `draw(Graphics&)` overrides stay. `Compositor.cpp`,
the Shell overlays, `ServerWindow`'s integer frame, `View::draw_tree` and `WindowServer.ipc` are what change.

**Delivers:** a retained layer tree in WindowServer (Core Animation's model) with transforms, opacity, masks, filters and
transactions; animations run in the server, so they survive a busy app; opt-in layer-backed views; scrolling on the
compositor for `ScrollView`, `TableView` and `TextView`; live window resize; the spinning wait cursor and "Not
Responding"; a Reduce Motion switch.

**Done when:** every golden still passes; input event to flush completion at p99 ≤ 16.7 ms and 60 fps at 2560×1600 with
full-screen damage, 12 windows and blur (QEMU has no photon to measure); a hung app's window still animates and shows the
wait cursor; the server's frame log shows a spring within 1 px of its model.

### M22 Displays and gestures

**Delivers:** two displays (the driver and `Screen` both assume one today) with arrangement, mirroring and a menu bar on
each; scaled "looks like" modes; a Displays pane with Night Shift. A gesture recognizer over `virtio-multitouch`; natural
scrolling; Mouse and Trackpad panes. A screenshot and recording interface with a Screen Recording prompt, enforced in
the service, closing the gap `Architecture.md` notes.

**Done when:** two-display goldens pass; a scripted multi-touch swipe changes Space; capture without consent is refused
and the attempt logged.

**Limit:** `virtio-multitouch` is an absolute touchscreen, and QEMU's window may not forward real touches, so this proves
the logic. How gestures feel gets tuned in H2.

**To confirm first:** `max_outputs=2` with our driver, and multi-touch injection through QMP.

## Phase 2: It feels like a Mac

### M23 Windows and Spaces

**Delivers:** Sequoia's tiling (drag to edges and corners, Option-drag, the Move & Resize menu, tiled windows resizing
together); full screen in its own Space and Split View; resize from every edge; the green button's menu; window tabs;
Cmd-Tab, Cmd-`, Hide, Hide Others. Overview complete: App Exposé, hot corners, dragging windows between Spaces, adding,
removing and reordering Spaces, Spaces per display. Stage (Stage Manager). Click the wallpaper to reveal the desktop.

**Done when:** a walkthrough asserts each behaviour through the accessibility tree and goldens; animation timing matches
numbers measured from the VM.

### M24 The desktop

**Delivers:**

- **Dock:** auto-hide, left and right positions, folder stacks, recent apps, context menus, drag in and out, badges.
- **Menu bar:** menu extras as separate clients (the idea of Serenity's `Applets`), Force Quit, About This Computer, Help
  search over menu commands (the idea of Serenity's `CommandPalette`).
- **Panels:** Control Center's modules with customization; Focus modes; notification grouping, actions and per-app
  settings; a widget framework on the desktop and in Notification Center; Launcher folders and reordering.
- **Screenshot:** Cmd-Shift-3, 4 and 5, with the floating thumbnail.

**Done when:** the walkthrough has a checked step for each of these, and a screenshot's file is compared with what was on
screen.

### M25 Sessions

**Delivers:** account creation, which doesn't exist today, and a writable Users & Groups pane; Login Items; lock screen,
screen saver, fast user switching; first-run Setup; dynamic wallpapers (original art); start-up logo and sound.

**Done when:** from a blank disk, a walkthrough goes through Setup, creates an account, logs in, locks, unlocks, adds a
second user and switches between them, with goldens.

### M26 Documents and Search

**Delivers:**

- **LibDatabase:** an embedded SQL store, which Search, Photos, Mail, Calendar and the Browser all need.
- **Document model:** a type registry with default apps and Open With; autosave and Revert To; windows reopening at
  login; Recent Items; the Services menu; full Open and Save panels with access grants across the sandbox (the idea of
  Serenity's `FileSystemAccessServer`).
- **Quick View** (spacebar) and thumbnails, generated in a sandboxed process with `dlopen`ed plugins.
- **Search 2:** a persistent metadata index driven by FSEvents, importers, `mdfind` and `mdls`, and providers for apps,
  files, calculations and settings (the idea of `Providers.h` in Serenity's Assistant).

**Done when:** Search answers within 50 ms per keystroke over 100,000 files and survives a reboot; an app killed mid-edit
loses a few seconds of work at most; a crashing Quick View plugin leaves Files running.

### M27 Files

**Delivers:** Column and Gallery views, tabs, path and status bars, preview pane, Get Info, colour tags, Put Back and
Empty Trash, copy progress and conflict dialogs, Compress and Expand, aliases, instant duplicates through clones, Quick
Actions, Smart Folders, Recents, spring-loaded folders, desktop icons and Stacks, volumes on the Desktop.

**Done when:** a walkthrough with accessibility assertions and strict regions covers each of these.

### M28 The look, measured

The milestone for "pixel-perfect".

**Delivers:** from the macOS 15 VM, numbers for what is still an estimate: window shadows, vibrancy and each material,
focus rings, inactive controls, menu rows, every desktop token, motion curves frame by frame. Every theme token tagged
with its source: measured, published, fitted or estimated. An expanded symbol set with weights, a full app icon set on
Sequoia's icon shape, and optical sizes and tracking tuned so that Inter sits as close to SF's metrics as it can.

**Done when:** the ledger reports zero estimated tokens, or names each exception and why; about 40 scenes compared with
the VM, with at least 99.9% of non-text pixels within 2 levels per channel, and text differences listed. Only numbers
enter the repository.

**Kept by choice:** the Catalina-style pointer, even though Sequoia's differs.

## Phase 3: Your work

### M29 Debug and profile

**Delivers:** a DWARF reader; a command-line debugger; `sample` and `leaks`; kernel performance events; Profiler
(Instruments' look, with time profile, flame graph, signposts, allocations); the "quit unexpectedly" dialog and
symbolicated crash reports in Console; man pages for every tool, and a Help app.

**Done when:** debugging a crashed program reaches the right line and shows its variables; Profiler finds a planted hot
function; a test checks that every tool has a man page.

**Read:** Serenity `Kernel/Tasks/PerformanceEventBuffer.cpp`, `DevTools/Profiler`, `LibDebug`.

### M30 Documents and printing

**Delivers:** GIF, WebP, TIFF, BMP, SVG; ICC colour management; xz, bzip2, zstd, brotli. LibPDF (reading, fonts, images,
encryption) and a PDF writer. A print dialog with preview, and Save as PDF everywhere. Viewer reaches Preview's level:
PDFs, thumbnails, markup, signatures, crop, adjust, export. Archiver handles zip and the rest. Quick View plugins for PDF
and images.

**Done when:** a named PDF corpus renders within tolerance of Quartz on the host Mac; a document saved as PDF reopens
correctly; zips made here open on a Mac, and the reverse; fuzzed parsers stay inside their sandbox.

**Read:** Serenity `LibPDF`, `LibGfx/ImageFormats`.

### M31 Everyday apps

**Delivers:** `securityd` and the Keychain with Passwords. Text Editor (rich text, RTF, Find, tables); Calculator (basic,
scientific, programmer, history); Notes; Reminders; Calendar with `.ics`; Contacts with vCard; Clock, Stickies, Dictionary
with Search's definitions provider, Fonts, Color Meter, System Information. Terminal gets tabs, splits and profiles;
Monitor gets CPU, Memory, Energy, Disk and Network tabs; Console gets logs that persist. The Share menu, with these apps
as its first targets, and widgets for each.

**Done when:** the Monday test writes a rich-text note, adds an event and a reminder, stores a password, shares a note,
and finds all of them through Search after a reboot.

### M32 Network

**Delivers:** IPv6 dual stack; fragments; TCP SACK, timestamps and CUBIC; TLS 1.2, a TLS server, session resumption;
HTTP/2 with HPACK, gzip and brotli; WebSocket; a cookie store with the public suffix list; mDNS and DNS-SD; NTP; a packet
filter; SSH client, server and SFTP. A writable Network pane, `networksetup` writes, Sharing and Date & Time panes.

**Done when:** `curl --http2` works against local and real servers; RFC vectors pass; `ssh` works both ways with the host
Mac; two QEMUs on a multicast network find each other by mDNS (`run.sh` adds that NIC).

### M33 Mail and sharing

**Delivers:** Mail (IMAP, SMTP, threads, search, attachments, the Share menu). Connect to Server: SFTP, then an SMB 2/3
client, which is how Macs share files. IPP network printers with discovery.

**Done when:** Mail reads and sends against a host-side test server; a print job reaches the host Mac's
`/usr/bin/ippeveprinter` and the PDF it received is checked; SMB mounts a share from the host Mac's File Sharing, turned
on for that test.

**Limit:** there's no OAuth until the Browser exists, so Mail starts with password login, and accounts that require OAuth
wait until M40.

### M34 Sound

**Delivers:** MP3, FLAC, ALAC, Vorbis, Opus, AIFF; resampling, an output picker, per-app volume; audio input with a
Microphone prompt. Music (library, playlists, gapless, Now Playing in Control Center, media keys), Voice Memos.

**Done when:** decoders match published test vectors in `test-host`; `test-sound`'s method confirms a gapless album through
the device.

**Limit:** QEMU's `wav` backend records output only, so only the microphone's plumbing is automatic. Its content is
checked by ear through CoreAudio.

**Decision needed before this starts:** H.264, AAC and HEIC are what most real files use, but carry patent questions that
VP9, AV1, Opus and FLAC mostly don't. This affects M34, M35 and M36. It's an engineering note, not legal advice.

### M35 Video

**Delivers:** LibMedia: MP4/MOV and WebM/MKV demux, VP9, then AV1. Player (trim, picture in picture, screen recording).
Quick View plugins for media.

**Done when:** decoders match published test vectors bit-exactly; 1080p30 VP9 plays without dropped frames under HVF; a
screen recording made here plays back here.

### M36 Photos

**Delivers:** Photos over LibDatabase: library, grid, albums, editing, video thumbnails.

**Done when:** 10,000 test pictures import and scroll at 60 fps; an edit survives a reboot and reverts to the original.

## Phase 4: The web

Our own engine in C++. It has no JIT, which also keeps W^X intact. WPT, `html5lib-tests` and test262 run from the host and
never enter the image.

**The key idea from Ladybird** (`Libraries/LibCompositing/DisplayList/DisplayListCommand.h`): paint records commands and a
separate step rasterizes them, so our LibGfx can stand where Skia stands there.

| # | Delivers | Done when |
|---|---|---|
| **M37 Engine: reading** | Spec-exact HTML parser, DOM, CSS parser, selectors and cascade with properties generated from data, UA stylesheet, block and inline layout, tables, display list onto LibGfx, images, links. A first Browser: tabs, address bar, bookmarks, history | `html5lib-tests` 100%; Wikipedia articles readable, with reference comparisons against Safari on the host Mac, JavaScript off |
| **M38 Engine: modern layout** | Flexbox, grid, floats, positioning, transforms, transitions, filters, custom properties, media queries, WOFF2 web fonts, SVG subset, forms in pranaUI's look. Multi-process: WebContent, RequestServer and ImageDecoder, each sandboxed. Safari's interface: compact tabs, sidebar, Reader, downloads, private windows, password autofill | WPT `css/` **reftests only** (the rest need M39's JavaScript) at a rate set in M37 from its first run; 20 saved real pages within tolerance of Safari with JavaScript off; a crashing tab leaves the rest alive |
| **M39 JavaScript** | LibJS: parser, bytecode interpreter, garbage collector, modern ECMAScript; LibRegex with Unicode; a usable `Intl` over M17's data. Also becomes the system's scripting language | test262 at 90% or better with no crashes; a published benchmark suite runs to completion |
| **M40 The live web** | IDL-generated bindings, DOM APIs, events, fetch, timers, History, cookies, CSP, canvas 2D, localStorage and IndexedDB over LibDatabase, workers, WebSocket, `<video>` and `<audio>` over LibMedia, a Wasm interpreter, per-site compatibility data (the idea of Ladybird's `WebCompat`), a basic inspector | WPT `dom/`, `html/`, `fetch/` at agreed rates; a committed script for each of 50 real sites reads and interacts correctly (run like `test-internet`, outside `check`); the Monday test browses |

**Read:** Ladybird `Documentation/LibWebFromLoadingToPainting.md`, `Meta/WPT.sh`, `Tests/LibWeb`.

## Phase 5: For developers

### M41 Ports and self-hosting

**Delivers:** a `Ports/` tree of build scripts with pinned checksums and no third-party source in the repository, installed
outside the image under `/opt`. libc++, LLVM/clang/lld, CMake, Ninja, git, vim, Python. The SDK under `/Library/Developer`.
Software gets its first real repository. **Developer Mode** with ad-hoc trust, because today's code signing refuses anything
clang builds.

**Done when:** vultureOS builds its own image on itself; that image, booted on the host as QEMU isn't ported, passes `ninja
check`; a clean image holds no port files; with Developer Mode off, a freshly built program is refused.

**Decision needed before this starts:** where the package repository is hosted, as that's public.

### M42 Studio

**Delivers:** an Xcode-like IDE: projects, a code editor with highlighting and completion through the ported `clangd`, build,
inline errors, debugger, Git, and a live preview of pranaUI's builder layer (the ideas of Serenity's HackStudio and
GMLPlayground).

**Done when:** a new app is created, built, debugged and run inside Studio, in one walkthrough.

## Phase 6: Trust and everyone

### M43 Trust

**Delivers:** Disk Encryption (FileVault's model, unlocked at login); per-program signatures and a first-launch check
(Gatekeeper's model); the rest of the privacy list (Contacts, Calendar, Full Disk Access, Accessibility), enforced in
services; per-app containers; UEFI Secure Boot with our own keys and a verified kernel. Backups (Time Machine's model):
snapshots to an external disk with a browse-through-time view. Recovery: a separate boot environment to restore, reinstall
or repair. Software Update: signed updates that swap the sealed System whole and roll back after a failed boot.

**Done when:** a stolen-disk test finds no readable data; unsigned code is refused and tampered code caught; a tampered
loader fails Secure Boot on x86_64 (AArch64's firmware here has no secure build); a failed update rolls back by itself; a
machine wiped by script restores fully from Backups.

**Limit:** Camera's prompt waits for a machine, as QEMU has no `usb-video`.

### M44 Everyone

**Delivers:** Screen Reader (VoiceOver's model), driving the existing accessibility tree, with our own speech synthesizer
and `say`; Zoom; Reduce Transparency, Increase Contrast, colour filters; Sticky Keys, Slow Keys, Mouse Keys, Full Keyboard
Access. Localization: string catalogs, a right-to-left mirrored interface, a first set of languages.

**Done when:** the Monday test finishes keyboard only, and again driven only by what the Screen Reader says; RTL goldens
pass; a test checks every control's role.

## Phase 7: Machines

### M45 Power and devices

QEMU can prove all of this.

**Delivers:** sleep and wake, with power states walked through every driver (the idea of ReactOS's `ntoskrnl/po/power.c`);
`pmset`; a plug-and-play manager generalising M20's hot-plug; USB audio and network classes; MSI on AArch64; `igb`. A shared
core with thin hardware drivers (the idea of ReactOS's port and miniport split).

**Done when:** 100 sleep and wake cycles in QEMU, on x86_64 and on AArch64 if QEMU's PSCI allows, with windows and network
intact; USB audio hot-plugged and playing.

### Gated hardware

These start only when a machine is at hand. QEMU can't test them, and a driver written blind can harm a real machine, so
they aren't scheduled.

| # | Needs | Delivers |
|---|---|---|
| **H1** | Any real PC | Bring-up from that machine's reports; native-resolution mode setting for its GPU; its network chip |
| **H2** | A laptop | I2C trackpad and real gesture tuning; battery, lid, backlight, its sleep state, thermal and CPU frequency control |
| **H3** | A supported card | Wi-Fi with user-supplied firmware (as with the Pi), WPA2/3; Bluetooth HID; camera |
| **H4** | A Raspberry Pi | PCIe, USB, network and sound |

## Not on this roadmap

| Item | Why |
|---|---|
| Liquid Glass | Decided: Sequoia only |
| iCloud, Handoff, AirDrop to Apple devices, iMessage, FaceTime, App Store, Universal Control, Sidecar, AirPlay, iPhone Mirroring, Find My, Home | Need Apple's servers or devices |
| Running real Mac apps | Needs a Mach-O loader and a reimplementation of Cocoa. ReactOS shows the cost: decades, importing Wine, still incomplete |
| Apple Intelligence, Siri, Live Text, Dictation, Voice Control | Need an ML runtime and models we don't have |
| Metal, WebGL, 3D games | QEMU has no `virtio-gpu-gl`, so nothing could prove it works |
| Maps, Weather, News, Stocks, Books, Podcasts, TV | Depend on online services or stores; revisit once the Browser exists |
| Pages, Numbers, Keynote, Freeform | Separate products, or not essential to the system |
| Shortcuts, Automator, AppleScript | Valuable, but best built on LibJS; revisit after M39 |
| Photo Booth, Image Capture | Need a camera or scanner; revisit after H3 |
| Chess, Grapher | Small and pleasant, with nothing depending on them; add any time |
| WebRTC, DRM, HTTP/3 | Large, and not needed to reach M40's goal |

## New names

Plain and close to what people expect, as [What things are called](Naming.md) sets out; that page is updated as each one
ships.

| Name | What it stands in for |
|---|---|
| **Browser** | Safari |
| **Player** | QuickTime Player |
| **Stage** | Stage Manager |
| **Quick View** | Quick Look |
| **Passwords** | Keychain's app |
| **Fonts** | Font Book |
| **Backups** | Time Machine |
| **Disk Encryption** | FileVault |
| **Screen Reader** | VoiceOver |
| **Setup** | Setup Assistant |
| **Studio** | Xcode |
| **Profiler** | Instruments |

Unchanged because they're plain words: Mail, Notes, Reminders, Calendar, Contacts, Clock, Stickies, Dictionary, Music,
Photos, Voice Memos, Recovery, Screenshot, Color Meter, System Information.

## Performance budgets

Measured on QEMU + HVF with 8 vCPUs, tracked by `ninja bench` from M3 on. M15 measures the blanks.

| Metric | Target | Measured |
|---|---|---|
| Boot, firmware handoff to login window | ≤ 2 s | 0.22 s (M19, with the fonts for every language) |
| Null syscall | < 150 ns | 72 ns (M19) |
| Context switch | < 1 µs | 173 ns (M3, yield between two threads on one processor) |
| IPC round trip | < 3 µs | 0.84 µs (M7, call and answer between two processes; 4.6 µs through a generated proxy to a vlaunchd-started service) |
| Window drag at 2560×1600 | 60 fps | 111 fps (M21: 10 windows, 5 with blur; about 4 ms of composing and 0.7 ms of flushing per frame; 120 fps is the most it asks for). With all of the screen drawn for every frame, 12 windows and blur: 119 fps |
| Input to photon | ≤ 1 frame | 12.5 ms at the 99th percentile from the event to the flush, worst 14.2 ms; 9.5 ms with all of the screen drawn for every frame (M21, on a machine with little else going on: about 20 ms with a game running on it). It was 23.5 ms at M19, measured from when the event was read rather than when it happened. QEMU shows no photon, so this is as far as it can be measured |
| Text Editor cold launch | < 300 ms | 36 ms, 25 ms warm (M21) |
| Cmd-Tab appears | ≤ 1 frame | |
| Search, per keystroke | ≤ 50 ms | |
| Application launch | < 300 ms | 24 ms (M19, Calculator, cold) |
| Clone of a 1 GiB file | < 50 ms | Under 1 ms (M19): it takes the same for any size |
| 1080p30 VP9 | no dropped frames | |
| A PDF page | < 100 ms | |
| Idle memory | tracked per release | |

## Risks

| Risk | Mitigation |
|---|---|
| Scope: about 870k lines, more than five times what exists | Strict order; every milestone ends in something usable; the ledger shows real progress |
| The web takes longest and could swallow everything | It stays behind the Mac feel, as decided, and each of its four stages stands alone |
| No macOS 15 VM | M15 finds out early; M28 then falls back, and the ledger names what stays estimated |
| M21's compositor rewrite breaks the desktop | Existing goldens must all still pass; layers are opt-in, so views move across gradually |
| Case-insensitive names break assumptions in our programs and tests | M19's Mac-oracle tests; the default can still be changed per volume |
| Everything only ever runs in QEMU | M15's EFI log makes real-machine reports possible, and those reports jump the queue |

## Decisions and permissions still needed

| When | What |
|---|---|
| M15 | Permission to download a macOS 15 restore image (about 15 GB) for the VM. macOS's licence appears to allow VMs on Mac hardware for development and testing, but that's worth checking |
| M18 and M31 | Approval of the Noto fonts (OFL) and of word list and dictionary data, with licences shipped as with Inter. Such data sits outside the "no external code" rule, as fonts do |
| M33 | Turning on File Sharing on the host Mac for the SMB test |
| Before M34 | H.264, AAC and HEIC: whether to implement them, given the patent questions |
| Before M41 | Where the public package repository lives |
| Any time | When a real machine appears, H1 starts |

## Findings

Added as each milestone finishes. The first roadmap's are in [Roadmap 1](Roadmap-1.md#findings-so-far).

### M15 and M16

- **A serial console that stopped.** `run.sh`'s chats stuck now and then at the last character of a line with no newline. The PL011 driver drained its FIFO and then cleared the interrupt, so what arrived in between was lost until the next character. It clears first now, and twenty runs in a row get through.
- **A tickless kernel has no sample to average.** A timer every five seconds for the load average broke what `timer.fires_in_deadline_order_and_never_early` asserts: that nothing's queued when nothing's due. The average's worked out when it's asked for instead, from how long the idle threads have run.
- **The sanitizers earn their keep.** UBSan found that a core dump wrote a structure at four-byte alignment, as ELF notes are. Nothing went wrong on this processor; the same code on another could have faulted.
- **Testing inside a copy of the system.** With a `dd` of its own at last, a test that changes to a copy of the system and runs `dd` ran that one, for the wrong processor, if `PATH` had a relative entry in it. The seal tests then found nothing to catch, and said so as if the kernel had let a tampered file by.
- **Tests that click by position go stale.** One new row in Settings moved everything under it, and each walkthrough that clicked one of those by coordinates went wrong, as M15 said it would. They go by name now.
- **What a Mac does that's wrong.** Nine of POSIX's checks and seven of the commands are different on purpose. Each is written down in `Tests/Parity`, with why.

### M21

- **A race that had been there since M8.** The window server crashed or hung in most runs of a new test, and in nothing that came before it. A compositor thread that had just gone past the end of one job could read the next job's larger count, and take the index it held for a piece of that. Someone else had the same piece, so it ran twice, and the count of those done went past the end and never came level. It took a frame with many jobs of different sizes to show it: making frames faster made it happen. The count and the next index now share a word.
- **Most of a frame was waiting.** Twelve windows with blur took 9 ms of composing, of which only about 1.3 ms would be needed if all eight processors were kept busy. Each blur got its memory afresh every frame, and at that size it's a mapping of its own: page faults, and all the other processors being told to forget it, which under a hypervisor is slow. Each blur stopped everything until what's under it was done, everywhere and not just where it reads. And one thread did each of them. Keeping the memory, taking only what each one reads, and sharing the slow parts took the same frame to under 5 ms.
- **Nothing was ever covered.** Every window was drawn in full, under whatever was over it, and every blur was worked out, whether it could be seen or not. What's opaque of a window is now worked out as its pixels arrive, a square at a time, so nothing's drawn under that. A blur still needs what's under it to be there to read, even under something else: which is why that has to be worked out from the top down.
- **The clock started late.** Input to flush was timed from when the server got round to reading the event, which could be a whole frame after it happened. So it flattered itself by up to a frame. It's timed from the event now. The number got worse, and then much better.
- **A flush costs for being a flush.** All of the screen took 0.7 ms, and part of it 3.1 ms. QEMU copies whole rows in one go, and anything narrower a row at a time, starting from the first page of the framebuffer for each. So a flush takes whole rows, and rarely more than one of them is needed.
- **Being held still isn't having motion reduced.** They were one setting. For a golden screenshot, a scroller that's been shown stays shown, so nothing depends on when it's looked at. With Reduce Motion it would have stayed up for good. Testing a real scroll in a test with motion off is what showed it.
- **The first test of a fixed pause.** With a game running on the same Mac, a walkthrough quit a program, waited its one second, and clicked on the Dock as the Dock was moving along to fill the gap. It waits for the program to be gone now.
- **Numbers depend on the machine.** F1 22 under CrossOver was running on this Mac for some of these runs, taking 3.5 of its 10 cores. Composing went from under 4 ms to 7.5 ms, and input to flush at the 99th percentile from about 10 ms to 20 ms, as one stalled virtual processor holds up a frame. The budget's for a machine doing nothing else; on one doing a lot else, it's over.
- **Limit: Genie and Overview aren't layers yet.** They already run in the server, so they don't care about a stuck program, and M23 rebuilds Overview anyway.
- **Limit: a stuck program doesn't scroll.** The server moves what it's told. What's kept ready is for not having to draw, not for going on without the program.
- **Limit: there's no refresh to keep to.** Frames someone's waiting for go at up to twice the rate, as QEMU has no display to wait for. A real one will want to be tied to its refresh, which is H1's.
- **Limit: resizing makes a new surface for each size.** It's fast enough, at about 50 fps for a big window, as most of the cost is drawing it anyway. Keeping a bit more than it needs would make it faster.
- **Limit: Reduce Motion changes things at once,** where a Mac fades them.

### M20

- **A real Mac's fsck is a better judge than ours.** Ours and the volume both thought all was well, and `fsck_msdos` found 67 clusters in use by nothing: files that had been removed and never let go of, which is the test's doing, and a fair thing for a checker to complain about. It's now what every volume written here has to get past, in both directions.
- **A hash table moves things.** The first host run with the sanitizers on found a volume keeping references to nodes in a table that had since grown. They're on the heap now, where they stay.
- **A reference that outlived what it was of.** Unmounting looked up the root, then let go of the file system, then of the root: whose last moments wanted the volume that had just gone. A `VERIFY` said so, on the first stick to come out.
- **Ejected isn't taken out.** A stick that's been ejected is still in its port, and when it does come out there has to be something left to tell. The device keeps a reference to its disk for as long as it lasts.
- **Nothing used to change the list of disks.** `BlockDevice::all()` handed out the list itself, which was fine when it was only ever added to, before anything ran. Now it's a copy, under a lock.
- **A walkthrough passed with a sheet nobody could see.** Erase's sheet asked for a size from something that had no opinion on one, and came out with no height. It wasn't drawn, yet the accessibility tree listed its buttons and a click on them still landed, so every step passed. It only showed in the screenshot. What has no room is now gone from the tree and from hit testing too, as it already was from the screen, and a test says so.
- **A stale message about a name.** After partitioning, the first new volume was forced off and mounted again. The daemon did the work and only then read that `disk2s1` had gone, which by then was true of the old one and not of the new one with its name. Anything writing to it just then would have lost data. A mount's record now says whether what it's on has gone, so a name alone can't take a volume away, and the walkthrough forbids that line.
- **Look at symbols at the size they're seen.** First Aid's cross came out as a pair of dice, from two overlapping arms under an even-odd fill. Refresh looked like a "d". Both had passed by being there; a crop of the toolbar is what showed them.
- **A frame rate that depends on the machine.** The drag test fell to 56 fps during a full `check`, and made 59.7 alone. Each frame only takes about 10 ms of work: it's the fixed beat that can't catch up, which is what M21 replaces.
- **Limit:** disk images are plain ones, so a Mac's compressed `.dmg` isn't read yet, and a disc is ISO 9660 with Joliet or Rock Ridge, with no UDF.
- **Limit:** only disks come and go. The general kind of hot-plugging is M45.
- **Limit:** a volume forced out while in use stays in memory until the machine restarts, for whatever's still holding its files to be told.

### M19

- **Two threads and one bounce buffer.** "The manifest isn't signed", sometimes, on x86_64 under software emulation. A virtio request's lock was dropped when the request ended, before the data was copied out of the one bounce buffer, so the next request could overwrite it first. A signature check is a fine way to find a race, as it can't be argued with.
- **A signature comes before believing anything.** A flipped byte in the image's size went past the sanity check, was called "no system here", and the machine quietly started from the copy in memory. The first thing checked now is the signature on the block that says how big it is.
- **`execvp` didn't look at `PATH`.** It went through a fixed list, and nobody noticed for as long as everything was in it. `/usr/sbin/purge` was the first thing that wasn't.
- **A Mac keeps what it replaced's spelling.** Renaming `x` to `Y` with `y` already there leaves a file called `y`. There's no reason to guess that: it came from asking.
- **Letting go of memory lost its place.** The file cache gives pages back in turn, from wherever it got to last time, and that moved while it went, so most files were skipped: a pass over the cache gave back a little of it. `purge`, which is what a test of a block changing after it left the cache needs, showed that up. Nothing had noticed under pressure, as it went round again soon enough.
- **A log with bytes in it stopped being a log.** `run.sh` shows what the console said through `sed`, which on a Mac stops at the first byte that isn't UTF-8, and took everything after it with it. And `sysctl` printed a number as text whenever its top byte was zero, which is what put those bytes there.
- **Some programs are copies.** Twenty tools share a binary, so flipping a bit in "`logname`" flipped it in whichever of them was first on the disk, and `logname` carried on working. The hash tree was right, and the test wasn't.
- **Blocks that are hashed cost something at boot.** Each block read off the System volume is hashed, with no help from the processor yet. Reading what's needed to start takes a few more milliseconds; it hasn't troubled any budget so far.
- **Limit: the hash list is in memory, and it's flat.** A signature, a hash of the list, and a hash of each block: two levels, where a real tree would have more, read as it's needed. For an image of 83 MiB, it's 650 KiB, which is fine for now. It should become a tree if the image grows by much.
- **Limit: the Open and Save panels show all of `/`.** On a Mac, most of what's at the top is hidden, by a flag it has now: M27 is where that's looked at.

### M18

- **A window that doesn't look at the hot spot.** "The cursor's not accurate", from someone using it: they had to aim off to the side to hit the traffic lights or a window's edge. Everything here was right, by every test, as all of them run with no window. QEMU's window on a Mac places the cursor's picture by its corner and never reads its hot spot, so the arrow's tip was 6 points from where it clicked, and the resize arrows 16. `run.sh` says so now, with the boot word `cursortopleft`, and the driver tells it the corner instead.
- **A group doesn't end with its leader.** Now and then a shell printed its prompt before a pipeline's output. A group whose first member had finished, but hadn't been waited for, counted as gone, so a fast `echo` left the next command out of its group and the shell stopped waiting for it. It took a slow enough machine and a busy enough one to see, and `test-keyboard` found it. It's four more checks that a real Mac agrees with.
- **A dictionary knows less than it seems to.** Words are compared with its entries without regard to case, so "iss" passes because "ISS" is in there. Proper names and acronyms deserve better, and that's still to do.
- **Core Text agrees, for the most part by being asked.** 135 lines in ten scripts come out the same, glyph for glyph and position for position. The last two differences were a contextual feature that Core Text leaves off outside the scripts that need it, and a consonant with a pre-base form taken for the base.
- **A font for Chinese has more than one set of subroutines.** With only the first, a CID-keyed font's outlines were recognisably characters, some of the time. `世界` looked right and `你好` didn't.
- **A font of pictures has no outlines**, and the parser refused it for that. And bitmaps go through a painter's transform where masks don't, so emoji were twice the size and somewhere else.
- **Joiners are skipped, except when they're what's being looked for.** A family of emoji is a ligature with joiners in it.
- **The fonts and the dictionary aren't in the repository.** `Meta/fetch-fonts.sh` fetches Noto and a word list, pinned and checked, and an image built without them still works, with boxes where those languages would be. Whether they go in is a decision for later.

### M17

- **A truncated download looks like a bug in the algorithm.** `BidiCharacterTest` passed four times in ten, and everything said the rules were wrong. `DerivedBidiClass.txt` had been cut short when the disk filled up: a size that's an exact multiple of 4096 was the clue. Everything else downloaded has been checked for the same since.
- **A Mac has three sets of locale data.** Foundation's matches CLDR but for a short list, which is in `Meta/generate-locale.py`. Its C library's, in `/usr/share/locale`, is from old BSD tables, and disagrees with its own Foundation about things as basic as "Sept". This system has one set, and 78 differences from those old tables are listed in `Tests/Parity/locale-file-differences.json`.
- **This Mac isn't Sequoia.** What's measured here is macOS 26.4. Locale data does change between releases, so some of what's in that list may be different on Sequoia: without the VM, that can't be known.
