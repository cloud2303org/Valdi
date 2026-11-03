package com.snapchat.client.valdi;

/** Only the disposal endpoint needed by the production CppNativeHandlePair. */
public final class NativeBridge {
    public static void releaseNativeRef(long handle) {
        throw new AssertionError("Callback did not transfer native handle " + handle);
    }
}
