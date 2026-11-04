# Actual Valdi/Hermes application acceptance — source preparation

Parent integration update, November 4, 2026: the independent Android and iOS
native-suite shells each completed 260/260 checks and a clean 4/4 replay.
Those shells do not start Hermes. Actual iOS and Android Hermes applications
now pass all nine managed scenes; Android's SnapDrawing root also passes a real
SurfaceView PixelCopy content control. See `VALIDATION-20261104.md` for final
artifacts, replay results and limits. The historical source-only handoff below
is retained for provenance.
The isolated consumer now canonicalizes its tools path with Bazel's `realpath`.
Without that adjustment, tsx resolves the staged tools symlink while the runtime
CLI compares its real `import.meta.url` with the unresolved entry-point argument,
exits successfully, and creates no declared outputs. Export attempt 4 confirms
the corrected Jasmine and drawing compiler actions produce their outputs.
Only the staged tool repository is adjusted; app-main tools remain unchanged.

The first actual iOS app run verified Hermes and passed eight of nine cases.
Its SVG input failed asset loading: the integration-app SVG has only a viewBox,
while the static ImageLoader calls `Image::make` without preferred dimensions.
Both retained raster backends require explicit output dimensions for that input.
`fixtures/image.svg` preserves the original geometry and adds only intrinsic
`width="120"` and `height="80"`. Both Node entry points bind this fixture's real
bytes and hash. All nine cases, pixel assertions, tolerances, and asset-error
checks are unchanged. The original input and failed run are preserved separately;
the corrected fixture passed fresh application runs on both platforms.

**Source handoff revision 2: the two independent review fixes are source-only
and have not been compiled.** Parent Node compilation of the earlier TS source
passed on November 4, 2026: exit 0 in 1.869931292 seconds. Native export, host
compilation, installation, and runtime acceptance remain pending. The author
has executed no compiler, configure, test, native
executable, or device command. Parent source review must precede native
compilation. All additions and future staging outputs live under this directory.
App main, fork main, their pins, and their runtime artifacts are inputs only.
The independent `../mobile` full native suite shells remain a different gate.

The parent evidence is `.local/node-diagnostics-01/compile-complete.json`,
`.local/node-diagnostics-01.log`, and
`/tmp/valdi-tgfx-full-20261104/app-node-compile1.json`. The real Release
`tgfx_acceptance` archive, generated Kotlin/Objective-C bindings, catalog
resources, and drawing JS wrappers compiled. This is explicitly
`runtimeBinding=unbound`, `runtimeArtifact=null`, `nativeAcceptance=false`.
It establishes no Hermes application execution or native rendering result and
does not validate the revised `Acceptance.tsx`.

The historical successful compiler subset was:

| Input | SHA256 |
| --- | --- |
| `module/src/Acceptance.tsx` | `104c75ffddb679988bb1910f6f7a9b25533d7762ade042c8f453832450e600dd` |
| `bridge/compile-only.mts` | `4517dfc327ea56e486b6fda5a40dddb43fde78f8c1f2b025197f02c1e5e75a23` |
| `bridge/artifact.mjs` | `5d4597175e61120c3f2b10e4abf1bc49744b803123fafe5dd03609f10456533d` |

The old 20 source files, their original freeze, complete compile1 output/log,
staged consumer, and parent compile/staging records were copied before edits to
`.local/review-fixes-01/before/`. All original evidence is retained.
`REVIEW-FIXES.md` records the source changes, source envelope, and remaining
parent validation. The compiler entry point and artifact helper above are
unchanged; the revised TS source needs a fresh diagnostic output directory.

The emitted Android constructor is
`com.snap.modules.tgfx_acceptance.AcceptanceContext(report: (event: String) -> Unit)`.
The emitted Objective-C type is `TGFXAcceptanceAcceptanceContext`, with
`TGFXAcceptanceAcceptanceContextReportBlock` and `initWithReport:`.
Both hosts consume these actual generated declarations.
`SOURCE-FREEZE.sha256` records revision 2's 21 app source files, excluding local
diagnostics/staging/build outputs and the hash list itself. Review those sources
before compiling the hosts; after review the parent can verify the recorded
bytes with `shasum -a 256 -c SOURCE-FREEZE.sha256` from this directory.

**Native execution remains blocked at this handoff.** Parent reports
TGFX6acf/full still failed, with P1 async writers/cancellation under the GPU
owner's correction. `SAFETY-STATUS.json` records that blocked handoff and is
deliberately rejected as clearance. Five compiled dependency archives and the
successful Node diagnostics do not clear this blocker. The recipe below starts
only after the parent resolves it and reviews the final source.

This app starts the real platform Valdi runtime, loads real Node/Rolldown-produced
Valdi modules, checks the executing Hermes engine, and renders TSX through the
real `drawing.createManagedContext()` / native SnapDrawing module factories.
It captures RGBA pixels, encodes PNG, decodes PNG, and asserts content. iOS keeps
its normal UIKit root. Android has a normal Android root by default and an
optional real SnapDrawing root configuration. Its managed bitmap gate is
supplemented by a separate Android SurfaceView PixelCopy content control.

## Source and ownership

* `bridge/stage-consumer.mjs`: stages an isolated runtime export consumer. Reads
  app main's **actual** MODULE, bazelrc, Node runtime adapter, and referenced
  root-local patch sources. It replaces only
  the staged archive overrides for Valdi and its internal submodules with explicit
  paths into the experimental worktree. A tools symlink is created only inside
  `.local/export-consumer/`. No main checkout file is changed.
* `consumer/BUILD.bazel`: existing `valdi_exported_library()` for Android, plus
  explicit single-profile Apple XCFramework targets using the official export's
  rewritten public headers. No app-main C++/Swift dependencies. Do not build its
  default `valdi_runtime_ios` target, which requests x86_64.
* `bridge/prepare.mts`: imports app main's actual exported
  `compileNodeModule(original, out, iosName, mode, dependencies, iosLanguage)`.
  It supplies an explicit staged `ValdiModule.dir`, installed `nodeModules`,
  `generatedDirs`, `VALDI_SOURCE_ROOT`, and an explicit `drawing` dependency.
  The original API copies sources into its own temporary module; temporary files
  are directed below this app's stage. No app-main `appModule()` source directory
  is used for compilation.
* The separate **actual** `compileRuntimeModule()` API packages drawing's JS
  wrappers with the known runtime adapter contract: `SCCDrawing`,
  `com.snap.modules.drawing`, and real worktree sources. Only its archive/map
  outputs are requested. Its native module factories come from the exported
  runtime; generated drawing native implementations are not compiled again.
* `module/src/Acceptance.tsx`: the only new application module. Its generated
  `AcceptanceContext.report(string)` is compiled into each host. No handwritten
  ABI, JNI entry point, replacement graphics runtime, or fake module loader.
* `android/`: Gradle only compiles the Kotlin host/generated bindings and packages
  the explicit runtime AAR, prepared module archives, and catalog drawables.
  No `externalNativeBuild`, downloader, Gradle wrapper, or implicit compiler phase.
* `ios/`: XcodeGen source description and Objective-C UIKit host. Only generated
  Objective-C bindings and the explicit experimental XCFramework are linked.
* `bridge/runtime-contract.mjs`, `runtime-receipt.mjs`, `check-stage.mjs`: pure
  file inspection for parent-reviewed native export binding. Schema 2 receipts
  bind selected-profile source state, actual runtime hashes/platform metadata,
  safety review evidence, successful export log, and configured dependency graph.
  Host prebuild checks reject unbound diagnostics and altered/stale staged inputs.
  Parent syntax/staging and intended negative-input checks passed for revision 1,
  as recorded in `app-source-staging-checks1.json`. The revised source-state
  helper has not been executed by the author; no positive runtime-bound stage
  has been tested.

The native source seams checked for this handoff are:

| Purpose | Existing experimental source |
| --- | --- |
| Export closure and rewritten public headers | `bzl/valdi/valdi_exported_library.bzl` |
| TGFX/Skia backend select | `snap_drawing/BUILD.bazel`, `third-party/tgfx/BUILD.bazel` |
| Android manager, default root selection, exception reporter | `valdi/src/java/com/snap/valdi/ValdiViewLoaderManager.kt` |
| Android scoped root configuration | `valdi/src/java/com/snap/valdi/ScopedValdiViewLoader.kt`, `ValdiContextConfiguration.kt` in the same directory |
| Android actual inflation/root handler | `valdi/src/java/com/snap/valdi/ValdiRuntime.kt` |
| Android module registration and support setup | `valdi/src/java/com/snap/valdi/modules/ModuleFactoryRegistry.kt`, `valdi/src/android_support/java/com/snap/valdi/support/SupportValdiViewLoaderManager.kt` |
| iOS lazy real runtime creation | `valdi/src/valdi/ios/SCValdiRuntimeManager.mm` |
| iOS normal container, provider, configuration, exception contracts | `valdi_core/src/valdi_core/ios/valdi_core/SCValdi{ComponentContainerView,CustomModuleProvider,Configuration,ExceptionReporter}.h` |
| Managed drawing factory and bitmap APIs | `src/valdi_modules/src/valdi/drawing/src/ManagedContextFactory.ts`, `BitmapFactory.ts` in the same directory |

The Android manager resolves `RenderBackend.DEFAULT` to Android when no
preference is set, even when SnapDrawing is available in the native export.
Enabling the TGFX build select therefore does not enable a SnapDrawing screen
root by itself. The extra host mode explicitly selects the real scoped root.

## Assertions and protocol

Nine required cases, in order:

1. `composition`: opaque red, source-over blue at 0.5 opacity, clipping interior
   and exterior pixels.
2. `shape`: real `GeometricPathBuilder` triangle interior and exterior.
3. `label-iiii`: black system-font glyph coverage in its label bounds.
4. `label-mmmm`: coverage plus more than 70 changed pixels from `IIII`.
5. `bitmap-bytes`: original full-suite `pattern.png` bytes through
   `makeAssetFromBytes`, with red/green/blue/white quadrant assertions.
6. `bitmap-catalog`: the same original PNG through the actual Node resource
   pipeline, with wider tolerance for Android's lossy density WebP conversion.
7. `svg`: integration app `image.svg` geometry with explicit intrinsic dimensions through
   `makeAssetFromBytes`, with three colored quadrants and a white center.
8. `lottie-t0`: original integration app `animation.json`, fixed time 0 seconds,
   `advanceRate=0`, `loop=false`, window [0,2]; center/outside/radius assertions.
9. `lottie-t1`: time 1 second, larger radius, yellow stroke, and more than 400
   changed pixels from time zero. No wall-clock animation timing is accepted.

For every image case, the managed asset tracker must count an asset, its
`onAssetLoad` success callback must fire, and the complete asset error list must
be empty. The managed `ImageLayerClass` and `AnimatedImageLayerClass` bind shared
`AssetAttributes` loading; they do not bind `onImageDecoded`. The acceptance
module therefore installs no decode callback or synthetic decode counter.
Decode evidence comes from the case's expected rendered content and the actual
PNG decode dimensions, format, and content checks below.
The source bytes and their SHA256s are recorded when
staging; SVG/Lottie are never converted to build-time raster images.

Every case checks RGBA readback size and round trips its PNG through the real
`drawing.decodeBitmap()`. PNG dimensions/format and decoded pixel hash must agree
with the original readback. `pixelHash` is a noncryptographic FNV content
fingerprint; build/artifact provenance uses SHA256.

The generated callback sends JSON schema 1 events:

* `started`: real `HermesInternal.getRuntimeProperties()` result, with OSS release
  equal to the pinned compiler contract and a positive bytecode version, `verified=true`,
  source/runtime/tool/fixture provenance, and the required case IDs.
* `case`: all individual assertions and actual values, asset counters/errors,
  pixel fingerprint, and the actual PNG bytes as base64.
* `finished`: the complete ordered case set and final PASS/FAIL. Exceptions
  include a JS stack when available. Zero cases, skips, incomplete sets, blank
  glyphs, blank animation frames, or just a nonempty PNG never pass.

Native hosts load bundled runtime provenance before starting Valdi and require the
JS-start runtime hash, source state, and source root to match that native input.
They independently require the full ordered case set, nonempty successful
assertions, empty asset errors, a verified Hermes start, and decodable 160x120 PNG
snapshots. They save each PNG before accepting completion. Runtime exception
reporters and a 180-second host watchdog force FAIL. A native main-thread hang or
native process crash still needs the parent's external timeout/crash evidence;
the in-process watchdog cannot execute on a blocked main loop.

Reports are under app-owned `tgfx-app/<run-UUID>/`:

* Android: `files/tgfx-app/latest.txt` points to the current directory.
* iOS: `Documents/tgfx-app/latest.txt` points to the current directory.
* Each directory contains `results.json`, nine named PNGs, and a terminal `done`
  file containing PASS or FAIL. Read the matching current-run files, not an old
  directory. `results.json` and the terminal marker are atomically replaced.
* A persistence failure forces the UI to FAIL, removes the PASS marker when
  possible, and attempts one explicit failure report. `REPORT NOT SAVED` is not
  acceptance.

Stable native accessibility identifiers/descriptions: `tgfx-app-status`,
`tgfx-app-results`; iOS preview is `tgfx-app-preview`, Android preview resource ID
is `tgfx_app_preview`. The native results display the complete JSON without PNG
base64; PNG files retain the image bytes.

## Parent recipe after source review

Commands below are a handoff, **not evidence of execution**. Use the parent's one
native build slot. Existing caches, SDK, Xcode, Node/pnpm installation, and the
appropriate prepared TGFX profile are prerequisites. If a dependency is not
available under the parent's permitted fetch policy, stop at that prerequisite.
No bridge downloads anything or runs the app main export scripts.

Set explicit paths; do not point runtime outputs at app main:

```sh
TGFX_SOURCE=/Users/cloud/.config/superpowers/worktrees/Valdi-node-only/tgfx-prototype-20261104
TGFX_MAIN=/Users/cloud/project/ValdiNativeRoutes
TGFX_APP="$TGFX_SOURCE/snap_drawing/experimental/tgfx/full/app"
TGFX_TOOLS="$TGFX_MAIN/tools/valdi-rolldown"
TGFX_NODE=$(command -v node)
TGFX_SOURCE_REVISION=$(git -C "$TGFX_SOURCE" rev-parse HEAD)
TGFX_CONSUMER="$TGFX_APP/.local/export-consumer/valdi"
```

The parent already created this consumer successfully before revision 2.
No restaging is needed for these review fixes: `stage-consumer.mjs`, its input
consumer BUILD, copied Bazel configuration, adapter, and patch sources are
unchanged. `Acceptance.tsx` and `source-state.mjs` are read from this app when
their bridge commands run; neither is copied into the export consumer.
Preserve the existing consumer and historical outputs. If starting in a
checkout without that stage, run:

```sh
node "$TGFX_APP/bridge/stage-consumer.mjs" \
  --app-main "$TGFX_MAIN" --source-root "$TGFX_SOURCE"
```

That command creates a fresh isolated source consumer and refuses to overwrite
an existing stage. It does not edit or install anything in either main checkout. The
staged MODULE retains unrelated dependency pins from app main. The flags below
also redirect the export disk cache into this app. The staged adapter resolves
installed tools through the app-local tools symlink; `@valdi` resolves through
the explicit experimental worktree override.

**iOS simulator first, after correction and source review.** Ensure the final prepared `ios_sim_arm64` dependency
manifest is installed before hashing; do not change native source/profile inputs
between hashing and export. Source policy `local-runtime-v2` hashes a
conservative local production/build envelope, including untracked files,
`libs/utils`, `valdi_protobuf`, the runtime wire proto in `compiler`, all local
third-party declarations/source/patches, runtime modules, and the local build
rules/tool inputs. The exact roots and exclusions are documented in
`REVIEW-FIXES.md`. It excludes the isolated `snap_drawing/experimental` apps and
suite shells, including this app, and binds the selected TGFX receipt/manifest.
This is not a snapshot of external Bazel repositories, installed SDKs, or
toolchains. It is an input snapshot, not proof that an archive was compiled
from those inputs. Regenerate any pre-revision-2 source hash, safety attestation,
and bound receipt before preparation; an artifact hash cannot substitute for
the expanded source binding.

```sh
TGFX_IOS_SOURCE_SHA256=$(node "$TGFX_APP/bridge/source-state.mjs" "$TGFX_SOURCE" ios_sim_arm64)
TGFX_COMMON_FLAGS=(
  --define=snap_drawing_backend=tgfx
  --define=enable_web=
  --@valdi//bzl/valdi:js_engine=hermes
  --@valdi//bzl/valdi:js_bytecode_format=hermes
  --@valdi//bzl/valdi:assets_mode=inline
  --disk_cache="$TGFX_APP/.local/bazel-disk-cache"
)
TGFX_IOS_FLAGS=(
  "${TGFX_COMMON_FLAGS[@]}"
  --platforms=@snap_platforms//:ios_sim_arm64
  --repo_env=VALDI_PLATFORM_DEPENDENCIES=ios
)
```

The parent must supply a **new** cleared review JSON at
`TGFX_IOS_SAFETY_REVIEW`, with the following fields. This is the required
interface; it is not an attestation for the currently blocked source:

| Field | Required value |
| --- | --- |
| `schema`, `kind` | `1`, `tgfx-native-safety` |
| `status`, `scope` | `cleared`, `async-writers-and-cancellation` |
| `sourceRoot`, `sourceRevision` | Real `TGFX_SOURCE` path, full `TGFX_SOURCE_REVISION` |
| `sourceStateSha256` | Exact `TGFX_IOS_SOURCE_SHA256` after the correction |
| `profiles` | Array containing `ios_sim_arm64` |
| `blockers` | Empty array |
| `evidence` | Array of absolute paths to nonempty actual fix/review evidence files |

Do not change `SAFETY-STATUS.json` to manufacture clearance or bind old
compile-only archives. The receipt hashes the review JSON and each cited file.
Use a separate review for Android when its selected-profile source hash differs.
This review clears the specific async-write/cancellation concern; runtime
acceptance remains pending.

After that review, export the real runtime and retain the matching configured
closure. These snippets require bash or zsh array syntax:

```sh
: "${TGFX_IOS_SAFETY_REVIEW:?absolute path to the new source-matched parent review}"
(
  set -e
  set -o pipefail
  cd "$TGFX_CONSUMER"
  bazel --output_base="$TGFX_APP/.local/bazel-consumer" build \
    //:valdi_runtime_ios_sim "${TGFX_IOS_FLAGS[@]}" 2>&1 \
    | tee "$TGFX_APP/.local/ios-export.log"
  bazel --output_base="$TGFX_APP/.local/bazel-consumer" cquery \
    'deps(//:valdi_runtime_ios_sim)' "${TGFX_IOS_FLAGS[@]}" --output=label \
    > "$TGFX_APP/.local/ios-runtime-deps.txt" \
    2> "$TGFX_APP/.local/ios-runtime-deps.log"
  bazel --output_base="$TGFX_APP/.local/bazel-consumer" cquery \
    //:valdi_runtime_ios_sim "${TGFX_IOS_FLAGS[@]}" --output=files \
    > "$TGFX_APP/.local/ios-runtime-outputs.txt" \
    2> "$TGFX_APP/.local/ios-runtime-outputs.log"
)
```

The explicit `ios={"simulator":["arm64"]}` export target selects the archive
profile through the real Apple rule's split transition. The supplied platform
also has the required apple_support simulator constraint. The parent must inspect
the actual target outputs in `ios-runtime-outputs.txt` and slice metadata; no
output filename or successful configured graph is fabricated here.
If the output is an XCFramework ZIP, extract it **under this app's `.local/`** and
set `TGFX_IOS_RUNTIME` to the resulting `ValdiRuntime.xcframework`. If it is a
directory artifact, use that directory directly. Do not use app main's framework.

The parent then attests the actual exported artifact:

```sh
: "${TGFX_IOS_RUNTIME:?absolute path to the inspected experimental XCFramework}"
node "$TGFX_APP/bridge/runtime-receipt.mjs" \
  --source-root "$TGFX_SOURCE" --source-revision "$TGFX_SOURCE_REVISION" \
  --source-state-sha256 "$TGFX_IOS_SOURCE_SHA256" --profile ios_sim_arm64 \
  --safety-review "$TGFX_IOS_SAFETY_REVIEW" \
  --build-log "$TGFX_APP/.local/ios-export.log" \
  --dependency-graph "$TGFX_APP/.local/ios-runtime-deps.txt" \
  --runtime "$TGFX_IOS_RUNTIME" --out "$TGFX_APP/.local/ios-runtime-receipt.json"
node "$TGFX_TOOLS/node_modules/tsx/dist/cli.mjs" "$TGFX_APP/bridge/prepare.mts" \
  --tools "$TGFX_TOOLS" --source-root "$TGFX_SOURCE" \
  --runtime-receipt "$TGFX_APP/.local/ios-runtime-receipt.json"
node "$TGFX_APP/bridge/check-stage.mjs" "$TGFX_APP/.local/ios_sim_arm64"
```

`runtime-receipt.mjs` writes schema 2, `kind=tgfx-valdi-runtime-export`,
`runtimeBinding=exported`, `nativeAcceptance=false`. It requires the cleared
review, a nonempty successful Bazel export log, and a configured graph containing
the actual platform Valdi, SnapDrawing, and prepared TGFX closure. It rejects
`snap_drawing:skia_backend` and external Skia repository targets;
`skia_user_config` is an allowed configuration repository. The iOS artifact
must contain exactly one arm64 iOS simulator slice, real public runtime headers,
and an arm64 Mach-O dynamic binary whose `LC_BUILD_VERSION` platform is
simulator. Binary plists, unsupported fat metadata, and escaping framework
symlinks fail inspection. If needed, normalize a parent-owned app-local copy's
plist before hashing. File inspection does not infer flags, absence of Skia
symbols, or source provenance from a binary.

`prepare.mts` revalidates all inputs before and after actual Node compilation.
It recompiles the application and drawing wrappers with **bound** provenance;
diagnostic archives cannot be reused. It checks the emitted context names and
Valdi-wrapped Hermes bytecode, copies the reviewed runtime while preserving
internal symlink text, and writes a schema 2 application stage. Source changes
during preparation fail the stage. `check-stage.mjs` recomputes source state,
original/copied runtime hashes and metadata, review/build evidence hashes,
generated bindings/resources hash, package hash, and both module archive hashes.
The host prebuild repeats this read-only check. Preparation is still not native
application acceptance.

Build the iOS host only after these steps:

```sh
(
  cd "$TGFX_APP/ios"
  xcodegen generate --spec project.yml
  xcodebuild -project TGFXValdiApp.xcodeproj -scheme TGFXValdiApp \
    -configuration Debug -sdk iphonesimulator \
    -destination 'generic/platform=iOS Simulator' \
    -derivedDataPath "$TGFX_APP/.local/ios-derived" \
    ARCHS=arm64 ONLY_ACTIVE_ARCH=YES CODE_SIGNING_ALLOWED=NO \
    NODE_BINARY="$TGFX_NODE" build
)
```

Expected application location:
`.local/ios-derived/Build/Products/Debug-iphonesimulator/TGFXValdiApp.app`.
Actual bundle ID is `io.valdiroutes.tgfxapp`. Installation/launch, current-run
report retrieval, image inspection, and log/crash evidence belong to the parent
device workflow. Do not claim an iOS whole-screen TGFX compositor result.

**Android, after the iOS gate.** Prepare the real `android_arm64` dependencies in
the parent's native slot first, then take a fresh source snapshot:

```sh
TGFX_ANDROID_SOURCE_SHA256=$(node "$TGFX_APP/bridge/source-state.mjs" "$TGFX_SOURCE" android_arm64)
TGFX_ANDROID_FLAGS=(
  "${TGFX_COMMON_FLAGS[@]}"
  --define=client_repo_arm64=true
  --android_platforms=@snap_platforms//os:android_arm64
  --repo_env=VALDI_PLATFORM_DEPENDENCIES=android
)
: "${TGFX_ANDROID_SAFETY_REVIEW:?absolute path to the new android source-matched parent review}"
(
  set -e
  set -o pipefail
  cd "$TGFX_CONSUMER"
  bazel --output_base="$TGFX_APP/.local/bazel-consumer" build \
    //:valdi_runtime_android "${TGFX_ANDROID_FLAGS[@]}" 2>&1 \
    | tee "$TGFX_APP/.local/android-export.log"
  bazel --output_base="$TGFX_APP/.local/bazel-consumer" cquery \
    'deps(//:valdi_runtime_android)' "${TGFX_ANDROID_FLAGS[@]}" --output=label \
    > "$TGFX_APP/.local/android-runtime-deps.txt" \
    2> "$TGFX_APP/.local/android-runtime-deps.log"
  bazel --output_base="$TGFX_APP/.local/bazel-consumer" cquery \
    //:valdi_runtime_android "${TGFX_ANDROID_FLAGS[@]}" --output=files \
    > "$TGFX_APP/.local/android-runtime-outputs.txt" \
    2> "$TGFX_APP/.local/android-runtime-outputs.log"
)
# Confirm this is the AAR in android-runtime-outputs.txt before binding.
TGFX_ANDROID_RUNTIME="$TGFX_CONSUMER/bazel-bin/valdi_runtime_android.aar"
node "$TGFX_APP/bridge/runtime-receipt.mjs" \
  --source-root "$TGFX_SOURCE" --source-revision "$TGFX_SOURCE_REVISION" \
  --source-state-sha256 "$TGFX_ANDROID_SOURCE_SHA256" --profile android_arm64 \
  --safety-review "$TGFX_ANDROID_SAFETY_REVIEW" \
  --build-log "$TGFX_APP/.local/android-export.log" \
  --dependency-graph "$TGFX_APP/.local/android-runtime-deps.txt" \
  --runtime "$TGFX_ANDROID_RUNTIME" --out "$TGFX_APP/.local/android-runtime-receipt.json"
node "$TGFX_TOOLS/node_modules/tsx/dist/cli.mjs" "$TGFX_APP/bridge/prepare.mts" \
  --tools "$TGFX_TOOLS" --source-root "$TGFX_SOURCE" \
  --runtime-receipt "$TGFX_APP/.local/android-runtime-receipt.json"
node "$TGFX_APP/bridge/check-stage.mjs" "$TGFX_APP/.local/android_arm64"
```

The AAR selector define chooses the native slice; `--android_platforms` chooses
the imported Android architecture. Do not use the generic app main export script
that also requests arm32. Inspect the AAR's actual `libvaldi_runtime.so` architecture
and dynamic dependencies. If it needs `libc++_shared.so` and does not contain it,
copy the runtime from the **same parent NDK** to
`.local/android-jniLibs/arm64-v8a/libc++_shared.so`. Package any other non-system
shared dependencies there as well; do not add a second native runtime or JNI owner.
The receipt requires real AAR packaging with only the requested arm64 JNI
slice and inspects `libvaldi_runtime.so` as an ELF64 AArch64 shared object.
It does not inspect the ELF dynamic dependency table; retain the parent's
actual link/dependency evidence and hash separately supplied NDK shared
libraries with the final APK evidence.
ExifInterface 1.3.3 is an explicit Gradle dependency because TGFX's real JNI codec
uses it. The actual exported runtime owns TGFX JNI initialization.

```sh
(
  cd "$TGFX_APP/android"
  gradle --offline --project-cache-dir "$TGFX_APP/.local/gradle-cache" \
    -PnodeBinary="$TGFX_NODE" assembleDebug
)
```

Use the parent's compatible installed/cached Gradle distribution (no wrapper
download). AGP 9.4.1 includes Kotlin support; cached Android SDK 36/build tools
36.0.0 and the declared AndroidX artifacts are required. The APK is under
`.local/android-build/outputs/apk/debug/`; inspect its actual filename and contents.
Package/activity: `io.valdiroutes.tgfxapp/.AcceptanceActivity`.

Run the default native-root gate first. For a separate run, launch
`io.valdiroutes.tgfxapp.SnapDrawingAcceptanceActivity`; this explicit Activity
selects the SnapDrawing root and can be launched by the device toolkit without
intent extras. The original boolean intent extra `snapDrawingRoot=true` also
remains available on the default Activity. The host uses the exact existing scoped
configuration seam: `RenderBackend.SNAP_DRAWING`,
`SnapDrawingOptions(enableSynchronousDraw=true, renderMode=SURFACE_VIEW)`, and a
`ValdiRootView(scoped.context)`. This additionally exercises root renderer
enablement; the JSON records `rootMode=snapdrawing`. This mode now delays PASS
until Android `PixelCopy` reads the actual presentation SurfaceView and finds its
white background and blue preview control. It saves `root-presentation.png` and
the observed pixel counts. A missing/black surface or failed copy fails the gate.
The nine exact scene assertions remain managed readbacks; this additional control
does not claim exact onscreen parity for every scene.

## Failure interfaces and remaining parent work

No generic helper outside this directory is required by the proposed source.
If the actual compiler API rejects a staged module, stop with its original
diagnostic. The narrow inputs are `ValdiModule {name,dir,generatedDirs,nodeModules}`,
the explicit drawing dependency, `VALDI_SOURCE_ROOT`, and the known-runtime
`compileRuntimeModule` spec. Do not patch app main or create placeholder outputs.
The bridge verifies generated native class names and stops if codegen changes;
hosts must compile against the actual emitted files.

All native dependency preparation, native exports, source/flag/link-closure
inspection, Apple slice verification, host compilation, runtime execution,
visual evidence, and comparison against a baseline remain parent-owned.
An artifact hash does not prove that a binary has no Skia symbols, uses the
attested backend, or came from the attested flags; retain the parent's actual
configured graph/link/build evidence with the receipt.

The app is one real JS/managed-rendering acceptance gate. Full native suites,
Exact Android onscreen parity for all scenes, iOS whole-screen compositor integration,
physical devices, and production app integration are separate results.
