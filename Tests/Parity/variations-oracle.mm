/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

// What Core Text says a variable font's glyphs are at some settings: each one's advance and outline's bounds, in the font's own units. For comparing with Tests/LibText/ShapePrint.cpp. Only ever built on a Mac.
//
//     variations-oracle FONT WGHT WDTH TEXT

int main(int argc, char** argv)
{
    @autoreleasepool {
        if (argc != 5)
            return 2;
        NSData* data = [NSData dataWithContentsOfFile:@(argv[1])];
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGFontRef graphics_font = CGFontCreateWithDataProvider(provider);
        CGFloat const units = CGFontGetUnitsPerEm(graphics_font);
        CTFontDescriptorRef descriptor = CTFontManagerCreateFontDescriptorFromData((__bridge CFDataRef)data);
        CTFontRef base = CTFontCreateWithFontDescriptor(descriptor, units, nullptr);
        // By the four letters of each axis, as a number.
        NSDictionary* variations = @{ @(0x77676874) : @(atof(argv[2])), @(0x77647468) : @(atof(argv[3])) };
        CTFontDescriptorRef varied = CTFontDescriptorCreateCopyWithAttributes(CTFontCopyFontDescriptor(base), (__bridge CFDictionaryRef) @{ (id)kCTFontVariationAttribute : variations });
        CTFontRef font = CTFontCreateWithFontDescriptor(varied, units, nullptr);
        printf("%s %s:", argv[2], argv[3]);
        NSString* text = @(argv[4]);
        for (NSUInteger i = 0; i < text.length; ++i) {
            UniChar character = [text characterAtIndex:i];
            CGGlyph glyph = 0;
            CTFontGetGlyphsForCharacters(font, &character, &glyph, 1);
            CGSize advance {};
            CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &advance, 1);
            CGPathRef path = CTFontCreatePathForGlyph(font, glyph, nullptr);
            CGRect const bounds = path ? CGPathGetPathBoundingBox(path) : CGRectZero;
            printf(" %u:%d:%d,%d,%d,%d", glyph, (int)lround(advance.width), (int)lround(bounds.origin.x), (int)lround(bounds.origin.y), (int)lround(bounds.origin.x + bounds.size.width), (int)lround(bounds.origin.y + bounds.size.height));
            if (path)
                CGPathRelease(path);
        }
        printf("\n");
    }
    return 0;
}
