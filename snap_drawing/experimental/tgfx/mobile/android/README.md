# Standalone Android TGFX vector probe

This Java-only shell runs the real shared vector checks once on a background
thread when the launcher Activity opens. Activity configuration changes retain
the running check and its result. A fresh Activity after finishing the previous
one, or after process death, starts a fresh run.

The result is a selectable, scrolling TextView with resource identifier
`io.valdiroutes.tgfxprobe:id/probe_results`. Its parent has identifier
`io.valdiroutes.tgfxprobe:id/probe_scroll`. While the check runs it shows
`RUNNING`. JNI prepends `PASS 16/16` only when `report.success` is true and
`report.passed == 16`; every other report starts with `FAIL`. The Activity
displays the returned text verbatim and does not parse the details for status.
Java library-loading failures and caught native exceptions also display `FAIL`.
Native aborts/signals cannot be converted into a Java report by this shell.

## Native integration owned by the parent experiment

`probe.cpp` includes `../../vector_checks.hpp` directly. The parent provides
the actual global `VectorTestReport` and `runVectorChecks()` implementation in
the TGFX experiment root. This directory supplies no replacement ABI declarations
or check implementation.

The parent's `mobile/CMakeLists.txt` should compile `android/probe.cpp` into the
`snap_tgfx_probe` shared library and link the real shared vector checks, TGFX,
and required Android native libraries. The exported JNI entry point corresponds
to `public static native String ProbeActivity.runChecks()`. C++ exceptions must
be enabled. JNI retains ASCII details and escapes other bytes as `\xHH`,
including NUL, before calling `NewStringUTF`.

Gradle consumes prebuilt libraries from `.local/jniLibs/arm64-v8a/`. It has no
native build configuration. Stage `libsnap_tgfx_probe.so` and every non-system
shared dependency there. If the parent uses `c++_shared`, include
`libc++_shared.so` from the same NDK used for that native build. The parent
should inspect native dependencies before packaging.

## Packaging recipe for the parent's later serial build

The existing executable wrapper at
`/Users/cloud/project/ValdiNativeRoutes/android/gradlew` selects Gradle 9.6.0.
This standalone project selects cached AGP 9.4.1, disables built-in Kotlin,
uses SDK/build-tools 36, Java 17, minimum API 24, and only `arm64-v8a`.
It has no application dependencies and requests no Android permissions.

After the parent has built the real native target, set
`TGFX_PROBE_NATIVE_LIBRARY` to the absolute path of its `libsnap_tgfx_probe.so`:

```sh
PROBE_ANDROID=/Users/cloud/.config/superpowers/worktrees/Valdi-node-only/tgfx-prototype-20261104/snap_drawing/experimental/tgfx/mobile/android
: "${TGFX_PROBE_NATIVE_LIBRARY:?Set to the parent-built libsnap_tgfx_probe.so}"
mkdir -p "$PROBE_ANDROID/.local/jniLibs/arm64-v8a"
cp "$TGFX_PROBE_NATIVE_LIBRARY" "$PROBE_ANDROID/.local/jniLibs/arm64-v8a/libsnap_tgfx_probe.so"
```

If the native target depends on the shared C++ runtime, also set
`TGFX_PROBE_CXX_RUNTIME` to that build's `libc++_shared.so` and stage it:

```sh
: "${TGFX_PROBE_CXX_RUNTIME:?Set to the matching NDK libc++_shared.so}"
cp "$TGFX_PROBE_CXX_RUNTIME" "$PROBE_ANDROID/.local/jniLibs/arm64-v8a/libc++_shared.so"
```

Stage any additional non-system native shared dependencies in the same ABI
directory, then package using the existing wrapper:

```sh
JAVA_HOME=/Users/cloud/Library/Java/JavaVirtualMachines/azul-17.0.19/Contents/Home \
ANDROID_HOME=/Users/cloud/Library/Android/sdk \
/Users/cloud/project/ValdiNativeRoutes/android/gradlew \
  --project-dir "$PROBE_ANDROID" \
  --project-cache-dir "$PROBE_ANDROID/.local/gradle-cache" \
  --offline --no-daemon assembleDebug
```

The APK will be at `.local/build/outputs/apk/debug/SnapTgfxProbe-debug.apk`.
Build outputs, staged libraries, and the project cache stay under ignored
`.local/`. No separate wrapper or local SDK configuration file is required.
The pre-build guard rejects a missing probe library.

## Preparation evidence and limits

The source-preparation pass confirmed the existing wrapper is executable and
that AGP 9.4.1 artifacts, an unpacked Gradle 9.6.0 distribution, the JDK 17
directory, SDK 36 `android.jar`, and build-tools 36.0.0 `aapt2` exist locally.
It did not run Java, Gradle, CMake, a compiler, or a device. Complete offline
dependency resolution and toolchain compatibility remain for the parent's
build to verify. The real shared header, native library and any native runtime
dependencies must be supplied by the parent before packaging.

The later parent execution completed strict native linking and offline APK
packaging, then ran 16/16 checks on the local arm64 Android API 36 emulator.
Its five-step recorded restart/report flow passed without warnings. The first
packaging attempt lacked dependency-resolution repositories; adding Google
and Maven Central to `settings.gradle` resolved that offline cached-tool
lookup. Exact artifacts and preserved attempts are recorded in
`../../evidence/mobile-results.json`.
