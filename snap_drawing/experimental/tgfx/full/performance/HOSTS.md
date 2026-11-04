# Native benchmark hosts (source handoff, 2026-11-04)

These hosts load the **same** `render_benchmark` and `drawing` archives in the
actual Valdi runtime. Android uses `ValdiRuntimeManager`, a custom module provider,
and `ValdiRootView` with `RenderBackend.ANDROID`. iOS uses
`SCValdiRuntimeManager`, `SCValdiCustomModuleProvider`, and the UIKit
`SCValdiComponentContainerView` pattern from `full/app`. Backend selection belongs
to the supplied native runtime, never to a different host source/build branch.
The neutral identifier on both platforms is `io.valdiroutes.renderbenchmark`.
Install/run backends sequentially and collect each result before replacing the
app or switching the stage symlink.

This handoff is source only. No native build, project generation, compiler,
signing, installation, device interaction, or performance measurement ran as
part of this implementation.

## Stage contract

The parent supplies a fresh backend/platform stage outside the source directories:

```text
STAGE/
  provenance.json
  runtime/
    valdi_runtime.aar                      # Android arm64-v8a
    ValdiRuntime.xcframework/              # iOS: actual device/simulator slices
  generated/
    android/render_benchmark.kt            # actual compiler-generated model
    ios/RenderBenchmarkTypes/RenderBenchmarkTypes.h
    ios/RenderBenchmarkTypes/...            # implementations emitted by codegen
    resources/android/...
    resources/ios/render_benchmark.bundle/...
  package/modules/
    drawing.valdimodule
    render_benchmark.valdimodule
    drawing.map.json                       # optional
    render_benchmark.map.json              # optional
  jniLibs/arm64-v8a/libc++_shared.so         # optional companion NDK dependency
  host-manifest.json                       # optional parent hash manifest
```

Only the current platform's runtime/generated/resources are required by its
validator. Keep the sibling compiler's full generated output, package, resources,
and compile manifests. A shared module compilation is deliberately
`runtimeBinding: "unbound"`: reuse those exact compiled module bytes for Skia and
TGFX. Its embedded compile provenance cannot attest either backend.

The actual `release-001` iOS resource output contains only
`render_benchmark.bundle`. The drawing runtime compilation emits its JS archive,
not a `drawing.bundle`; the iOS project therefore copies only the generated
benchmark resource bundle. Regenerate the Xcode project after this source change
so an earlier `drawing.bundle` copy phase is removed.

`STAGE/provenance.json` is supplied by the parent from the **actual external
runtime artifact and its build provenance**. Required fields:

```json
{
  "backend": "skia",
  "config": "Release",
  "runtimeSha256": "<64 lowercase hexadecimal characters>"
}
```

`backend` is `skia` or `tgfx`; `config` must be Release (case insensitive).
Preserve additional actual provenance fields, including source/build identity
and architecture. The host copies this object into its results verbatim.
Neither a runtime hash nor an unchecked backend label proves native execution;
the parent's export/build and actual host run establish that connection.
Do not create an old TGFX acceptance receipt to admit a Skia artifact.
No checker here writes receipts or modifies the stage.
Android requires `libvaldi_runtime.so` inside the hashed AAR and rejects a
separately overriding `jniLibs` copy. Record hashes for any companion NDK
dependencies in the optional manifest and native build provenance.

`android/check_stage.py` checks real files and generated context names and hashes
the supplied runtime before either native build. A file hash is SHA-256(bytes).
An XCFramework tree hash is SHA-256 of sorted relative-path entries, each encoded
as `path + NUL + file-SHA256 + LF`; symlink entries use `link:` plus the stored
link target instead of a file hash and are not followed. This is the
`full/app/bridge/artifact.mjs` tree convention, reproduced here without importing
the backend-specific receipt checker.

An optional `host-manifest.json` has this shape:

```json
{
  "sha256": {
    "generated": "<tree digest>",
    "package": "<tree digest>",
    "generated/resources": "<tree digest>"
  }
}
```

All supplied entries are checked; relative paths must remain inside the stage.
Prefer covering generated sources, module archives and resources. This manifest
is a parent-owned file-integrity input, not a fabricated runtime acceptance
receipt. If `package/provenance.json` is also present it must match the root
object. Android exports the root object as `assets/provenance.json`; iOS bundles
it at the app resource root.

## Generated native contract and reports

The component path is `BenchmarkApp@render_benchmark/src/Benchmark`.
The compiler emits Kotlin package `com.snap.modules.render_benchmark` with
`BenchmarkContext(report = ..., nowMs = ...)` and Objective-C
`RenderBenchmarkBenchmarkContext`, in `RenderBenchmarkTypes`, with generated
`ReportBlock`, `NowMsBlock`, and `initWithReport:nowMs:`. The actual
`performance/.local/release-001/generated` Kotlin and Objective-C header were
inspected and match these host call sites. `NowMsBlock` returns primitive
`double`, and the generated wrapper header is
`RenderBenchmark/RenderBenchmark.h`. Neither host defines a replacement ABI or
bridge model. This inspection is generated-source evidence; it is not a native
host compile or runtime claim.

`nowMs()` is synchronous on its calling thread: Android `System.nanoTime()`
converted to milliseconds; iOS `clock_gettime(CLOCK_MONOTONIC)` converted to
milliseconds. It performs no UI work or synchronous dispatch. Each report
callback samples the process clock/CPU on entry, then dispatches parsing,
persistence, and status UI to main.

Before loading/creating the Valdi runtime or root, both hosts display the stage
backend with stable `SETTLING` status under `render-benchmark-status` and wait
8,000 ms in the foreground. Android schedules a cancellable main
`Handler.postDelayed` from `onResume`; iOS uses a cancellable main-queue
`dispatch_after` block from `applicationDidBecomeActive`. Leaving the foreground
cancels pending startup; returning starts a fresh full eight-second settle.
Teardown/termination cancels pending work, and lifetime, terminal and
already-started guards prevent a late or duplicate runtime/root creation.
Reports record this control as `startupSettleMs: 8000`.

This fixed startup window allows Argent's automatic launch screenshot/tree
capture to finish before the short benchmark scenes begin. The parent performs
no UI polling during measurement. Status remains `SETTLING` through native
runtime/root initialization until the actual module `started` callback arrives;
there is no countdown or periodic status update. Both CPU proxy and whole-run
elapsed timing start at that callback, so the startup settle is excluded.
The 180-second watchdog includes the settle and any foreground interruptions.

The sibling module emits JSON schema 1 with ordered `kind` values:

1. `started`: compile provenance, `suiteIds`, plan, `status: RUNNING`.
2. Four `scene` reports: `index`, `result.id`, status, full timing/canary payload.
   IDs are `cached-vector`, `shaped-text`, `image-svg`, `lottie`.
3. `finished`: status, `completed`, the full `scenes` array, plan, provenance,
   and verified Hermes engine.

Both hosts preserve every received JSON event under `events[].event`, plus
`events[].nativeReceipt` timestamps. They retain failed scenes through the final
report. PASS requires all four ordered scenes and final completed/PASS scenes
and Hermes verification. Startup/report/module errors and the 180-second
native watchdog produce a `nativeFailure` entry and terminal FAIL.

Each launch creates a UUID directory:

- Android primary: `filesDir/render-benchmark/<UUID>/`.
- Android export: `getExternalFilesDir(null)/render-benchmark/<same UUID>/`.
- iOS: `Documents/render-benchmark/<UUID>/`.

Primary `results.json` is atomically replaced after started/scene/final events.
The complete JSON includes stage provenance, events, CPU proxy, terminal status,
and paths. `latest.txt` in each base directory contains that base's absolute UUID
directory path. `done` is terminal text such as `tgfx PASS` or `skia FAIL`, ending
with a newline. Only accept a result when `done`, `results.json` status/backend,
run UUID, and the UI agree. Both Android latest pointers are initialized for
the new run before loading the runtime, so a collector does not reuse an old
terminal result while a new run is starting.

Android mirrors the **final benchmark report only** into app-specific external
storage, requiring no storage permission. It writes external results/latest/done
before committing primary `done`; any export failure changes the primary result
and UI to FAIL, removes PASS markers, and makes a bounded attempt to persist FAIL
to both locations. The export contains only this benchmark's reports, never
other private app data. The status view shows the terminal result and both
directory paths. Its stable identifier is `render-benchmark-status`
(`contentDescription` plus `R.id.render_benchmark_status` on Android,
`accessibilityIdentifier` on iOS).

Fatal process termination, OS kill, inaccessible storage, or a blocked main
thread may prevent any terminal marker. Missing `done` is incomplete, never PASS.
The native watchdog is a main-thread timeout; it does not recover a wedged UI
thread. Android teardown and iOS termination callbacks record failure when
delivered before completion.

## Measurement meaning

The module's timed loop is managed render plus synchronous bitmap readback:
`processFrame -> await draw -> rasterInto(bitmap, true)`. Reported renders per
second and duration distributions are **GPU-synchronized render/readback
throughput and frame latency**, not measured onscreen presentation FPS.
The UIKit/Android root is a small host/status view; neither host claims screen
compositor performance or adds display-link cadence as a substitute.

`cpuProxy` samples Android `Process.getElapsedCpuTime()` or iOS
`getrusage(RUSAGE_SELF)` user + system CPU time at native started/finished callback
entry. It records start/end CPU, delta CPU, elapsed monotonic time, and
`processCpuMsPerWallMs`. This whole-process interval includes managed scene setup, warmup,
measured draws, readbacks, correctness checks, report/persistence overhead and
concurrent runtime work. The ratio can exceed 1 on multiple cores. It is a CPU
activity proxy, **not watts, joules, battery drain, or a direct power result**.
External platform power tooling and device controls belong to the parent.

## Android commands for the parent

Use the parent's existing Gradle installation; no wrapper or tool download was
added. Required properties are absolute `-PstagePath` and `-PbuildPath`.
`-Pbackend=skia|tgfx` optionally asserts the stage label; it does not select or
alter rendering/build configuration.

The assets SourceSet receives the concrete absolute `host-assets` directory,
not a Provider/TaskProvider (rejected by AGP 9.4.1). Both `preBuild` and every
variant's `merge*Assets` task explicitly depend on `verifyBenchmarkStage`, which
validates the stage before copying its package and provenance into that directory.
This wiring is shared by both backends and leaves stage verification enabled.

```sh
PERF=/Users/cloud/.config/superpowers/worktrees/Valdi-node-only/tgfx-prototype-20261104/snap_drawing/experimental/tgfx/full/performance
STAGE=/absolute/export/android-skia
BUILD=/absolute/build/android-skia
python3 "$PERF/android/check_stage.py" --platform android --stage "$STAGE" --backend skia
gradle -p "$PERF/android" --project-cache-dir "$BUILD/project-cache" \
  -PstagePath="$STAGE" -PbuildPath="$BUILD" -Pbackend=skia assembleRelease
```

The default output is unsigned:
`$BUILD/outputs/apk/release/*-release-unsigned.apk`. The parent can sign both
backend APKs with its same explicit local debug key using its normal
`zipalign`/`apksigner` flow. Alternatively, explicitly append
`-PlocalDebugKey=true` to both builds to use AGP's debug signing config for
**local test signing of the Release APK**. It remains Release,
`debuggable=false`, `minifyEnabled=false`, `shrinkResources=false`.
Never label that key as production/distribution signing.

Both use compile/target SDK 36, build tools 36.0.0, min SDK 24, arm64-v8a,
Java 17, AGP 9.4.1, AppCompat 1.7.1, DynamicAnimation 1.1.0,
lifecycle-process 2.2.0 and ExifInterface 1.3.3.
The manifest is `<profileable android:shell="true"/>`; it has no
`android:debuggable="true"` and requests no storage permission.
Use separate build paths and the same flags, dependencies, signing identity and
source freeze for both backends.

After a parent-authorized install/run, retrieve the mirrored Release results:

```sh
adb shell am start -n io.valdiroutes.renderbenchmark/.BenchmarkActivity
adb pull /sdcard/Android/data/io.valdiroutes.renderbenchmark/files/render-benchmark \
  /absolute/results/android-skia
```

Wait for terminal UI/result before pulling; the export's `latest.txt` names the
new UUID. `run-as` is intentionally unnecessary for a non-debuggable Release.
App-specific external paths and adb access depend on the target device; the
parent's emulator supports the path above. Pull before uninstalling; an uninstall
can delete both report locations.

## iOS commands for the parent

The parent prepares `performance/.local/host-stage` as a symlink to the exact
external iOS stage, then regenerates the project **for every stage switch**.
Generated file references are captured by XcodeGen, so switching only the symlink
after project generation is insufficient. `BENCH_STAGE`, when supplied, must
resolve to that same symlink target; the prebuild check rejects a mismatch.

```sh
PERF=/Users/cloud/.config/superpowers/worktrees/Valdi-node-only/tgfx-prototype-20261104/snap_drawing/experimental/tgfx/full/performance
BENCH_STAGE=/absolute/export/ios-skia
BUILD=/absolute/build/ios-skia
export BENCH_STAGE
mkdir -p "$PERF/.local"
test ! -e "$PERF/.local/host-stage" || test -L "$PERF/.local/host-stage"
ln -sfn "$BENCH_STAGE" "$PERF/.local/host-stage"
python3 "$PERF/android/check_stage.py" --platform ios --stage "$BENCH_STAGE" --backend skia
xcodegen generate --spec "$PERF/ios/project.yml" --project "$PERF/ios"
xcodebuild -project "$PERF/ios/RenderBenchmark.xcodeproj" -scheme RenderBenchmark \
  -configuration Release -sdk iphoneos -destination generic/platform=iOS \
  -derivedDataPath "$BUILD" CODE_SIGNING_ALLOWED=NO build
```

The unsigned device product is
`$BUILD/Build/Products/Release-iphoneos/RenderBenchmark.app`; this command alone
does not install it on a phone. The parent must provide the same real
development signing identity/team/profile for both backend builds before device
installation. For example, replace `CODE_SIGNING_ALLOWED=NO` above with
`CODE_SIGNING_ALLOWED=YES DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic`
using an already provisioned local identity. No signing team is hardcoded.

For a simulator stage use `-sdk iphonesimulator -destination
'generic/platform=iOS Simulator'` and `CODE_SIGNING_ALLOWED=NO`; its product is
under `Release-iphonesimulator`. Simulator measurements do not establish actual
phone FPS/power. iOS deployment target is 15.0, ARC is enabled, optimization is
`-Os`, `-ObjC` and runtime embedding are identical for both backends.
The parent retrieves `Documents/render-benchmark` from the app container using
its authorized device/simulator tooling.

## Source freeze

`android/SOURCE-FREEZE.sha256` hashes only this child's `android/**`, `ios/**`,
and `HOSTS.md`, excluding the freeze file itself. It excludes sibling module,
compiler, design, native benchmark, generated, stage and build outputs. Verify
from this `performance/` directory with:

```sh
shasum -a 256 -c android/SOURCE-FREEZE.sha256
```

The freeze is source identity only; it makes no native build, ABI, runtime,
presentation, power, size, or device acceptance claim. The parent owns all
native/device work and must measure actual signed Release artifacts separately.
