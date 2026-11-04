# Full backend mobile check shells

This directory contains independent UIKit and Java Activity shells for the
real production SnapDrawing/TGFX effects, media, text, and GPU lifetime checks.
It does not start a Valdi application or JavaScript runtime. Initial shell
preparation was source-only; subsequent parent mobile execution is recorded below.

Both identities are `io.valdiroutes.tgfxfullprobe`; the historical
`../../mobile` shells and `io.valdiroutes.tgfxprobe` 16-check artifacts are
independent. No original mobile source is included in these targets.

## Parent-owned native inputs

The parent owns `full/BUILD.bazel`, native configuration and the only native build
slot. Its native target must compile this directory's `full_checks.cpp` together
with the real `full/tests/{effects,media,text,gpu_error}_checks.cpp`, and link the actual
complete SnapDrawing backend, Valdi dependencies, pinned TGFX, ThorVG and codec,
font, HarfBuzz and ICU dependencies. Compile with both
`SNAP_DRAWING_TGFX_EXPERIMENT` and `SNAP_DRAWING_TGFX_COMPLETE`. Do not include the
CLI runner's `main()` or another `JNI_OnLoad` in an app library.

`runFullChecks(mediaDirectory, textDirectory)` returns a `FullChecksReport`:
`success`, actual native `passed`/`failed`, `registered`, `runnerFailures`, `status`
and `details`. The wrapper calls all four real suites even when an earlier
suite fails. Each must report `success == true`, `failed == 0`, a nonempty
`checks` vector, `passed > 0`, `passed == checks.size()`, and every individual
check must pass. Counts come from each actual report, with no fixed legacy total.
Counts are never fabricated for an exception or unavailable suite; those add a
separate runner failure and prevent PASS. Details include every native check's
name/outcome/diagnostic and the original report details.

Text and GPU errors are wired directly through their actual report contracts.
The parent binds the final fixture hash, per-suite counts, and each artifact's
actual results. Missing symbols fail linkage; no disabled-suite or
success-on-skip path exists. The parent can define
`TGFX_FULL_MOBILE_FIXTURE_HASH` as a quoted string when compiling the wrapper;
the report then includes that attestation. Without it, the report explicitly says
fixture parity is unverified. The hash is supplied by the parent, not computed or
validated by this shell. A local suite PASS alone does not attest cross-host parity.

`appendFullChecksSuite()` in `full_checks.hpp` validates each real report. The optional
third `runFullChecks()` argument, `AdditionalChecks`, accepts a parent adapter
`void(FullChecksReport&, const std::string& mediaDirectory, const std::string& textDirectory)`;
that adapter receives the actual copied fixture paths and can call the helper
with a lambda invoking another real suite. The four current suites are
unconditional and do not use this optional extension.

## iOS archive and app resources

`CMakeLists.txt` is an **iOS app shell only**: it compiles `ios/main.mm` and imports
an explicit absolute `TGFX_FULL_CHECKS_ARCHIVE`. Supply a parent-built merged
static archive for the selected SDK/architecture containing the wrapper, all
four suites, real backend objects and their native dependencies, or supply its
remaining static link closure in the semicolon-separated
`TGFX_FULL_LINK_LIBRARIES` cache value. No backend translation unit or algorithm
is compiled by CMake. Direct suite references retain the native checks, and
`-ObjC` retains Objective-C categories. Do not force-load duplicate transitive
dependency implementations or also supply component archives already merged.

Select the appropriate SDK/architecture through the parent's CMake/Xcode
configuration. This shell sets an iOS 15.0 deployment target, bundles the
`TGFXFullProbe` executable, and disables signing by default. A real-device
artifact needs the parent's matching device archive and signing configuration;
an arm64 simulator archive cannot be used as a device archive.

`TGFX_FULL_MEDIA_DATA` and `TGFX_FULL_TEXT_DATA` default to the actual sibling
`full/tests/media-data` and `text-data` trees. They are packaged, preserving
filenames, at:

```text
TGFXFullProbe.app/fixtures/media-data/{manifest.json,pattern.png,...}
TGFXFullProbe.app/fixtures/text-data/{RobotoFlex.ttf,receipt.json,...}
```

`NSBundle.mainBundle.resourceURL/fixtures` is copied to a fresh writable cache
directory on the worker; `runMediaChecks()` receives its **media-data directory**
and `runTextChecks()` its **text-data directory**, never the app/repository root.
The temporary copy is removed after the synchronous suites return. Missing
resources/copy failures display FAIL. The UIKit app starts once after foreground
entry, defers one main-loop turn for native lifecycle observers, and dispatches
checks to a background worker. Main-thread labels display the returned status
and full details without converting failures to green.

Expected app: parent-selected build output's `TGFXFullProbe.app`.
Accessible identifiers: `tgfx-full-status`, `tgfx-full-results`.

## Android native artifact and packaging

The parent builds a shared library for **arm64-v8a**, with this directory's
`android/probe.cpp` as its JNI owner and the shared wrapper/suites/backend linked
in. Required packaging inputs:

```text
android/.local/jniLibs/arm64-v8a/libsnap_tgfx_full_probe.so
android/.local/jniLibs/arm64-v8a/libc++_shared.so
```

Use the same NDK runtime as the parent native build. Any additional non-system
shared dependency from that library's dynamic dependency list must also be
packaged here. The parent must confirm the ELF architecture, minimum API, JNI
exports, dependency closure and runtime compatibility. Gradle performs no
externalNativeBuild/CMake/NDK compilation.

The Java-only app uses cached AGP **9.4.1**, compile/target SDK **36**, build tools
**36.0.0**, Java **17**, min SDK **24**, `android.builtInKotlin=false`, and only the
arm64 ABI. It needs the parent's compatible cached Gradle distribution/SDK and
AndroidX artifacts. No wrapper downloader or dependency installation is added.
Expected debug APK output directory after the parent's build:
`android/.local/build/outputs/apk/debug/`. The parent must inspect the generated
APK filename and package identity rather than reuse an old probe APK.

The fixture Sync task packages real sibling fixture trees as
`assets/fixtures/{media-data,text-data}` and checks that the manifest and fonts
exist. Generator `.py` files are excluded; malformed media fixtures are retained.
Before JNI checks, Java copies those assets recursively to ordinary files under
`getFilesDir()/tgfx-full-fixtures`, replacing the previous copy. A copy failure
produces a typed startup failure. No fixture is generated or replaced with
synthetic data.

`FullProbeActivity` keeps one `CheckRun` across configuration changes using
`onRetainNonConfigurationInstance`. The background worker retains application
context; its Activity attachment and result updates stay on the main thread.
Rotation reattaches to the running/completed result and does not repeat JNI.
Process death starts a fresh run. Activity destruction detaches the old view.

The JNI return type is the real Java `NativeResult`, with the same success,
counts and details as C++. The framework utility owns `JNI_OnLoad`/`JNI_OnUnload`.
The APK packages the actual Djinni Java support sources needed by that loader,
plus `javax.inject:javax.inject:1`; the desktop-only Djinni loader is excluded.
At the first Java native call, `std::call_once` obtains the real VM and invokes
TGFX's public `tgfx::JNIEnvironment::SetJavaVM(vm)`. It preflights ExifInterface
and the three methods used by `NativeCodec`, records initialization failures,
and keeps typed reporting available. Report class/constructor references are
local to the call. Java catches library-load and pending JNI exceptions.
No ABI declaration stubs or replacement TGFX runtime are supplied.

The inspected pinned TGFX source at
`/Users/cloud/project/ValdiNativeRoutes/.tools/vendor/tgfx`, commit
`4cf8991d5a3a010c9314538dd2fea937593b206b`, has **no**
`android/libtgfx` helper-source directory. Its native JNI init uses Android
framework classes plus `androidx.exifinterface:exifinterface:1.3.3`, included as a
real dependency. The Kotlin/AndroidX demo app is not needed. For a different
parent-supplied revision that has actual Java helper sources, pass
`-PtgfxAndroidJavaDir=/absolute/path/to/tgfx/android/libtgfx/src/main/java`;
that source root is added only when explicitly provided. Do not point it to
the demo Activity tree or use it to duplicate an already packaged helper jar.
Kotlin helper sources would require additional parent packaging; this shell is
Java-only.

TGFX's internal `JNIInit::Run()` clears some internal exceptions and exposes no
aggregate initialization report. This shell records observable VM/class/method
and pending-exception failures; the real GPU/media/text checks remain the
functional gate for internal native initialization. It does not claim to
observe every internal TGFX initialization error.

Android resource IDs are `tgfx_full_status` and `tgfx_full_results` (hyphens are
not valid Android resource names), with exact accessibility content descriptions
`tgfx-full-status` and `tgfx-full-results`. Full returned native details remain
selectable in the UI. `TGFX_FULL_RESULT` log messages supplement the UI; Android
logs contain only a short status/count summary and the report-file location.

## Complete report retrieval

Both shells finish writing an app-owned **`full-results.txt` before posting the
final UI result**, including caught startup, fixture-copy, library/JNI and check
exceptions. Retrieve this file through app filesystem access when available:

- iOS: `<app data container>/Documents/full-results.txt`.
- Android: `getFilesDir()/full-results.txt`, normally
  `/data/user/0/io.valdiroutes.tgfxfullprobe/files/full-results.txt` for user 0.

The file starts with `status`, `success`, actual `passed`/`failed`/`registered`
counts, `runnerFailures` and `completedUnixMs`, followed by the delimiter
`--- report.details (verbatim) ---` and the complete actual report with no
truncation. iOS appends the original native report bytes directly, including
embedded NULs or invalid UTF-8; Android writes the complete JNI-returned Java
String as UTF-8 (the existing JNI bridge escapes non-ASCII native bytes).
Caught Java and Objective-C exceptions include their available stack traces.
Native C++ exceptions retain their available `what()` diagnostic.

iOS uses an atomic Foundation write. Android writes and syncs a temporary file
in `filesDir`, closes it, then atomically renames it over `full-results.txt`.
Neither report depends on a full accessibility-tree dump or a giant log message.
The final UI includes the saved file's path.

A write failure preserves the original case counts/details, adds a runner
failure and forces **`FAIL report persistence`**. Each shell makes one bounded
attempt to persist that explicit failure report. A successful retry remains
FAIL; if both attempts fail, the UI/log pointer says **REPORT NOT SAVED**.
Atomic replacement may leave an older completed file after a failed write, so
match `completedUnixMs` and the current UI's saved/not-saved state when retrieving
an artifact. The file is overwritten on the next completed write.

## Validation boundary

The initial source-preparation handoff established source inspection and
whitespace checks only. Parent-owned native validation is recorded below.
Complete Valdi startup, JavaScript execution, whole-screen presentation,
physical devices, HarmonyOS, and production app integration remain separate
acceptance gates.

## Parent iOS simulator validation, November 4, 2026

The arm64 iOS 26.5 simulator executed all four suites successfully:

| Suite | Passed / registered |
| --- | --- |
| Effects and composition | 97 / 97 |
| Media, including SVG and Lottie | 99 / 99 |
| Text and variable fonts | 32 / 32 |
| GPU errors and asynchronous lifetime | 32 / 32 |
| Total | 260 / 260 |

The four-step recorded and polished `tgfx-full-ios-run2` E2E replay also
passed with no failed, errored, skipped, or warning steps. Both executions
persisted complete app-owned reports before the terminal UI result.

Evidence is under `/tmp/valdi-tgfx-full-20261104/`:

- `ios-full-native-run2.txt` and `ios-full-native-run2-replay.txt`.
- `ios-full-native-run2-acceptance.json`, binding counts, artifact receipt,
  report hash, and all 72 packaged fixture/resource hashes.
- `mobile-fixture-identity-final2.json`, binding all 82 fixture and runner
  inputs to `ccf78299903855ba8a2603893fa2d090b7a03920b6803551d936525168ac17a2`.
- `ios-full-native-build-attempt6.json` and
  `ios-full-probe-shell-build4.json` for successful native compilation/linking.

The TGFX vendor diff is
`5d7fbd0ef0b6f9fdb6735c0f8adf65618e6825fd28a0ef29795e25976c6d3067`.
The matching simulator archive hash is
`c880f90024d25025c6a1b55f9cb5c8e17bdf6230c88033b31ae2182f124cebaa`.
It disables unsupported simulator framebuffer fetch and uses the existing
destination-copy blend path. It also corrects image AA UV padding when source
and destination rectangles use different units. DrawingContext explicitly
preserves the former default nearest/no-mipmap sampling.

The shell references every suite directly and links the merged static archive
normally, with `-ObjC` for Objective-C categories. Force-loading the entire
transitive archive is unnecessary and pulled in duplicate zlib implementations.
Lottie support remains enabled. This is native-suite evidence, not a Valdi or
Hermes application-startup result or a performance comparison.

## Parent Android emulator validation, November 4, 2026

The local ARM64 API 36 emulator executed the same **260/260** cases, with
97 effects, 99 media, 32 text, and 32 GPU checks. The four-step
`tgfx-full-android-run1` E2E replay passed with no failures, errors, skips,
or warnings. The terminal UI selector includes its accessibility description:
`tgfx-full-status PASS 260/260`.

`android-full-native-run1-acceptance.json` binds the complete replay report,
fixture identity, and installed APK hash. `android-full-probe-apk-audit.json`
verifies actual DEX class definitions, the two arm64 ELF libraries and their
system/shared-library closure, JNI exports, and every packaged fixture hash.
The installed APK matches SHA256
`a0d2402fc3859aa7c1ceb4714bce6618da98e54901bb823e1f1fa19d348575a3`.

The strict native link passed with `--no-undefined`. The Android TGFX archive
is `337cf59c294fffce861f39e9e93c94cc9d04255f585bb1aaf30197cc0e60bf05`,
built from vendor diff
`7c83628e88ef61216a4daa74400cf07e6ef9cebc9fc71a1f1deca564ff517f7b`.
Its opt-in `TGFX_ANDROID_ENABLE_RTTI=ON` emits real interface type information
required by production `ImageCodec` subclasses. The default vendor Android
configuration still disables RTTI; preparation opts in and import validation
requires it. Exception settings remain unchanged.

The first offline Gradle attempt lacked ExifInterface 1.3.3. The next build
resolved that fixed dependency from configured repositories and succeeded.
Gradle retained the unstripped libraries, so this debug APK is not an
application-size comparison. Neither mobile shell proves Hermes/TSX startup
or whole-screen application presentation.
