//! HTTP-only extraction of NSH's Java class-loader and cached-method bootstrap.
use jni::{
    JNIEnv, JavaVM,
    objects::{GlobalRef, JClass, JStaticMethodID},
};
use std::sync::OnceLock;

pub(crate) struct Java {
    vm: JavaVM,
    pub(crate) net: GlobalRef,
    pub(crate) net_fetch: JStaticMethodID,
    pub(crate) net_send: JStaticMethodID,
    pub(crate) net_finish: JStaticMethodID,
    pub(crate) net_pause: JStaticMethodID,
    pub(crate) net_cancel: JStaticMethodID,
}
static JAVA: OnceLock<Java> = OnceLock::new();
pub(crate) fn initialized() -> bool {
    JAVA.get().is_some()
}
pub(crate) fn java() -> &'static Java {
    JAVA.get()
        .expect("initialize valdi-nsh-transport from the app class loader first")
}

/// Initializes only the HTTP bridge on the calling app-class-loader thread.
/// Call from the parent's JNI_OnLoad with its JNIEnv, before network().fetch().
/// Repeated initialization is a no-op. No JNI_OnLoad is exported by this crate.
pub fn init_android(env: &mut JNIEnv<'_>) -> jni::errors::Result<()> {
    if initialized() {
        return Ok(());
    }
    let vm = env.get_java_vm()?;
    let class = env.find_class("com/snap/valdi/nsh/Net")?;
    let net = env.new_global_ref(class)?;
    let class: &JClass<'_> = net.as_obj().into();
    let java = Java {
        net_fetch: env.get_static_method_id(
            class,
            "fetch",
            "(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BZZ)V",
        )?,
        net_send: env.get_static_method_id(class, "send", "(J[B)V")?,
        net_finish: env.get_static_method_id(class, "finish", "(J)V")?,
        net_pause: env.get_static_method_id(class, "pause", "(JZ)V")?,
        net_cancel: env.get_static_method_id(class, "cancel", "(J)V")?,
        net,
        vm,
    };
    let _ = JAVA.set(java);
    Ok(())
}

/// C bootstrap: 0 success, -1 invalid env or JNI failure. Clears a pending Java
/// exception on failure so the parent can report its own initialization error.
///
/// # Safety
/// env must be the current thread's live JNI environment, from JNI_OnLoad or
/// an app Java native call; it must resolve com.snap.valdi.nsh.Net.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn valdi_nsh_transport_init_android(env: *mut jni::sys::JNIEnv) -> i32 {
    // SAFETY: caller supplies its current valid JNIEnv, as documented above.
    let Ok(mut env) = (unsafe { JNIEnv::from_raw(env) }) else {
        return -1;
    };
    match init_android(&mut env) {
        Ok(()) => 0,
        Err(_) => {
            let _ = env.exception_clear();
            -1
        }
    }
}
pub(crate) fn with_env<R>(
    f: impl FnOnce(&mut JNIEnv<'_>) -> jni::errors::Result<R>,
) -> jni::errors::Result<R> {
    let mut env = java().vm.attach_current_thread_permanently()?;
    let result = env.with_local_frame(32, f);
    if result.is_err() {
        let _ = env.exception_clear();
    }
    result
}
