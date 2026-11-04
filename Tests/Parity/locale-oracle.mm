/*
 * Copyright (c) 2022-2026, Krisna Pranav
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Foundation/Foundation.h>

// What a real Mac says about numbers and dates, for Meta/oracle-test.sh to compare with Tests/LibLocale/LocalePrint.cpp, which says what this system does. Only ever built on
// a Mac, and never part of the system.

static void say(NSString* name, NSString* text)
{
    printf("commands: BEGIN %s\n%s\ncommands: END %s status=0\n", name.UTF8String, text.UTF8String, name.UTF8String);
}

static NSString* joined(NSArray<NSString*>* list)
{
    return [list componentsJoinedByString:@" | "];
}

int main()
{
    @autoreleasepool {
        NSArray<NSString*>* identifiers = @[ @"en_US", @"en_GB", @"fr_FR", @"de_DE", @"es_ES", @"it_IT", @"pt_BR", @"nl_NL", @"sv_SE", @"pl_PL", @"tr_TR", @"ru_RU", @"ar_EG", @"he_IL", @"hi_IN", @"ja_JP", @"ko_KR", @"zh_CN" ];
        NSDate* date = [NSDate dateWithTimeIntervalSince1970:1772719629]; // 2026-03-05 14:07:09 UTC, a Thursday.
        NSDate* morning = [NSDate dateWithTimeIntervalSince1970:1794546000]; // 2026-11-12 05:00:00 UTC, a Thursday.
        NSArray<NSNumber*>* styles = @[ @(NSDateFormatterFullStyle), @(NSDateFormatterLongStyle), @(NSDateFormatterMediumStyle), @(NSDateFormatterShortStyle) ];
        NSArray<NSString*>* styleNames = @[ @"full", @"long", @"medium", @"short" ];
        NSArray<NSString*>* words = @[ @"cote", @"côte", @"coté", @"côté", @"Z", @"z", @"a", @"A", @"ñandú", @"nube", @"nz", @"zebra", @"Ångström", @"Öl", @"Oz", @"Zoo", @"ı", @"i", @"I", @"İ", @"дом", @"α", @"ch", @"cz", @"ł", @"lz", @"m" ];
        for (NSString* identifier in identifiers) {
            NSLocale* locale = [NSLocale localeWithLocaleIdentifier:identifier];
            NSNumberFormatter* number = [NSNumberFormatter new];
            number.locale = locale;
            number.numberStyle = NSNumberFormatterDecimalStyle;
            for (NSNumber* value in @[ @1234.5678, @0.5, @-1234.5, @1234567.891, @12, @1000 ])
                say([NSString stringWithFormat:@"%@ number %@", identifier, value], [number stringFromNumber:value]);
            number.numberStyle = NSNumberFormatterPercentStyle;
            for (NSNumber* value in @[ @0.256, @1.5 ])
                say([NSString stringWithFormat:@"%@ percent %@", identifier, value], [number stringFromNumber:value]);
            number.numberStyle = NSNumberFormatterCurrencyStyle;
            for (NSString* code in @[ @"", @"USD", @"EUR", @"JPY", @"GBP" ]) {
                number.locale = locale;
                if (code.length)
                    number.currencyCode = code;
                for (NSNumber* value in @[ @1234.56, @-1234.56 ])
                    say([NSString stringWithFormat:@"%@ currency %@ %@", identifier, code, value], [number stringFromNumber:value]);
            }

            NSDateFormatter* formatter = [NSDateFormatter new];
            NSCalendar* calendar = [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
            calendar.locale = locale;
            formatter.calendar = calendar;
            formatter.locale = locale;
            formatter.timeZone = [NSTimeZone timeZoneWithName:@"Etc/UTC"];
            formatter.timeStyle = NSDateFormatterNoStyle;
            for (NSUInteger i = 0; i < 4; ++i) {
                formatter.dateStyle = (NSDateFormatterStyle)styles[i].integerValue;
                say([NSString stringWithFormat:@"%@ date %@", identifier, styleNames[i]], [formatter stringFromDate:date]);
            }
            formatter.dateStyle = NSDateFormatterNoStyle;
            for (NSUInteger i = 0; i < 4; ++i) {
                formatter.timeStyle = (NSDateFormatterStyle)styles[i].integerValue;
                say([NSString stringWithFormat:@"%@ time %@", identifier, styleNames[i]], [formatter stringFromDate:date]);
                say([NSString stringWithFormat:@"%@ morning %@", identifier, styleNames[i]], [formatter stringFromDate:morning]);
            }
            for (NSUInteger i = 0; i < 4; ++i) {
                formatter.dateStyle = (NSDateFormatterStyle)styles[i].integerValue;
                formatter.timeStyle = NSDateFormatterShortStyle;
                say([NSString stringWithFormat:@"%@ both %@", identifier, styleNames[i]], [formatter stringFromDate:date]);
            }
            say([NSString stringWithFormat:@"%@ months", identifier], joined(formatter.monthSymbols));
            say([NSString stringWithFormat:@"%@ short months", identifier], joined(formatter.shortMonthSymbols));
            say([NSString stringWithFormat:@"%@ standalone months", identifier], joined(formatter.standaloneMonthSymbols));
            say([NSString stringWithFormat:@"%@ days", identifier], joined(formatter.weekdaySymbols));
            say([NSString stringWithFormat:@"%@ short days", identifier], joined(formatter.shortWeekdaySymbols));
            say([NSString stringWithFormat:@"%@ am and pm", identifier], [NSString stringWithFormat:@"%@ %@", formatter.AMSymbol, formatter.PMSymbol]);
            say([NSString stringWithFormat:@"%@ first day of the week", identifier], [NSString stringWithFormat:@"%lu", (unsigned long)calendar.firstWeekday - 1]);

            NSArray<NSString*>* sorted = [words sortedArrayUsingComparator:^NSComparisonResult(NSString* a, NSString* b) {
                return [a compare:b options:0 range:NSMakeRange(0, a.length) locale:locale];
            }];
            say([NSString stringWithFormat:@"%@ sorted", identifier], joined(sorted));
        }
    }
    return 0;
}
