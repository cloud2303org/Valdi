#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#include "valdi_core/cpp/Utils/Value.hpp"
#include <iostream>
#include <stdexcept>

// Generated verbatim from the production NSNumber branch for this test run.
// The complete production .mm is also compiled against the official headers.
static Valdi::Value convertNumber(NSNumber* object) {
#include "NSNumberBranch.inc"
    throw std::runtime_error("Expected NSNumber");
}

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    @autoreleasepool {
        for (NSNumber* boolean in @[@YES, @NO, [NSNumber numberWithBool:YES],
                                    [NSNumber numberWithBool:NO],
                                    (__bridge NSNumber*)kCFBooleanTrue,
                                    (__bridge NSNumber*)kCFBooleanFalse]) {
            const auto value = convertNumber(boolean);
            require(value.getType() == Valdi::ValueType::Bool, "CFBoolean lost its boolean type");
            require(value.toBool() == static_cast<bool>(boolean.boolValue), "Boolean value changed");
        }
        for (NSNumber* number in @[@0, @1, @0.0, @1.0, @(-0.0), @(-17), @1.25,
                                   [NSNumber numberWithChar:1], [NSNumber numberWithChar:0],
                                   [NSNumber numberWithUnsignedChar:1],
                                   [NSNumber numberWithShort:0], [NSNumber numberWithLongLong:1]]) {
            const auto value = convertNumber(number);
            require(value.getType() == Valdi::ValueType::Double, "Numeric NSNumber was reclassified");
            require(value.toDouble() == number.doubleValue, "Numeric value changed");
        }
        for (int32_t number : {0, 1, -42}) {
            auto cf = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &number);
            const auto value = convertNumber((__bridge NSNumber*)cf);
            CFRelease(cf);
            require(value.getType() == Valdi::ValueType::Double, "CFNumber was reclassified");
            require(value.toDouble() == number, "CFNumber changed");
        }
        std::cout << "PASS Foundation CFBoolean and numeric NSNumber production branch\n";
    }
}
