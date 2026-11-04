#include "../../vector_checks.hpp"

#include <jni.h>

#include <exception>
#include <string>

namespace {

// NewStringUTF expects modified UTF-8. Preserve ASCII report text and escape
// every other byte, including embedded NUL, so no invalid sequence reaches JNI.
jstring asciiString(JNIEnv* env, const std::string& text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string ascii;
    ascii.reserve(text.size());
    for (unsigned char byte : text) {
        if ((byte >= 0x20 && byte <= 0x7E) || byte == '\n' || byte == '\r' || byte == '\t') {
            ascii.push_back(static_cast<char>(byte));
        } else {
            ascii += "\\x";
            ascii.push_back(hex[byte >> 4]);
            ascii.push_back(hex[byte & 0x0F]);
        }
    }
    return env->NewStringUTF(ascii.c_str());
}

jstring exceptionReport(JNIEnv* env, const char* message) noexcept {
    try {
        return asciiString(env, std::string("FAIL\nNative exception: ") + message + "\n");
    } catch (...) {
        return env->NewStringUTF("FAIL\nNative exception; unable to format details.\n");
    }
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_io_valdiroutes_tgfxprobe_ProbeActivity_runChecks(JNIEnv* env, jclass) {
    try {
        const VectorTestReport report = ::runVectorChecks();
        const bool passed = report.success && report.passed == 16;
        return asciiString(env, std::string(passed ? "PASS 16/16\n" : "FAIL\n") + report.details);
    } catch (const std::exception& failure) {
        return exceptionReport(env, failure.what());
    } catch (...) {
        return env->NewStringUTF("FAIL\nUnknown native exception.\n");
    }
}
