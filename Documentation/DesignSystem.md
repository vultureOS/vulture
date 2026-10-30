# Design System

vultureOS follows the macOS Sonoma/Sequoia look: translucent ("vibrant")
menu bar, Dock and sidebars, rounded windows with soft shadows, traffic-light
window controls, and a global menu bar. It reproduces metrics and behaviors,
not Apple's assets: every icon, wallpaper, symbol and sound is original, and
the system font is Inter (SIL OFL).

## How "pixel-perfect" is achieved

1. **Measure real macOS.** Capture controls, title bars, traffic lights, the
   menu bar, Dock, sidebars, menus, sheets and shadows at 2x, in light and dark
   appearance, using Screenshot and Digital Color Meter. Record sizes, radii,
   colors, blur, shadow and animation timing.
2. **Encode the measurements as tokens** in
   `Base/System/Library/Themes/{Light,Dark}.json`: colors, materials (blur
   radius, saturation, tint), metrics, corner radii, shadows, type scale and
   animation curves. pranaUI and PranaWindowServer read only tokens; no visual
   value is hard-coded in code.
3. **Compare and lock.** The Widget Gallery app renders every control at 1x
   and 2x, in light and dark. `Tools/imgdiff` overlays our render on the
   measured reference for review. Approved renders become golden images, and
   `ninja test-ui` fails on any regression (max channel difference ≤ 2, ≤ 0.1%
   of pixels).

## Starting tokens

All values are approximate (≈) and get calibrated in step 1. Units are points;
2x displays double them.

| Token | Value |
|---|---|
| Menu bar height | ≈ 24 pt, translucent material |
| Title bar / unified toolbar | ≈ 28 pt / ≈ 52 pt |
| Window corner radius | ≈ 10 pt |
| Traffic lights | ⌀ ≈ 12 pt, ≈ 8 pt gaps; red/yellow/green; glyphs on hover; grey when the window is inactive |
| Type (Inter) | body 13 pt, small 11, mini 9, title 13 semibold, large title 26 |
| Accent | systemBlue `#007AFF` (light) / `#0A84FF` (dark); user-selectable accent |
| Push button | ≈ 22 pt tall, ≈ 6 pt radius; the default button uses the accent fill |
| Dock | 48–64 pt icons, magnifying to 128; rounded translucent shelf; running-app dots |
| Motion | standard ≈ 0.25 s ease-in-out; springs for the Dock, Mission Control and sheets; genie minimize |
| Rendering | 2x HiDPI by default (2560×1600 px = 1280×800 pt), with a 1x fallback |

## Materials

Vibrancy is a backdrop blur plus a saturation boost and a tint, composited by
PranaWindowServer. Blur is recomputed only for damaged backdrop regions, so
translucent surfaces stay cheap when nothing behind them moves.

| Material | Used by |
|---|---|
| Menu bar | menu bar |
| Popover / menu | menus, popovers, Spotlight |
| Sidebar | Finder and Settings sidebars |
| HUD | volume and brightness overlays |
| Under-window | Dock shelf, Notification Center |

## Assets

- **Fonts:** Inter for UI text and JetBrains Mono for Terminal, both SIL OFL
  with their licenses in `Base/System/Library/Fonts/`. Rendered by our own
  LibText.
- **Icons:** original artwork, authored as vectors and compiled by a host tool
  into multi-resolution `.vicns` files.
- **Symbols:** an original set of vector UI glyphs drawn through LibGfx paths.
- **Wallpapers and sounds:** original. Wallpapers can be generated with LibGfx
  as a host tool.
