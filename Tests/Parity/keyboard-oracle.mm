/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>

// What each key types on a real Mac, in each layout: for Meta/generate-keyboards.py, which is where this system's layouts come from, and for checking them against. Only ever built on a
// Mac, and never part of the system.

// A Mac's codes for keys, and this system's (Kernel/API/POSIX/input.h), which go by position on the keyboard.
struct Key {
    UInt16 mac;
    int ours;
};

static Key const s_keys[] = {
    { 50, 41 }, { 18, 2 }, { 19, 3 }, { 20, 4 }, { 21, 5 }, { 23, 6 }, { 22, 7 }, { 26, 8 }, { 28, 9 }, { 25, 10 }, { 29, 11 }, { 27, 12 }, { 24, 13 },
    { 12, 16 }, { 13, 17 }, { 14, 18 }, { 15, 19 }, { 17, 20 }, { 16, 21 }, { 32, 22 }, { 34, 23 }, { 31, 24 }, { 35, 25 }, { 33, 26 }, { 30, 27 }, { 42, 43 },
    { 0, 30 }, { 1, 31 }, { 2, 32 }, { 3, 33 }, { 5, 34 }, { 4, 35 }, { 38, 36 }, { 40, 37 }, { 37, 38 }, { 41, 39 }, { 39, 40 },
    { 6, 44 }, { 7, 45 }, { 8, 46 }, { 9, 47 }, { 11, 48 }, { 45, 49 }, { 46, 50 }, { 43, 51 }, { 47, 52 }, { 44, 53 }, { 10, 86 },
    { 49, 57 },
};

static NSString* escaped(UniChar const* characters, UniCharCount count)
{
    NSMutableString* out = [NSMutableString string];
    for (UniCharCount i = 0; i < count; ++i)
        [out appendFormat:@"%04x", characters[i]];
    return out.length ? out : @"-";
}

int main()
{
    @autoreleasepool {
        NSArray<NSString*>* wanted = @[
            @"com.apple.keylayout.US", @"com.apple.keylayout.British", @"com.apple.keylayout.German", @"com.apple.keylayout.French", @"com.apple.keylayout.Spanish",
            @"com.apple.keylayout.Italian", @"com.apple.keylayout.Swedish-Pro", @"com.apple.keylayout.Dutch", @"com.apple.keylayout.Polish", @"com.apple.keylayout.Turkish",
            @"com.apple.keylayout.Russian", @"com.apple.keylayout.Arabic", @"com.apple.keylayout.Hebrew", @"com.apple.keylayout.Devanagari", @"com.apple.keylayout.Greek",
            @"com.apple.keylayout.Brazilian",
        ];
        // Each of what can be held down, in Carbon's way of saying it.
        UInt32 const shift = shiftKey >> 8;
        UInt32 const option = optionKey >> 8;
        UInt32 const caps = alphaLock >> 8;
        struct Layer {
            char const* name;
            UInt32 modifiers;
        };
        Layer const layers[] = { { "plain", 0 }, { "shift", shift }, { "option", option }, { "shift-option", shift | option }, { "caps", caps } };
        for (NSString* identifier in wanted) {
            NSDictionary* query = @{ (__bridge id)kTISPropertyInputSourceID : identifier };
            CFArrayRef sources = TISCreateInputSourceList((__bridge CFDictionaryRef)query, true);
            if (!sources || CFArrayGetCount(sources) == 0) {
                printf("layout %s: not on this Mac\n", identifier.UTF8String);
                continue;
            }
            TISInputSourceRef source = (TISInputSourceRef)CFArrayGetValueAtIndex(sources, 0);
            CFDataRef data = (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData);
            if (!data) {
                printf("layout %s: no data\n", identifier.UTF8String);
                continue;
            }
            auto const* layout = (UCKeyboardLayout const*)CFDataGetBytePtr(data);
            NSString* name = (__bridge NSString*)TISGetInputSourceProperty(source, kTISPropertyLocalizedName);
            printf("layout %s %s\n", [identifier substringFromIndex:20].UTF8String, [name stringByReplacingOccurrencesOfString:@" " withString:@"_"].UTF8String);
            for (auto const& layer : layers) {
                printf("layer %s\n", layer.name);
                for (auto const& key : s_keys) {
                    UInt32 dead = 0;
                    UniChar text[8];
                    UniCharCount length = 0;
                    UCKeyTranslate(layout, key.mac, kUCKeyActionDown, layer.modifiers, LMGetKbdType(), 0, &dead, 8, &length, text);
                    if (dead != 0) {
                        // A dead key: what it does to each letter, and what it is by itself.
                        UInt32 state = dead;
                        UniChar alone[8];
                        UniCharCount alone_length = 0;
                        UCKeyTranslate(layout, 49, kUCKeyActionDown, 0, LMGetKbdType(), 0, &state, 8, &alone_length, alone);
                        printf("%d dead %s\n", key.ours, escaped(alone, alone_length).UTF8String);
                        for (auto const& next : s_keys) {
                            UInt32 const with[] = { 0, shift };
                            for (UInt32 next_modifiers : with) {
                                state = dead;
                                UniChar combined[8];
                                UniCharCount combined_length = 0;
                                UCKeyTranslate(layout, next.mac, kUCKeyActionDown, next_modifiers, LMGetKbdType(), 0, &state, 8, &combined_length, combined);
                                if (combined_length == 1 && combined[0] > 0x7f)
                                    printf("compose %d %d %d %s\n", key.ours, next.ours, next_modifiers ? 1 : 0, escaped(combined, combined_length).UTF8String);
                            }
                        }
                    } else {
                        printf("%d %s\n", key.ours, escaped(text, length).UTF8String);
                    }
                }
            }
            CFRelease(sources);
        }
    }
    return 0;
}
