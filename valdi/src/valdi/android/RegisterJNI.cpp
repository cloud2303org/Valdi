#include "valdi/android/AttributedTextJNI.hpp"
#include "valdi/android/AttributesJNI.hpp"
#include "valdi/android/CppPromiseCallbackJNI.hpp"
#include "valdi/android/CppPromiseJNI.hpp"
#include "valdi/android/FunctionTrampolineJNI.hpp"
#include "valdi/android/InternedStringCPP.hpp"
#include "valdi/android/ModuleFactoryRegistryJNI.hpp"
#include "valdi/android/NativeBridge.hpp"
#include "valdi/android/ValdiFunctionNative.hpp"
#include "valdi/android/ValueMarshallerRegistryCppJNI.hpp"
#include "valdi/jni/JavaThread.hpp"
#ifdef VALDI_NATIVE_WEB
#include "libs/nsh_web_transport/include/valdi_nsh_transport_android.h"
#endif

#if SNAP_DRAWING_ENABLED
#include "valdi/android/AnimatedImageJNI.hpp"

#endif

djinni::JniClassInitializer valdiJNIOnLoad([] { // NOLINT(fuchsia-statically-constructed-objects)
    JavaVM* vm = nullptr;
    djinni::jniGetThreadEnv()->GetJavaVM(&vm);

    facebook::jni::initialize(vm, [] {
        ValdiAndroid::InternedStringCPP::registerNatives();
        ValdiAndroid::ValdiFunctionNative::registerNatives();
        ValdiAndroid::NativeBridge::registerNatives();
        ValdiAndroid::JavaThread::registerNatives();
        ValdiAndroid::AttributesJNI::registerNatives();
        ValdiAndroid::AttributedTextJNI::registerNatives();
        ValdiAndroid::ValueMarshallerRegistryCppJNI::registerNatives();
        ValdiAndroid::FunctionTrampolineJNI::registerNatives();
        ValdiAndroid::CppPromiseJNI::registerNatives();
        ValdiAndroid::CppPromiseCallbackJNI::registerNatives();
        ValdiAndroid::ModuleFactoryRegisterJNI::registerNatives();
#if SNAP_DRAWING_ENABLED
        if constexpr (snap::drawing::kLottieEnabled) {
            ValdiAndroid::AnimatedImageViewJNI::registerNatives();
            ValdiAndroid::AnimatedImageJNI::registerNatives();
        }
#endif
    });
#ifdef VALDI_NATIVE_WEB
    auto* env = djinni::jniGetThreadEnv();
    if (valdi_nsh_transport_init_android(env) != 0) {
        auto errorClass = env->FindClass("java/lang/IllegalStateException");
        if (errorClass != nullptr) {
            env->ThrowNew(errorClass, "Could not initialize the NSH network transport");
            env->DeleteLocalRef(errorClass);
        }
    }
#endif
});
