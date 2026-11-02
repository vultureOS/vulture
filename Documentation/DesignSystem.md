# Design System

vultureOS follows the macOS Sonoma/Sequoia look: translucent ("vibrant")
menu bar, Dock and sidebars, rounded windows with soft shadows, traffic-light
window controls, and a global menu bar. It reproduces metrics and behaviors,
not Apple's assets: every icon, wallpaper, symbol and sound is original, and
the system font is Inter (SIL OFL).

## How "pixel-perfect" is achieved

1. **Measure AppKit.** `Meta/Calibration/measure.sh` links a program against a
   macOS 15 SDK, which is what selects Sequoia's look on any newer macOS, has
   AppKit draw every control off screen in light and dark at 1x and 2x, and prints
   numbers: frames, alignment insets, intrinsic sizes, the box of what shows, rows
   of pixels through the middle, and system colors and metrics. It runs only on a
   Mac, only when calibrating. Nothing in the OS or its build depends on it, and no
   picture of Apple's goes in the repository.
2. **Encode the measurements as tokens** in
   `Base/System/Library/Themes/{Light,Dark}.json` (colors, and each control's face
   as a fill with rings of shadow outside and shading inside) and `Metrics.json`
   (sizes, which do not change with the appearance). pranaUI and PranaWindowServer
   read only tokens; no visual value is hard-coded in code. A token that is missing
   draws magenta and is counted, and the tests hold the count at zero.
3. **Compare.** `Meta/Calibration/compare.py` sets AppKit's rows and boxes against
   those of `ui-catalog`, which draws the same controls set up the same way.
   `Tests/UI/Measured/controls.json` keeps the sizes and boxes, and `TestMetrics` holds
   every control's visible box within a pixel of AppKit's on every edge, in light and
   dark, at 1x and 2x (300 looks).
4. **Lock.** The Widget Gallery shows every control on the real window server. Its
   screenshots, approved by eye, are golden images: `ninja test-ui` fails on any
   regression (max channel difference 2, at most 0.1% of pixels).

## Where this stands

Controls are measured, and held to AppKit by a test. Be clear about what that does
and does not mean:

- **It is AppKit linked against SDK 15.2, running on macOS 26.4, not a Sequoia
  install.** SDK-linked behavior gives Sequoia's look for what AppKit gates on the
  SDK: window corners (12 pt, 16 on 26), title bars (28 pt, 32 on 26), buttons and
  the controls drawn with them. What it does not gate is today's macOS: menu rows
  report 24 pt under both SDKs, so they say nothing of Sequoia, and the 22 pt here
  is from memory, not measured. The same goes for anything else not gated.
- **Text differs.** Our font is Inter, not SF, so widths differ. Padding is what is
  carried over; a label's ends are not compared. Bezel labels use Inter Medium, which
  matched AppKit's weight by eye; SF's letterforms are not reproduced.
- **Not measured:** window shadows and vibrancy (AppKit's frame view renders opaque
  off screen), materials in general, focus rings, the inactive-window look of controls,
  anything in motion. These are estimates, and goldens only say they have not changed.
- **Bezel shading is a fit.** Rings of alpha reproduce AppKit's rows at the edges to a
  few levels out of 255, not exactly. At 2x they come close; at 1x they follow from
  the same numbers. Control symbols (chevrons, checkmarks) are original shapes
  drawn to match in size and weight.
- **System colors** are the published values for macOS, not measured here (they are
  not gated either).

So it reads as Sequoia and lines up to the pixel on the metrics that are tested,
but it has not been compared with a real Sequoia install. Doing so, with the same tool,
is what remains.

## Tokens, as measured

Units are points; 2x displays double them.

| Token | Value |
|---|---|
| Title bar / toolbar | 28 pt / 44 expanded, 52 unified, 38 compact |
| Window corner radius | 12 pt |
| Traffic lights | ⌀ 12 pt, 8 pt gaps, 8 pt inset, centered at y = 14 |
| Type (Inter) | body 13, small 11, mini 9, large title 26, title1 22, title2 17, title3 15 |
| Accent | systemBlue `#007AFF` (user-selectable: what uses it follows its hue) |
| Push button | 20 pt tall (large 28, small 16, mini 13), radius 5 (6, 4, 3), padding 10.35 |
| Checkbox, radio | 14 pt (16, 12, 10) |
| Switch | 38 by 22 (32 by 18, 26 by 15) |
| Segmented | 22 pt (30, 18, 15) |
| Pop-up | 20 pt (28, 16, 13) |
| Field | 21 pt, rounded 22, search 22 (large 30) |
| Table | row 24, header 28, indent 13 |
| Menu | row 22 (unmeasured: see above), separator 11, padding 5 |
| Alert | 260 pt wide |
| Motion | standard 0.25 s ease-in-out; window resize 0.35 s; springs for switches and sheets |
| Rendering | 2x HiDPI by default (2560×1600 px = 1280×800 pt), with a 1x fallback |

The desktop (menu bar, Dock, Search, notifications, Quick Settings, Launcher, Mission
Control) has tokens of its own, in the same files. **None of those are measured:** AppKit
does not draw them off screen for a program, so their sizes and colors are estimates
from memory of Sequoia, to be compared with a real one.

## Materials

Vibrancy is a backdrop blur plus a saturation boost and a tint, composited by
PranaWindowServer. Blur is recomputed only for damaged backdrop regions, so
translucent surfaces stay cheap when nothing behind them moves.

| Material | Used by |
|---|---|
| Menu bar | menu bar |
| Popover / menu | menus, popovers, Search |
| Sidebar | Files and Settings sidebars |
| HUD | volume and brightness overlays |
| Under-window | Dock shelf, Notifications |

## Assets

- **Fonts:** Inter for UI text and JetBrains Mono for Terminal, both SIL OFL
  with their licenses in `Base/System/Library/Fonts/`. Rendered by our own
  LibText.
- **Icons:** original artwork, authored as vectors and compiled by a host tool
  into multi-resolution `.vicns` files.
- **Symbols:** an original set of vector UI glyphs drawn through LibGfx paths.
- **Wallpapers and sounds:** original. Wallpapers can be generated with LibGfx
  as a host tool.
