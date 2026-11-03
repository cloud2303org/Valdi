#include <jni.h>

// A capture endpoint, not the Android Valdi native runtime. RegisterNatives
// validates the real compiled callback's JNI names and descriptors on the JVM.
static jstring captured_message;
static jlong captured_handles[2];
static jint captured_code, failure_count, success_count;

static void reset(JNIEnv *env, jclass cls) {
    if (captured_message) (*env)->DeleteGlobalRef(env, captured_message);
    captured_message = NULL;
    captured_handles[0] = captured_handles[1] = 0;
    captured_code = failure_count = success_count = 0;
}

static void failure(JNIEnv *env, jclass cls, jlong handle, jlong marshaller,
                    jstring message, jint code) {
    if (captured_message) (*env)->DeleteGlobalRef(env, captured_message);
    captured_message = (*env)->NewGlobalRef(env, message);
    captured_handles[0] = handle;
    captured_handles[1] = marshaller;
    captured_code = code;
    failure_count++;
}

static void success(JNIEnv *env, jclass cls, jlong handle, jlong marshaller, jobject value) {
    success_count++;
}

static jstring message(JNIEnv *env, jclass cls) { return captured_message; }
static jint error_code(JNIEnv *env, jclass cls) { return captured_code; }
static jint failures(JNIEnv *env, jclass cls) { return failure_count; }
static jint successes(JNIEnv *env, jclass cls) { return success_count; }
static jlongArray handles(JNIEnv *env, jclass cls) {
    jlongArray result = (*env)->NewLongArray(env, 2);
    (*env)->SetLongArrayRegion(env, result, 0, 2, captured_handles);
    return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass callback = (*env)->FindClass(env, "com/snap/valdi/promise/CppPromiseCallback");
    if (!callback) return JNI_ERR;
    JNINativeMethod callback_methods[] = {
        {"nativeOnFailure", "(JJLjava/lang/String;I)V", (void *)failure},
        {"nativeOnSuccess", "(JJLjava/lang/Object;)V", (void *)success},
    };
    if ((*env)->RegisterNatives(env, callback, callback_methods, 2) != JNI_OK) return JNI_ERR;
    jclass probe = (*env)->FindClass(env, "NativeProbe");
    if (!probe) return JNI_ERR;
    JNINativeMethod probe_methods[] = {
        {"reset", "()V", (void *)reset},
        {"message", "()Ljava/lang/String;", (void *)message},
        {"errorCode", "()I", (void *)error_code},
        {"handles", "()[J", (void *)handles},
        {"failureCount", "()I", (void *)failures},
        {"successCount", "()I", (void *)successes},
    };
    if ((*env)->RegisterNatives(env, probe, probe_methods, 6) != JNI_OK) return JNI_ERR;
    return JNI_VERSION_1_6;
}
