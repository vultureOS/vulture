/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// What macOS really looks like, in numbers: draws AppKit's controls off-screen,
// in light and dark, at 1x and 2x, and writes down their sizes, colors and pixel
// profiles. Nothing appears on the screen and nothing of the screen is read.
//
// AppKit picks its look by the SDK a program was linked against, so linked against
// the macOS 15 SDK this measures Sequoia's look on any newer macOS.
//
// Only the numbers it writes are used by vultureOS (see Tests/UI/Measured). The
// pictures it can leave behind (--pictures DIR) are Apple's art and stay out of the
// repository: they are for looking at, next to our own.
//
//   Meta/Calibration/measure.sh [--pictures DIR] > measured.json

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

@interface MeasureWindow : NSWindow
@property BOOL pretendsKey;
@end
@implementation MeasureWindow
- (BOOL)isKeyWindow { return self.pretendsKey; }
- (BOOL)isMainWindow { return self.pretendsKey; }
- (BOOL)canBecomeKeyWindow { return YES; }
// Not public: what controls ask before drawing in their active colors.
- (BOOL)hasKeyAppearance { return self.pretendsKey; }
- (BOOL)hasMainAppearance { return self.pretendsKey; }
- (BOOL)_hasKeyAppearance { return self.pretendsKey; }
- (BOOL)_hasMainAppearance { return self.pretendsKey; }
- (BOOL)_hasActiveAppearance { return self.pretendsKey; }
- (BOOL)_hasActiveControls { return self.pretendsKey; }
@end

static BOOL always_active(id self, SEL _cmd) { return YES; }

static NSString* s_pictures = nil;
static NSMutableDictionary* s_controls = nil;

static NSBitmapImageRep* render(NSView* view, NSRect area, CGFloat scale)
{
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:(NSInteger)(area.size.width * scale)
                      pixelsHigh:(NSInteger)(area.size.height * scale)
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:0
                    bitsPerPixel:0];
    rep.size = area.size;
    [view cacheDisplayInRect:area toBitmapImageRep:rep];
    return rep;
}

static NSString* hex_at(NSBitmapImageRep* rep, NSInteger x, NSInteger y)
{
    NSUInteger p[4] = { 0, 0, 0, 0 };
    [rep getPixel:p atX:x y:y];
    if (p[3] == 255)
        return [NSString stringWithFormat:@"%02lx%02lx%02lx", p[0], p[1], p[2]];
    return [NSString stringWithFormat:@"%02lx%02lx%02lx%02lx", p[0], p[1], p[2], p[3]];
}

static NSUInteger difference(NSBitmapImageRep* a, NSBitmapImageRep* b, NSInteger x, NSInteger y)
{
    NSUInteger pa[4] = { 0, 0, 0, 0 }, pb[4] = { 0, 0, 0, 0 };
    [a getPixel:pa atX:x y:y];
    [b getPixel:pb atX:x y:y];
    NSUInteger most = 0;
    for (int i = 0; i < 4; ++i) {
        NSUInteger d = pa[i] > pb[i] ? pa[i] - pb[i] : pb[i] - pa[i];
        if (d > most)
            most = d;
    }
    return most;
}

// The smallest rectangle holding what differs from the empty background by more
// than `threshold`: in pixels, from the control's frame's top-left.
static NSDictionary* visible_box(NSBitmapImageRep* with, NSBitmapImageRep* without, NSInteger origin_x, NSInteger origin_y, NSUInteger threshold)
{
    NSInteger left = NSIntegerMax, top = NSIntegerMax, right = NSIntegerMin, bottom = NSIntegerMin;
    for (NSInteger y = 0; y < with.pixelsHigh; ++y) {
        for (NSInteger x = 0; x < with.pixelsWide; ++x) {
            if (difference(with, without, x, y) <= threshold)
                continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (right < left)
        return @{ @"x" : @0, @"y" : @0, @"w" : @0, @"h" : @0 };
    return @{ @"x" : @(left - origin_x), @"y" : @(top - origin_y), @"w" : @(right - left + 1), @"h" : @(bottom - top + 1) };
}

typedef NSView* (^Maker)(void);

// Draws one control on a window's background, and without it, and notes the rest.
static void measure(NSString* name, Maker maker)
{
    NSMutableDictionary* entry = [NSMutableDictionary dictionary];
    NSString* appearances[2] = { @"light", @"dark" };
    for (int dark = 0; dark < 2; ++dark) {
        for (int scale = 1; scale <= 2; ++scale) {
            NSRect content = NSMakeRect(0, 0, 360, 240);
            MeasureWindow* window = [[MeasureWindow alloc] initWithContentRect:content styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:YES];
            window.pretendsKey = YES;
            window.releasedWhenClosed = NO;
            window.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
            NSAppearance.currentAppearance = window.appearance;
            NSView* background = window.contentView;
            NSBitmapImageRep* without = render(background, content, scale);
            NSView* control = maker();
            NSSize size = control.frame.size;
            if (size.width == 0 || size.height == 0) {
                size = control.intrinsicContentSize;
                if (size.width <= 0)
                    size.width = 160;
                if (size.height <= 0)
                    size.height = 20;
            }
            NSPoint origin = NSMakePoint(60, 240 - 60 - size.height);
            control.frame = (NSRect) { origin, size };
            [background addSubview:control];
            [background layoutSubtreeIfNeeded];
            if ([control isKindOfClass:NSButton.class] && [((NSButton*)control).keyEquivalent isEqualToString:@"\r"])
                window.defaultButtonCell = ((NSButton*)control).cell;
            NSBitmapImageRep* with = render(background, content, scale);

            NSInteger frame_x = (NSInteger)(origin.x * scale);
            NSInteger frame_y = (NSInteger)((240 - origin.y - size.height) * scale);
            NSInteger middle_x = frame_x + (NSInteger)(size.width * scale / 2);
            NSInteger middle_y = frame_y + (NSInteger)(size.height * scale / 2);

            NSMutableDictionary* one = [NSMutableDictionary dictionary];
            one[@"box"] = visible_box(with, without, frame_x, frame_y, 32);
            one[@"softBox"] = visible_box(with, without, frame_x, frame_y, 2);

            // Lines through the middle, reaching 8 points past the frame.
            NSMutableArray* down = [NSMutableArray array];
            for (NSInteger y = frame_y - 8 * scale; y < frame_y + (NSInteger)(size.height * scale) + 8 * scale; ++y)
                [down addObject:hex_at(with, middle_x, y)];
            NSMutableArray* across = [NSMutableArray array];
            for (NSInteger x = frame_x - 8 * scale; x < frame_x + (NSInteger)(size.width * scale) + 8 * scale; ++x)
                [across addObject:hex_at(with, x, middle_y)];
            one[@"down"] = down;
            one[@"across"] = across;
            one[@"background"] = hex_at(without, 4, 4);

            // The dark ink in it (a label, a glyph): where text sits. Only light
            // appearance has dark ink on a light fill.
            NSInteger ink_left = NSIntegerMax, ink_top = NSIntegerMax, ink_right = NSIntegerMin, ink_bottom = NSIntegerMin;
            for (NSInteger y = 0; y < with.pixelsHigh; ++y) {
                for (NSInteger x = 0; x < with.pixelsWide; ++x) {
                    NSUInteger p[4];
                    [with getPixel:p atX:x y:y];
                    if (p[3] < 200 || p[0] >= 128 || p[1] >= 128 || p[2] >= 128)
                        continue;
                    if (x < ink_left) ink_left = x;
                    if (x > ink_right) ink_right = x;
                    if (y < ink_top) ink_top = y;
                    if (y > ink_bottom) ink_bottom = y;
                }
            }
            if (ink_right >= ink_left)
                one[@"ink"] = @{ @"x" : @(ink_left - frame_x), @"y" : @(ink_top - frame_y), @"w" : @(ink_right - ink_left + 1), @"h" : @(ink_bottom - ink_top + 1) };

            // The top-left of what shows, for working out corners.
            NSDictionary* box = one[@"box"];
            NSMutableArray* corner = [NSMutableArray array];
            NSInteger box_x = frame_x + [box[@"x"] integerValue], box_y = frame_y + [box[@"y"] integerValue];
            for (NSInteger y = box_y; y < box_y + 16 * scale && y < with.pixelsHigh; ++y) {
                NSMutableArray* row = [NSMutableArray array];
                for (NSInteger x = box_x; x < box_x + 16 * scale && x < with.pixelsWide; ++x)
                    [row addObject:hex_at(with, x, y)];
                [corner addObject:row];
            }
            one[@"corner"] = corner;

            NSString* key = [NSString stringWithFormat:@"%@-%dx", appearances[dark], scale];
            entry[key] = one;
            entry[@"frame"] = @{ @"w" : @(size.width), @"h" : @(size.height) };
            NSSize intrinsic = control.intrinsicContentSize;
            entry[@"intrinsic"] = @{ @"w" : @(intrinsic.width), @"h" : @(intrinsic.height) };
            NSEdgeInsets insets = control.alignmentRectInsets;
            entry[@"alignmentInsets"] = @{ @"top" : @(insets.top), @"left" : @(insets.left), @"bottom" : @(insets.bottom), @"right" : @(insets.right) };

            if (s_pictures) {
                NSRect around = NSInsetRect(control.frame, -12, -12);
                // Out of the whole render: rep rows run from the top.
                NSInteger crop_x = (NSInteger)(around.origin.x * scale);
                NSInteger crop_y = (NSInteger)((240 - around.origin.y - around.size.height) * scale);
                NSInteger crop_w = (NSInteger)(around.size.width * scale);
                NSInteger crop_h = (NSInteger)(around.size.height * scale);
                CGImageRef cropped = CGImageCreateWithImageInRect(with.CGImage, CGRectMake(crop_x, crop_y, crop_w, crop_h));
                NSBitmapImageRep* out = [[NSBitmapImageRep alloc] initWithCGImage:cropped];
                CGImageRelease(cropped);
                NSString* file = [NSString stringWithFormat:@"%@/%@-%@-%dx.png", s_pictures, name, appearances[dark], scale];
                [[out representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:file atomically:YES];
            }
            [window close];
        }
    }
    s_controls[name] = entry;
}

static NSString* color_string(NSColor* color)
{
    NSColor* srgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (!srgb)
        return @"pattern";
    int r = (int)lround(srgb.redComponent * 255), g = (int)lround(srgb.greenComponent * 255), b = (int)lround(srgb.blueComponent * 255), a = (int)lround(srgb.alphaComponent * 255);
    if (a == 255)
        return [NSString stringWithFormat:@"#%02x%02x%02x", r, g, b];
    return [NSString stringWithFormat:@"#%02x%02x%02x%02x", r, g, b, a];
}

static NSDictionary* system_colors(void)
{
    NSDictionary* colors = @{
        @"label" : NSColor.labelColor,
        @"secondaryLabel" : NSColor.secondaryLabelColor,
        @"tertiaryLabel" : NSColor.tertiaryLabelColor,
        @"quaternaryLabel" : NSColor.quaternaryLabelColor,
        @"text" : NSColor.textColor,
        @"placeholderText" : NSColor.placeholderTextColor,
        @"selectedText" : NSColor.selectedTextColor,
        @"textBackground" : NSColor.textBackgroundColor,
        @"selectedTextBackground" : NSColor.selectedTextBackgroundColor,
        @"unemphasizedSelectedTextBackground" : NSColor.unemphasizedSelectedTextBackgroundColor,
        @"keyboardFocusIndicator" : NSColor.keyboardFocusIndicatorColor,
        @"link" : NSColor.linkColor,
        @"separator" : NSColor.separatorColor,
        @"selectedContentBackground" : NSColor.selectedContentBackgroundColor,
        @"unemphasizedSelectedContentBackground" : NSColor.unemphasizedSelectedContentBackgroundColor,
        @"selectedMenuItemText" : NSColor.selectedMenuItemTextColor,
        @"grid" : NSColor.gridColor,
        @"headerText" : NSColor.headerTextColor,
        @"alternatingContentBackground0" : NSColor.alternatingContentBackgroundColors[0],
        @"alternatingContentBackground1" : NSColor.alternatingContentBackgroundColors[1],
        @"controlAccent" : NSColor.controlAccentColor,
        @"control" : NSColor.controlColor,
        @"controlBackground" : NSColor.controlBackgroundColor,
        @"controlText" : NSColor.controlTextColor,
        @"disabledControlText" : NSColor.disabledControlTextColor,
        @"selectedControl" : NSColor.selectedControlColor,
        @"selectedControlText" : NSColor.selectedControlTextColor,
        @"alternateSelectedControlText" : NSColor.alternateSelectedControlTextColor,
        @"windowBackground" : NSColor.windowBackgroundColor,
        @"windowFrameText" : NSColor.windowFrameTextColor,
        @"underPageBackground" : NSColor.underPageBackgroundColor,
        @"findHighlight" : NSColor.findHighlightColor,
        @"highlight" : NSColor.highlightColor,
        @"shadow" : NSColor.shadowColor,
        @"red" : NSColor.systemRedColor,
        @"orange" : NSColor.systemOrangeColor,
        @"yellow" : NSColor.systemYellowColor,
        @"green" : NSColor.systemGreenColor,
        @"mint" : NSColor.systemMintColor,
        @"teal" : NSColor.systemTealColor,
        @"cyan" : NSColor.systemCyanColor,
        @"blue" : NSColor.systemBlueColor,
        @"indigo" : NSColor.systemIndigoColor,
        @"purple" : NSColor.systemPurpleColor,
        @"pink" : NSColor.systemPinkColor,
        @"brown" : NSColor.systemBrownColor,
        @"gray" : NSColor.systemGrayColor,
    };
    NSMutableDictionary* result = [NSMutableDictionary dictionary];
    NSString* appearances[2] = { @"light", @"dark" };
    for (int dark = 0; dark < 2; ++dark) {
        NSAppearance* appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        NSMutableDictionary* one = [NSMutableDictionary dictionary];
        [appearance performAsCurrentDrawingAppearance:^{
            for (NSString* name in colors)
                one[name] = color_string(colors[name]);
        }];
        result[appearances[dark]] = one;
    }
    return result;
}

static NSDictionary* font_of(NSFont* font)
{
    NSDictionary* traits = [font.fontDescriptor objectForKey:NSFontTraitsAttribute];
    return @{ @"size" : @(font.pointSize), @"weight" : traits[NSFontWeightTrait] ?: @0, @"ascent" : @(font.ascender), @"descent" : @(font.descender), @"leading" : @(font.leading), @"capHeight" : @(font.capHeight), @"xHeight" : @(font.xHeight) };
}

static NSButton* button(NSString* title, NSButtonType type, NSBezelStyle bezel, NSControlSize size)
{
    NSButton* b = [[NSButton alloc] initWithFrame:NSZeroRect];
    b.title = title;
    [b setButtonType:type];
    b.bezelStyle = bezel;
    b.controlSize = size;
    b.font = [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:size]];
    [b sizeToFit];
    return b;
}

static void measure_controls(void)
{
    struct {
        NSControlSize size;
        NSString* name;
    } const sizes[] = { { NSControlSizeLarge, @"large" }, { NSControlSizeRegular, @"regular" }, { NSControlSizeSmall, @"small" }, { NSControlSizeMini, @"mini" } };
    for (size_t i = 0; i < 4; ++i) {
        NSControlSize s = sizes[i].size;
        NSString* n = sizes[i].name;
#define NAME(what) [NSString stringWithFormat:@"%@.%@", what, n]
        measure(NAME(@"button"), ^NSView* { return button(@"Button", NSButtonTypeMomentaryPushIn, NSBezelStyleRounded, s); });
        measure(NAME(@"checkbox.off"), ^NSView* { return button(@"", NSButtonTypeSwitch, NSBezelStyleRegularSquare, s); });
        measure(NAME(@"radio.off"), ^NSView* { return button(@"", NSButtonTypeRadio, NSBezelStyleRegularSquare, s); });
        measure(NAME(@"popup"), ^NSView* {
            NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            p.controlSize = s;
            p.font = [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:s]];
            [p addItemsWithTitles:@[ @"Popup" ]];
            [p sizeToFit];
            return p;
        });
        measure(NAME(@"segmented"), ^NSView* {
            NSSegmentedControl* c = [NSSegmentedControl segmentedControlWithLabels:@[ @"One", @"Two", @"Three" ] trackingMode:NSSegmentSwitchTrackingSelectOne target:nil action:nil];
            c.controlSize = s;
            c.font = [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:s]];
            c.selectedSegment = 1;
            [c sizeToFit];
            return c;
        });
        measure(NAME(@"stepper"), ^NSView* {
            NSStepper* c = [[NSStepper alloc] initWithFrame:NSZeroRect];
            c.controlSize = s;
            [c sizeToFit];
            return c;
        });
        measure(NAME(@"switch.on"), ^NSView* {
            NSSwitch* c = [[NSSwitch alloc] init];
            c.controlSize = s;
            c.state = NSControlStateValueOn;
            [c sizeToFit];
            return c;
        });
        measure(NAME(@"switch.off"), ^NSView* {
            NSSwitch* c = [[NSSwitch alloc] init];
            c.controlSize = s;
            [c sizeToFit];
            return c;
        });
        measure(NAME(@"slider"), ^NSView* {
            NSSlider* c = [NSSlider sliderWithValue:40 minValue:0 maxValue:100 target:nil action:nil];
            c.controlSize = s;
            c.frame = NSMakeRect(0, 0, 200, c.intrinsicContentSize.height);
            return c;
        });
        measure(NAME(@"field"), ^NSView* {
            NSTextField* c = [NSTextField textFieldWithString:@"Text"];
            c.controlSize = s;
            c.font = [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:s]];
            c.frame = NSMakeRect(0, 0, 160, c.intrinsicContentSize.height);
            return c;
        });
        measure(NAME(@"search"), ^NSView* {
            NSSearchField* c = [[NSSearchField alloc] initWithFrame:NSZeroRect];
            c.controlSize = s;
            c.font = [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:s]];
            c.frame = NSMakeRect(0, 0, 160, c.intrinsicContentSize.height);
            return c;
        });
        measure(NAME(@"combo"), ^NSView* {
            NSComboBox* c = [[NSComboBox alloc] initWithFrame:NSZeroRect];
            c.controlSize = s;
            c.font = [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:s]];
            c.stringValue = @"Combo";
            c.frame = NSMakeRect(0, 0, 160, c.intrinsicContentSize.height);
            return c;
        });
        measure(NAME(@"progress"), ^NSView* {
            NSProgressIndicator* c = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
            c.indeterminate = NO;
            c.controlSize = s;
            c.doubleValue = 40;
            c.frame = NSMakeRect(0, 0, 200, c.intrinsicContentSize.height > 0 ? c.intrinsicContentSize.height : 16);
            return c;
        });
#undef NAME
    }
    // States, at the regular size.
    measure(@"button.default", ^NSView* {
        NSButton* b = button(@"Button", NSButtonTypeMomentaryPushIn, NSBezelStyleRounded, NSControlSizeRegular);
        b.keyEquivalent = @"\r";
        return b;
    });
    measure(@"button.disabled", ^NSView* {
        NSButton* b = button(@"Button", NSButtonTypeMomentaryPushIn, NSBezelStyleRounded, NSControlSizeRegular);
        b.enabled = NO;
        return b;
    });
    measure(@"button.pressed", ^NSView* {
        NSButton* b = button(@"Button", NSButtonTypeMomentaryPushIn, NSBezelStyleRounded, NSControlSizeRegular);
        b.highlighted = YES;
        return b;
    });
    measure(@"button.textured", ^NSView* { return button(@"Button", NSButtonTypeMomentaryPushIn, NSBezelStyleTexturedRounded, NSControlSizeRegular); });
    measure(@"button.help", ^NSView* { return button(@"", NSButtonTypeMomentaryPushIn, NSBezelStyleHelpButton, NSControlSizeRegular); });
    measure(@"button.disclosure", ^NSView* { return button(@"", NSButtonTypeOnOff, NSBezelStyleDisclosure, NSControlSizeRegular); });
    measure(@"button.roundedDisclosure", ^NSView* { return button(@"", NSButtonTypeOnOff, NSBezelStylePushDisclosure, NSControlSizeRegular); });
    measure(@"button.circular", ^NSView* { return button(@"", NSButtonTypeMomentaryPushIn, NSBezelStyleCircular, NSControlSizeRegular); });
    measure(@"checkbox.on", ^NSView* {
        NSButton* b = button(@"", NSButtonTypeSwitch, NSBezelStyleRegularSquare, NSControlSizeRegular);
        b.state = NSControlStateValueOn;
        return b;
    });
    measure(@"checkbox.mixed", ^NSView* {
        NSButton* b = button(@"", NSButtonTypeSwitch, NSBezelStyleRegularSquare, NSControlSizeRegular);
        b.allowsMixedState = YES;
        b.state = NSControlStateValueMixed;
        return b;
    });
    measure(@"checkbox.disabled", ^NSView* {
        NSButton* b = button(@"", NSButtonTypeSwitch, NSBezelStyleRegularSquare, NSControlSizeRegular);
        b.state = NSControlStateValueOn;
        b.enabled = NO;
        return b;
    });
    measure(@"radio.on", ^NSView* {
        NSButton* b = button(@"", NSButtonTypeRadio, NSBezelStyleRegularSquare, NSControlSizeRegular);
        b.state = NSControlStateValueOn;
        return b;
    });
    measure(@"slider.ticks", ^NSView* {
        NSSlider* c = [NSSlider sliderWithValue:40 minValue:0 maxValue:100 target:nil action:nil];
        c.numberOfTickMarks = 11;
        c.frame = NSMakeRect(0, 0, 200, c.intrinsicContentSize.height);
        return c;
    });
    measure(@"slider.vertical", ^NSView* {
        NSSlider* c = [NSSlider sliderWithValue:40 minValue:0 maxValue:100 target:nil action:nil];
        c.vertical = YES;
        c.frame = NSMakeRect(0, 0, c.intrinsicContentSize.width, 120);
        return c;
    });
    measure(@"slider.disabled", ^NSView* {
        NSSlider* c = [NSSlider sliderWithValue:40 minValue:0 maxValue:100 target:nil action:nil];
        c.enabled = NO;
        c.frame = NSMakeRect(0, 0, 200, c.intrinsicContentSize.height);
        return c;
    });
    measure(@"segmented.capsule", ^NSView* {
        NSSegmentedControl* c = [NSSegmentedControl segmentedControlWithLabels:@[ @"One", @"Two" ] trackingMode:NSSegmentSwitchTrackingSelectOne target:nil action:nil];
        c.segmentStyle = NSSegmentStyleCapsule;
        c.selectedSegment = 0;
        [c sizeToFit];
        return c;
    });
    measure(@"popup.pulldown", ^NSView* {
        NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
        [p addItemsWithTitles:@[ @"Pull", @"Item" ]];
        [p sizeToFit];
        return p;
    });
    measure(@"field.disabled", ^NSView* {
        NSTextField* c = [NSTextField textFieldWithString:@"Text"];
        c.enabled = NO;
        c.frame = NSMakeRect(0, 0, 160, c.intrinsicContentSize.height);
        return c;
    });
    measure(@"field.placeholder", ^NSView* {
        NSTextField* c = [NSTextField textFieldWithString:@""];
        c.placeholderString = @"Text";
        c.frame = NSMakeRect(0, 0, 160, c.intrinsicContentSize.height);
        return c;
    });
    measure(@"field.rounded", ^NSView* {
        NSTextField* c = [NSTextField textFieldWithString:@"Text"];
        c.bezelStyle = NSTextFieldRoundedBezel;
        c.frame = NSMakeRect(0, 0, 160, c.intrinsicContentSize.height);
        return c;
    });
    measure(@"spinner", ^NSView* {
        NSProgressIndicator* c = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        c.style = NSProgressIndicatorStyleSpinning;
        return c;
    });
    measure(@"box", ^NSView* {
        NSBox* c = [[NSBox alloc] initWithFrame:NSMakeRect(0, 0, 200, 80)];
        c.title = @"Box";
        return c;
    });
    measure(@"colorwell", ^NSView* {
        NSColorWell* c = [[NSColorWell alloc] initWithFrame:NSMakeRect(0, 0, 44, 23)];
        c.color = NSColor.systemBlueColor;
        return c;
    });
    measure(@"level", ^NSView* {
        NSLevelIndicator* c = [[NSLevelIndicator alloc] initWithFrame:NSMakeRect(0, 0, 200, 18)];
        c.levelIndicatorStyle = NSLevelIndicatorStyleContinuousCapacity;
        c.minValue = 0;
        c.maxValue = 100;
        c.doubleValue = 40;
        return c;
    });
    measure(@"separator", ^NSView* {
        NSBox* c = [[NSBox alloc] initWithFrame:NSMakeRect(0, 0, 200, 1)];
        c.boxType = NSBoxSeparator;
        return c;
    });
    measure(@"scroller.overlay", ^NSView* {
        NSScroller* c = [[NSScroller alloc] initWithFrame:NSMakeRect(0, 0, 16, 160)];
        c.scrollerStyle = NSScrollerStyleOverlay;
        c.enabled = YES;
        c.knobProportion = 0.4;
        c.doubleValue = 0.3;
        return c;
    });
    measure(@"scroller.legacy", ^NSView* {
        NSScroller* c = [[NSScroller alloc] initWithFrame:NSMakeRect(0, 0, 16, 160)];
        c.scrollerStyle = NSScrollerStyleLegacy;
        c.enabled = YES;
        c.knobProportion = 0.4;
        c.doubleValue = 0.3;
        return c;
    });
    measure(@"table.header", ^NSView* {
        NSTableHeaderCell* cell = [[NSTableHeaderCell alloc] initTextCell:@"Name"];
        NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:@"a"];
        column.headerCell = cell;
        column.width = 160;
        NSTableView* table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 200, 22)];
        [table addTableColumn:column];
        NSTableHeaderView* header = table.headerView;
        header.frame = NSMakeRect(0, 0, 200, header.frame.size.height);
        return header;
    });
}

// Numbers AppKit will state outright.
static NSDictionary* stated_metrics(void)
{
    NSMutableDictionary* m = [NSMutableDictionary dictionary];

    // Text in buttons: what is left around the label is the control's own padding,
    // the number that carries over to a font of other widths.
    NSFont* body = [NSFont systemFontOfSize:13];
    m[@"labelWidth"] = @{
        @"Button@13" : @([@"Button" sizeWithAttributes:@{ NSFontAttributeName : body }].width),
        @"Button@11" : @([@"Button" sizeWithAttributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:11] }].width),
        @"Button@9" : @([@"Button" sizeWithAttributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:9] }].width),
        @"Popup@13" : @([@"Popup" sizeWithAttributes:@{ NSFontAttributeName : body }].width),
        @"One@13" : @([@"One" sizeWithAttributes:@{ NSFontAttributeName : body }].width),
        @"Two@13" : @([@"Two" sizeWithAttributes:@{ NSFontAttributeName : body }].width),
        @"Three@13" : @([@"Three" sizeWithAttributes:@{ NSFontAttributeName : body }].width),
    };

    m[@"fonts"] = @{
        @"system" : font_of([NSFont systemFontOfSize:NSFont.systemFontSize]),
        @"small" : font_of([NSFont systemFontOfSize:NSFont.smallSystemFontSize]),
        @"label" : font_of([NSFont labelFontOfSize:NSFont.labelFontSize]),
        @"mini" : font_of([NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:NSControlSizeMini]]),
        @"large" : font_of([NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:NSControlSizeLarge]]),
        @"titleBar" : font_of([NSFont titleBarFontOfSize:0]),
        @"menu" : font_of([NSFont menuFontOfSize:0]),
        @"menuBar" : font_of([NSFont menuBarFontOfSize:0]),
        @"toolTips" : font_of([NSFont toolTipsFontOfSize:0]),
        @"message" : font_of([NSFont messageFontOfSize:0]),
        @"palette" : font_of([NSFont paletteFontOfSize:0]),
        @"controlContent" : font_of([NSFont controlContentFontOfSize:0]),
        @"mono" : font_of([NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular]),
        @"largeTitle" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleLargeTitle options:@{}]),
        @"title1" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleTitle1 options:@{}]),
        @"title2" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleTitle2 options:@{}]),
        @"title3" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleTitle3 options:@{}]),
        @"headline" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleHeadline options:@{}]),
        @"subheadline" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleSubheadline options:@{}]),
        @"body" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleBody options:@{}]),
        @"callout" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleCallout options:@{}]),
        @"footnote" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleFootnote options:@{}]),
        @"caption1" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleCaption1 options:@{}]),
        @"caption2" : font_of([NSFont preferredFontForTextStyle:NSFontTextStyleCaption2 options:@{}]),
    };

    // Menus: a menu's whole height over some rows.
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"M"];
    [menu addItemWithTitle:@"One" action:nil keyEquivalent:@""];
    NSSize one_row = menu.size;
    [menu addItemWithTitle:@"Two" action:nil keyEquivalent:@""];
    NSSize two_rows = menu.size;
    [menu addItem:NSMenuItem.separatorItem];
    NSSize with_separator = menu.size;
    [menu addItemWithTitle:@"Long enough to set the width" action:nil keyEquivalent:@"K"];
    NSSize with_key = menu.size;
    m[@"menu"] = @{
        @"oneRow" : @(one_row.height),
        @"twoRows" : @(two_rows.height),
        @"rowHeight" : @(two_rows.height - one_row.height),
        @"separatorHeight" : @(with_separator.height - two_rows.height),
        @"verticalPadding" : @(one_row.height - (two_rows.height - one_row.height)),
        @"widthOneShortRow" : @(one_row.width),
        @"longRowWidth" : @(with_key.width),
        @"longRowTextWidth" : @([@"Long enough to set the width" sizeWithAttributes:@{ NSFontAttributeName : [NSFont menuFontOfSize:0] }].width),
    };

    // Windows and their title bars.
    NSRect content = NSMakeRect(0, 0, 400, 300);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow* w = [[NSWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:YES];
    w.releasedWhenClosed = NO;
    CGFloat title_height = w.frame.size.height - content.size.height;
    NSMutableDictionary* window = [@{
        @"titleBarHeight" : @(title_height),
        @"closeFrame" : NSStringFromRect([w standardWindowButton:NSWindowCloseButton].frame),
        @"minimizeFrame" : NSStringFromRect([w standardWindowButton:NSWindowMiniaturizeButton].frame),
        @"zoomFrame" : NSStringFromRect([w standardWindowButton:NSWindowZoomButton].frame),
    } mutableCopy];
    struct {
        NSString* name;
        NSWindowToolbarStyle style;
    } const toolbar_styles[] = { { @"toolbarExpanded", NSWindowToolbarStyleExpanded }, { @"toolbarUnified", NSWindowToolbarStyleUnified }, { @"toolbarUnifiedCompact", NSWindowToolbarStyleUnifiedCompact } };
    for (size_t i = 0; i < 3; ++i) {
        NSWindow* t = [[NSWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:YES];
        t.releasedWhenClosed = NO;
        t.toolbar = [[NSToolbar alloc] initWithIdentifier:toolbar_styles[i].name];
        t.toolbarStyle = toolbar_styles[i].style;
        window[toolbar_styles[i].name] = @(t.frame.size.height - t.contentLayoutRect.size.height);
    }
    // Not public, and not always there: only for measuring.
    if ([w respondsToSelector:@selector(_cornerRadius)])
        window[@"cornerRadius"] = [w valueForKey:@"_cornerRadius"];
    m[@"window"] = window;

    // Tables.
    NSTableView* table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:@"a"];
    [table addTableColumn:column];
    NSOutlineView* outline = [[NSOutlineView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    m[@"table"] = @{
        @"rowHeight" : @(table.rowHeight),
        @"intercellWidth" : @(table.intercellSpacing.width),
        @"intercellHeight" : @(table.intercellSpacing.height),
        @"headerHeight" : @(table.headerView.frame.size.height),
        @"outlineIndent" : @(outline.indentationPerLevel),
    };

    NSSplitView* split = [[NSSplitView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    NSMutableDictionary* splits = [NSMutableDictionary dictionary];
    split.dividerStyle = NSSplitViewDividerStyleThin;
    splits[@"thin"] = @(split.dividerThickness);
    split.dividerStyle = NSSplitViewDividerStyleThick;
    splits[@"thick"] = @(split.dividerThickness);
    split.dividerStyle = NSSplitViewDividerStylePaneSplitter;
    splits[@"pane"] = @(split.dividerThickness);
    m[@"split"] = splits;

    m[@"scroller"] = @{
        @"overlay" : @([NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleOverlay]),
        @"legacy" : @([NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleLegacy]),
        @"overlaySmall" : @([NSScroller scrollerWidthForControlSize:NSControlSizeSmall scrollerStyle:NSScrollerStyleOverlay]),
    };

    NSTabView* tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)];
    NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:@"a"];
    item.label = @"Tab";
    [tabs addTabViewItem:item];
    NSRect tab_content = tabs.contentRect;
    NSSize tab_minimum = tabs.minimumSize;
    m[@"tabs"] = @{
        @"contentInsetLeft" : @(tab_content.origin.x),
        @"contentInsetTop" : @(tab_content.origin.y),
        @"contentInsetRight" : @(300 - NSMaxX(tab_content)),
        @"contentInsetBottom" : @(200 - NSMaxY(tab_content)),
        @"minimumWidth" : @(tab_minimum.width),
        @"minimumHeight" : @(tab_minimum.height),
    };

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Message";
    alert.informativeText = @"Informative text";
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    [alert layout];
    m[@"alert"] = @{ @"width" : @(alert.window.frame.size.width), @"height" : @(alert.window.frame.size.height) };

    m[@"animation"] = @{
        @"contextDefaultDuration" : @([NSAnimationContext currentContext].duration),
        @"windowResizeTime" : @([w animationResizeTime:NSMakeRect(0, 0, 800, 600)]),
    };
    return m;
}

// The window's frame view as it draws itself: title bar, traffic lights, corners.
static NSDictionary* measure_chrome(void)
{
    NSMutableDictionary* result = [NSMutableDictionary dictionary];
    NSString* appearances[2] = { @"light", @"dark" };
    for (int dark = 0; dark < 2; ++dark) {
        for (int key = 0; key < 2; ++key) {
            for (int scale = 1; scale <= 2; ++scale) {
                NSRect content = NSMakeRect(0, 0, 300, 100);
                NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
                MeasureWindow* w = [[MeasureWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:YES];
                w.pretendsKey = key;
                w.releasedWhenClosed = NO;
                w.title = @"Title";
                w.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
                NSView* frame_view = w.contentView.superview;
                NSRect area = frame_view.bounds;
                [frame_view layoutSubtreeIfNeeded];
                NSBitmapImageRep* rep = render(frame_view, area, scale);
                NSMutableDictionary* one = [NSMutableDictionary dictionary];
                NSMutableArray* corner = [NSMutableArray array];
                for (NSInteger y = 0; y < 20 * scale; ++y) {
                    NSMutableArray* row = [NSMutableArray array];
                    for (NSInteger x = 0; x < 20 * scale; ++x) {
                        NSUInteger p[4];
                        [rep getPixel:p atX:x y:y];
                        [row addObject:@(p[3])];
                    }
                    [corner addObject:row];
                }
                one[@"cornerAlpha"] = corner;
                NSMutableArray* down = [NSMutableArray array];
                for (NSInteger y = 0; y < rep.pixelsHigh; ++y)
                    [down addObject:hex_at(rep, 150 * scale, y)];
                one[@"downMiddle"] = down;
                NSMutableArray* lights = [NSMutableArray array];
                for (NSInteger x = 0; x < 72 * scale; ++x)
                    [lights addObject:hex_at(rep, x, 14 * scale)];
                one[@"acrossLights"] = lights;
                NSString* key_name = [NSString stringWithFormat:@"%@-%@-%dx", appearances[dark], key ? @"key" : @"inactive", scale];
                result[key_name] = one;
                if (s_pictures) {
                    NSString* file = [NSString stringWithFormat:@"%@/chrome-%@.png", s_pictures, key_name];
                    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:file atomically:YES];
                }
                [w close];
            }
        }
    }
    return result;
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        for (int i = 1; i + 1 < argc; ++i) {
            if (strcmp(argv[i], "--pictures") == 0) {
                s_pictures = [NSString stringWithUTF8String:argv[i + 1]];
                [NSFileManager.defaultManager createDirectoryAtPath:s_pictures withIntermediateDirectories:YES attributes:nil error:nil];
            }
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        // Controls draw as they do in the frontmost program.
        class_replaceMethod(NSApplication.class, @selector(isActive), (IMP)always_active, "c@:");

        s_controls = [NSMutableDictionary dictionary];
        NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
        NSMutableDictionary* result = [NSMutableDictionary dictionary];
        result[@"measuredOn"] = [NSString stringWithFormat:@"macOS %ld.%ld, AppKit linked against the macOS 15 SDK", os.majorVersion, os.minorVersion];
        result[@"colors"] = system_colors();
        result[@"metrics"] = stated_metrics();
        result[@"chrome"] = measure_chrome();
        measure_controls();
        result[@"controls"] = s_controls;

        NSData* json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
        [json writeToFile:@"/dev/stdout" atomically:NO];
    }
    return 0;
}
