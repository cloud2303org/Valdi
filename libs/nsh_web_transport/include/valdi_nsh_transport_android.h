#ifndef VALDI_NSH_TRANSPORT_ANDROID_H
#define VALDI_NSH_TRANSPORT_ANDROID_H
#include <jni.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Call from the parent JNI_OnLoad or a native call on the app class-loader
 * thread. 0 = success (including repeated init); -1 = JNI/init error.
 * This library intentionally exports no JNI_OnLoad. */
int32_t valdi_nsh_transport_init_android(JNIEnv *env);
#ifdef __cplusplus
}
#endif
#endif
