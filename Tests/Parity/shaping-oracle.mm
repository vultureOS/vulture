/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

// What Core Text does with the same text in the same font, for comparing with Tests/LibText/ShapePrint.cpp. Only ever built on a Mac, and never part of the system.
//
//     shaping-oracle FONT.ttf RTL TEXT...

int main(int argc, char** argv)
{
    @autoreleasepool {
        if (argc < 4)
            return 2;
        NSData* data = [NSData dataWithContentsOfFile:@(argv[1])];
        CTFontDescriptorRef descriptor = CTFontManagerCreateFontDescriptorFromData((__bridge CFDataRef)data);
        // At the font's own size, so that what comes back is in its own units.
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGFontRef graphics_font = CGFontCreateWithDataProvider(provider);
        CGFloat const units = CGFontGetUnitsPerEm(graphics_font);
        CTFontRef font = CTFontCreateWithFontDescriptor(descriptor, units, nullptr);
        bool const is_right_to_left = atoi(argv[2]) != 0;
        for (int i = 3; i < argc; ++i) {
            NSString* text = @(argv[i]);
            NSDictionary* attributes = @{ (id)kCTFontAttributeName : (__bridge id)font };
            CTParagraphStyleSetting setting;
            CTWritingDirection direction = is_right_to_left ? kCTWritingDirectionRightToLeft : kCTWritingDirectionLeftToRight;
            setting.spec = kCTParagraphStyleSpecifierBaseWritingDirection;
            setting.valueSize = sizeof(direction);
            setting.value = &direction;
            CTParagraphStyleRef style = CTParagraphStyleCreate(&setting, 1);
            NSMutableDictionary* with_style = [attributes mutableCopy];
            with_style[(id)kCTParagraphStyleAttributeName] = (__bridge id)style;
            NSAttributedString* string = [[NSAttributedString alloc] initWithString:text attributes:with_style];
            CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)string);
            printf("%s:", argv[i]);
            NSArray* runs = (__bridge NSArray*)CTLineGetGlyphRuns(line);
            CGFloat end = 0;
            for (id item in runs) {
                CTRunRef run = (__bridge CTRunRef)item;
                CFIndex const count = CTRunGetGlyphCount(run);
                CGGlyph glyphs[count];
                CGPoint positions[count];
                CTRunGetGlyphs(run, CFRangeMake(0, count), glyphs);
                CTRunGetPositions(run, CFRangeMake(0, count), positions);
                for (CFIndex g = 0; g < count; ++g)
                    printf(" %u@%d,%d", glyphs[g], (int)lround(positions[g].x), (int)lround(positions[g].y));
            }
            end = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
            printf(" |%d\n", (int)lround(end));
            CFRelease(line);
            CFRelease(style);
        }
    }
    return 0;
}
