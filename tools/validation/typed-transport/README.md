# Typed transport regression consumers

Run from this fork's repository root:

```sh
node --test --test-concurrency=1 tools/validation/typed-transport/*.test.mjs
```

The drivers read production sources from this checkout. Negative controls read
the baseline object `15feb6530ceecb971f4c8519ea8ab77474b959db` with `git show`;
that object must be available locally. No patches are applied. No application
checkout, application schemas, network downloads, Bazel invocation, or device
is required for the default source/Kotlin checks. All compilation outputs and
baseline copies live in automatically removed temporary directories. These
fixtures live outside `valdi/test` so their `main()` functions cannot enter its
production test glob.

## Default checks

`source-contracts.test.mjs` checks the two current map call sites, captured and
released JavaScript intrinsics, absence of source evaluation in initialization
and the map helper, and Foundation's `CFBoolean` discriminator. These are source
contracts, not runtime evidence.

`android-promise-rejection.test.mjs` compiles the actual Kotlin Promise classes,
`CppPromiseCallback`, and `PromiseRejectionException` from this checkout. It
runs the preserved 19-case consumer on the JVM with `-Xcheck:jni` and a real
`RegisterNatives` capture library. Replacing only the callback with its baseline
source must reproduce exactly nine verbatim-message failures. Restoring the
current source must pass all 19 cases. Cases cover empty/whitespace rejection
text, ordinary exceptions and cause chains, error codes, cancellation, pending
and already settled promises, native-handle transfer, and Java constructors.
The emitted JNI descriptor is also compared with this checkout's actual
`CppPromiseCallbackJNI.hpp` contract.

Requirements: Node.js with `node:test`, `git`, `rg`, a host C compiler, JDK 17 or
newer, an existing Kotlin compiler distribution, and AndroidX annotation jar.
The driver discovers Kotlin and AndroidX in the user's existing Gradle caches;
it never starts Gradle or installs anything. To use other existing inputs:

```sh
VALDI_TEST_KOTLIN_LIB=/absolute/path/to/gradle/lib \
VALDI_TEST_ANDROIDX_ANNOTATION=/absolute/path/to/annotation-jvm.jar \
node --test tools/validation/typed-transport/android-promise-rejection.test.mjs
```

Missing Kotlin/JDK/compiler inputs fail the Kotlin test with a prerequisite
error. To run only the source contracts:

```sh
node --test tools/validation/typed-transport/source-contracts.test.mjs
```

The JNI library observes the real callback's native call. It does not link the
Android runtime or prove native-to-JavaScript Promise delivery on a device.
The Java disposal endpoint intentionally throws if the callback fails to
transfer its handles.

## Optional linked host consumers

`runtime.test.mjs` skips unless running on macOS arm64 with all four explicit
inputs below. Set inputs to compatible, already built framework/Hermes caches:

```sh
VALDI_NATIVE_TEST_HOST=/absolute/path/to/host \
VALDI_NATIVE_TEST_EXECROOT=/absolute/path/to/bazel/execroot/_main \
VALDI_NATIVE_TEST_HERMES_SOURCE=/absolute/path/to/prepared/hermes \
VALDI_NATIVE_TEST_HERMESC=/absolute/path/to/matching/hermesc \
node --test --test-concurrency=1 tools/validation/typed-transport/runtime.test.mjs
```

The input layout follows the existing host fixtures:

- `HOST/compile_commands.json` contains the real `Runtime.cpp` compiler command.
  The driver remaps cached framework include paths to this fork. Generated and
  external dependency headers remain read-only build inputs.
- `HOST/libreal_valdi_host.a`, `HOST/deps/fmt/libfmt.a`, and
  `HOST/deps/jsoncpp/src/lib_json/libjsoncpp.a` support the Foundation consumer.
- `EXECROOT/bazel-out/darwin_arm64-fastbuild/bin/external/valdi+/valdi/`
  contains `test_hermes_boundary-0.params` and `test_hermes_jsi_lean-0.params`,
  along with their referenced archives/objects and generated Hermes headers.
  If an archive was removed but its archive response file and object files
  remain, the driver reconstructs only a temporary archive.
- `HERMES_SOURCE` provides the prepared Hermes headers corresponding to those
  archives. `HERMESC` must emit compatible HBC bytecode.

An absent input causes an explicit skip; supplied invalid or incompatible
inputs fail. These drivers never rebuild the caches or write into them.

The map test compiles this checkout's `IJavaScriptContext.cpp`,
`JavaScriptUtils.cpp`, `JavaScriptValueDelegate.cpp`, and real derived
`HermesJavaScriptContext.cpp` (plus the full engine's compiler unit), and links
them before cached dependency archives. Both full and LEAN Hermes execute
precompiled bytecode using the unchanged C++ consumer. It exercises untyped
and delegate conversions, nested maps and arrays, `__proto__`, `constructor`,
`prototype`, null/boolean/number/string leaves, Unicode/NUL text, descriptors,
mutation/deletion, prototype safety, replaced global intrinsics, and inherited
descriptor getter/setter pollution. Independently reverting either map call
site to its baseline source must fail the LEAN consumer.

The Foundation test compiles the full current production
`SCValdiObjCConversionUtils.mm` against the real framework headers. Its
executable uses the unchanged consumer with the NSNumber branch extracted
verbatim from the current production file. `CFBoolean` stays boolean while
numeric `NSNumber`/`CFNumber`, including zero and one, stay numeric. The baseline
production branch must fail on `CFBoolean`. This is a linked branch consumer
and a complete-unit compilation check; it does not link and exercise the
entire Objective-C bridge.

These are bounded host consumers using existing dependency artifacts. They do
not export a runtime AAR/XCFramework, run the complete framework test suite, or
establish Android/iOS device acceptance. No Harmony target is built or tested.

## Fixture provenance

The following five files were copied byte-for-byte from the consuming
application's historical commit `dd802fe1`, under `scripts/tests/`. That
history is provenance only; the test drivers do not access that repository.
Driver code was adapted to use current fork production sources and baseline
`git show`, removing patch registration/application and application build
checks. No handwritten engine ABI is used.

```text
c5fa10beeeb8d1957b7cb5e83641dc6af93382bef2fa86b4247e8547feb19bdf  fixtures/android-promise-rejection/NativeBridge.java
9a089e080f1063d09789225840ef9bf57be3048e26caa55a6f571d068e23b7ae  fixtures/android-promise-rejection/PromiseRejectionConsumer.kt
9504fe9eb740130a69baa6f2917363c8f424d36789f5c72854be6b7ca75ea6e6  fixtures/android-promise-rejection/capture.c
445b45f8bde8b0cff0c34532299c78daa8f81987b97045fdc4ba74739298ee86  fixtures/foundation-number-consumer.mm
67e3145f3d18460005c5806e7c170a6e0391549a4074a29f1e3465ea99f3d8de  fixtures/lossless-map-consumer.cpp
```
