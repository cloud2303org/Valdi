#include "../render_performance.hpp"
#include "utils/platform/JvmUtils.hpp"

#include <exception>
#include <jni.h>
#include <mutex>
#include <string>

#ifndef ANDROID_WITH_JNI
#error "The Android host requires the real utility JNI lifecycle; do not build with jni_disabled=true"
#endif

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include "tgfx/platform/android/JNIEnvironment.h"
#endif

namespace {

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
std::once_flag initializationOnce;
std::string initializationError;

void initializeTGFX(JNIEnv* env) {
    // utils/platform/JvmUtils.cpp owns the only JNI_OnLoad in this closure.
    // Use a Java-entered call so TGFX sees this app's real class loader.
    std::call_once(initializationOnce, [env] {
        jclass exif = env->FindClass("androidx/exifinterface/media/ExifInterface");
        if (!exif || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) { env->ExceptionClear(); }
            if (exif) { env->DeleteLocalRef(exif); }
            initializationError = "Missing actual ExifInterface Java support";
            return;
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
        bool valid = true;
        for (const auto& method : methods) {
            auto id = env->GetMethodID(exif, method.name, method.signature);
            if (!id || env->ExceptionCheck()) { valid = false; }
            if (env->ExceptionCheck()) { env->ExceptionClear(); }
        }
        env->DeleteLocalRef(exif);
        if (!valid) { initializationError = "Missing required ExifInterface methods"; return; }
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK || !vm || !tgfx::JNIEnvironment::SetJavaVM(vm)) {
            initializationError = "Actual TGFX JavaVM initialization failed";
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            initializationError = "TGFX JavaVM initialization raised a JNI exception";
        }
    });
}
#endif

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_io_valdiroutes_renderperformance_BenchmarkActivity_runNative(JNIEnv* env, jclass, jstring directory) {
    using namespace snap::drawing::performance;
    try {
        // This real reference retains JvmUtils.cpp and its sole JNI_OnLoad in
        // the closure, even when the rest of the drawing core needs no Java.
        if (!snap::utils::platform::getJvm() || snap::utils::platform::getEnv() != env) {
            return env->NewStringUTF(renderPerformanceFailure("Framework JNI lifecycle is not initialized").c_str());
        }
        if (!directory) {
            return env->NewStringUTF(renderPerformanceFailure("Fixture directory is null").c_str());
        }
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        initializeTGFX(env);
        if (!initializationError.empty()) {
            return env->NewStringUTF(renderPerformanceFailure(initializationError).c_str());
        }
#endif
        const char* raw = env->GetStringUTFChars(directory, nullptr);
        if (!raw) { return nullptr; } // Preserve the VM allocation exception.
        std::string path;
        try {
            path = raw;
        } catch (...) {
            env->ReleaseStringUTFChars(directory, raw);
            throw;
        }
        env->ReleaseStringUTFChars(directory, raw);
        const auto report = runRenderPerformance(path);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return env->NewStringUTF(renderPerformanceFailure("Native benchmark left a JNI exception").c_str());
        }
        // The core JSON serializer emits ASCII only.
        return env->NewStringUTF(report.c_str());
    } catch (const std::exception& error) {
        if (env->ExceptionCheck()) { return nullptr; }
        // Do not allow C++ failures to unwind across a Java native call.
        jclass type = env->FindClass("java/lang/IllegalStateException");
        if (type) {
            env->ThrowNew(type, error.what());
            env->DeleteLocalRef(type);
        }
        return nullptr;
    } catch (...) {
        if (!env->ExceptionCheck()) {
            jclass type = env->FindClass("java/lang/IllegalStateException");
            if (type) {
                env->ThrowNew(type, "Unknown native render benchmark failure");
                env->DeleteLocalRef(type);
            }
        }
        return nullptr;
    }
}
