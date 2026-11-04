#include "../full_checks.hpp"

// This is TGFX's actual public header, supplied by the parent's native deps.
#include "tgfx/platform/android/JNIEnvironment.h"

#include <jni.h>

#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

std::once_flag initializationOnce;
std::string initializationFailure;
bool initializedVM = false;

// NewStringUTF takes modified UTF-8; escaping non-ASCII bytes preserves every
// native diagnostic byte without passing malformed UTF-8 or NUL into JNI.
jstring reportString(JNIEnv* env, const std::string& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string ascii;
    for (unsigned char byte : value) {
        if ((byte >= 0x20 && byte <= 0x7e) || byte == '\n' || byte == '\r' || byte == '\t') {
            ascii.push_back(static_cast<char>(byte));
        } else {
            ascii += "\\x";
            ascii.push_back(hex[byte >> 4]);
            ascii.push_back(hex[byte & 0x0f]);
        }
    }
    return env->NewStringUTF(ascii.c_str());
}

bool requireClass(JNIEnv* env, const char* name) {
    jclass value = env->FindClass(name);
    const bool found = value != nullptr && !env->ExceptionCheck();
    if (value != nullptr) {
        env->DeleteLocalRef(value);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (!found) {
        initializationFailure += std::string("Missing required TGFX Java support class: ") + name + "\n";
    }
    return found;
}

bool requireExifMethods(JNIEnv* env) {
    jclass type = env->FindClass("androidx/exifinterface/media/ExifInterface");
    if (type == nullptr) {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        initializationFailure += "Cannot resolve required ExifInterface support\n";
        return false;
    }
    struct Method {
        const char* name;
        const char* signature;
    };
    constexpr Method methods[] = {
        {"<init>", "(Ljava/lang/String;)V"},
        {"<init>", "(Ljava/io/InputStream;)V"},
        {"getAttributeInt", "(Ljava/lang/String;I)I"},
    };
    bool success = true;
    for (const auto& method : methods) {
        if (env->GetMethodID(type, method.name, method.signature) == nullptr || env->ExceptionCheck()) {
            success = false;
            initializationFailure += std::string("Missing ExifInterface method: ") +
                                     method.name + method.signature + "\n";
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
    }
    env->DeleteLocalRef(type);
    return success;
}

std::string directoryString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        throw std::runtime_error("Native check directory is null");
    }
    const char* bytes = env->GetStringUTFChars(value, nullptr);
    if (bytes == nullptr) {
        throw std::runtime_error("Cannot read native check directory");
    }
    std::string result;
    try {
        result = bytes;
    } catch (...) {
        env->ReleaseStringUTFChars(value, bytes);
        throw;
    }
    env->ReleaseStringUTFChars(value, bytes);
    if (result.empty()) {
        throw std::runtime_error("Native check directory is empty");
    }
    return result;
}

jobject javaReport(JNIEnv* env, const tgfx_full_mobile::FullChecksReport& report) {
    jclass resultClass = env->FindClass("io/valdiroutes/tgfxfullprobe/NativeResult");
    if (resultClass == nullptr) {
        return nullptr;
    }
    jmethodID resultConstructor = env->GetMethodID(resultClass, "<init>",
                                                   "(ZIIIILjava/lang/String;Ljava/lang/String;)V");
    if (resultConstructor == nullptr || env->ExceptionCheck()) {
        env->DeleteLocalRef(resultClass);
        return nullptr;
    }
    jstring status = reportString(env, report.status);
    if (status == nullptr) {
        env->DeleteLocalRef(resultClass);
        return nullptr; // Preserve the JVM exception for the Java startup handler.
    }
    jstring details = reportString(env, report.details);
    if (details == nullptr) {
        env->DeleteLocalRef(status);
        env->DeleteLocalRef(resultClass);
        return nullptr;
    }
    jobject result = env->NewObject(resultClass, resultConstructor,
                                    static_cast<jboolean>(report.success),
                                    static_cast<jint>(report.passed), static_cast<jint>(report.failed),
                                    static_cast<jint>(report.registered), static_cast<jint>(report.runnerFailures),
                                    status, details);
    env->DeleteLocalRef(status);
    env->DeleteLocalRef(details);
    env->DeleteLocalRef(resultClass);
    return result;
}

jobject failureReport(JNIEnv* env, const char* message) noexcept {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    try {
        return javaReport(env, tgfx_full_mobile::startupFailure(message));
    } catch (...) {
        // Last-resort reporting must not let a C++ allocation failure escape JNI.
        if (!env->ExceptionCheck()) {
            jclass type = env->FindClass("java/lang/IllegalStateException");
            if (type != nullptr) {
                env->ThrowNew(type, "Cannot format native full-check failure report");
                env->DeleteLocalRef(type);
            }
        }
        return nullptr;
    }
}

void initializeTGFX(JNIEnv* env) {
    // utils_core_cc owns JNI_OnLoad/JNI_OnUnload and initializes its real JVM
    // support first. Initialize TGFX from this Java call, on its class loader.
    std::call_once(initializationOnce, [env] {
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK || vm == nullptr) {
            initializationFailure += "Cannot obtain the application's real JavaVM\n";
            return;
        }
        // SetJavaVM invokes real TGFX JNI initialization. Preflight mandatory
        // application classes because TGFX JNIInit::Run clears its exceptions.
        if (!requireClass(env, "androidx/exifinterface/media/ExifInterface") || !requireExifMethods(env)) {
            return;
        }
        initializedVM = tgfx::JNIEnvironment::SetJavaVM(vm);
        if (!initializedVM) {
            initializationFailure += "tgfx::JNIEnvironment::SetJavaVM returned false\n";
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            initializationFailure += "TGFX JavaVM initialization raised a JNI exception\n";
        }
    });
}

} // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_io_valdiroutes_tgfxfullprobe_FullProbeActivity_runChecks(JNIEnv* env, jclass,
                                                           jstring fixturesDirectory, jstring textDirectory) {
    try {
        initializeTGFX(env);
        if (!initializedVM || !initializationFailure.empty()) {
            return javaReport(env, tgfx_full_mobile::startupFailure(
                "TGFX JNI initialization failed:\n" + initializationFailure));
        }
        const auto fixtures = directoryString(env, fixturesDirectory);
        const auto text = directoryString(env, textDirectory);
        auto report = tgfx_full_mobile::runFullChecks(fixtures, text);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            tgfx_full_mobile::recordRunnerFailure(report, "Native checks left a pending JNI exception");
            report.details = "FAIL JNI after native suites; original case results follow\n" + report.details;
        }
        return javaReport(env, report);
    } catch (const std::exception& error) {
        return failureReport(env, error.what());
    } catch (...) {
        return failureReport(env, "Unknown JNI wrapper exception");
    }
}
