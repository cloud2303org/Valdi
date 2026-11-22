# ValdiNativeRoutes framework maintenance

This fork retains upstream Git history. `valdiroutes` is the maintained runtime branch; `main` remains the upstream branch. The application pins an immutable commit from this branch.

## Current upstream base

- Repository: https://github.com/Snapchat/Valdi
- Branch: upstream `main` as of 2026-11-22
- Commit: `fcbd6549468be6cc5c8ff95425cf14ab4194770b`
- Last release tag: `beta-0.2.1` (`f26bee4dc170b6c929f71d4dd07d44e033f47176`)

The 2026-11-22 synchronization merges all 42 commits after `aed1249d`,
including branded primitive intersection support in bindings and compiler goldens,
SnapDrawing hover dispatching, text-input host wiring, and iOS viewport fixes,
plus JNI signature utilities and test isolation updates. The fork's removed
legacy Swift compiler (`compiler/compiler/Compiler`) and legacy hotreloader stay
removed, and the consuming application ports branded primitive intersection
resolution into its Node codegen (`tools/valdi-rolldown/codegen`).

The 2026-11-13 synchronization merges all 32 commits after `b527b091`,
including batched pooled-view release, Android border-radius animation safety,
Swift proxy-cache locking, bidirectional text layout, module/source-map loading,
and debugger input error reporting. The application must synchronize its vendored
`ConsoleLogTransformer` with the runtime: logging guards now resolve
`ValdiRuntimeProvider.getValdiRuntime()` on each call. ModuleLoader keeps bootstrap
logging independent of that provider to avoid a require cycle.

The obsolete foundation and valdi_test Web-only implementations are removed with
upstream. Test dependencies are kept out of production module dependencies. The
fork's removed legacy compiler/hotreloader stays removed; the upstream hotreload
launcher change does not restore it.
The PropertyList Jasmine test lives under `valdi_core/test/`, so moving Jasmine to
`test_deps` does not leave test globals in the Node compiler's production inputs.

The 2026-11-08 synchronization merged all 32 commits after `9b7a20d5`,
including the experimental SDL3 Linux host and calculator, Protobuf field
reads and decoding controls, Android gesture/date-format updates, iOS worker
enumeration, and Promise callback attribution. Native Web remains an opt-in
fork module with TypeScript installation glue and Rust/C++ implementations.

The 2026-11-04 synchronization includes all 21 commits after the previous
`4aa98f01` base, including the complete Web renderer changes. The earlier
2026-11-01 synchronization merged all 55 commits after that release. The
application's vendored JSX processor must be updated with the runtime: factory-backed
custom views use new JSXBootstrap/Renderer entry points, and `lazyImport` now accepts
a loader callback. Keep the application's Node/Rolldown regressions in the upgrade checks.

The Hermes dependency remains `260318099.0.4`, including native Promise ownership,
adapter boundary/teardown fixes, and internal-bytecode validation. The older upstream
Hermes AST patch is not applied to that different engine revision. The existing
custom-module asset URL method is retained alongside upstream's optional content
identity method. XCFramework exports retain both arm64 and x86_64 simulator slices
for the consuming project's current module and Swift tooling.

The new callback-attribution API must ship with its matching TypeScript modules:
`withGlobalNativeRefs` now requires an explicit owner label and calls
`runtime.withCallbackAttribution`. Android keeps the fork's custom asset URL
resolution while also supporting upstream's optional custom-module identity
provider. The Web renderer, browser WebView implementation, observers, animations,
and integration harness remain in the merged source.

TGFX is an archived experiment on `experiment/tgfx-prototype-20261104`; it is not
part of this synchronization. The maintained runtime continues to use Skia.

## Initial application patch migration

The initial migration applies these changes without changing application behavior. Hermes source lists, prepared headers and built-in bytecode now live in `third-party/hermes_v1/`, referenced through `@valdi` rather than the application root.

- `cdfaf744a4633ed208ba8f2e17ce06aff5552c2e`: Use rules_python for ClientSQL generator targets (formerly `clientsql_rules_python.patch`).
- `4dbb38deb81a3c0d560c17e39d2744a95b4627bc`: Upgrade Hermes to 260318099.0.4 with a self-contained Bazel toolchain (formerly `hermes_v1.patch`).
- `0a92b014264bcc186783e9d7f54ba61fe7a80d3c`: Adapt Valdi VM compiler and debugger to Hermes V1 (formerly `hermes_v1_adapter.patch`).
- `1628de216ed666fa7833303aef1e21d20eeb211b`: Use Hermes-only runtimes with lean production support (formerly `hermes_release.patch`).
- `f636b6335d04fb3d6732e86238fa05e7f19dc3b7`: Upgrade companion minification to Terser 5 (formerly `terser5.patch`).
- `41e07144cb59818d6063aa742db4d7e87cab2bfc`: Emit ES2020 JavaScript for the Hermes runtime (formerly `ts_target_es2020.patch`).
- `8f28b50e88fdacf5e842d220227593a1bee6dcc3`: Align Android native libraries for 16 KB memory pages (formerly `android_16kb_pages.patch`).
- `83463d84905fb969c72fd403bef3b2299a2e5042`: Export drawing headers required by the iOS runtime (formerly `ios_export_drawing_hdrs.patch`).
- `ceea4f472149802e9fefd4a9a22ffbcece04096c`: Precompile iOS release modules to Hermes bytecode (formerly `ios_hermes_bytecode.patch`).
- `22823725d353dd41b7add6ffc26044df30e102b9`: Resolve images from custom OTA module providers (formerly `custom_module_assets.patch`).
- `d4a33e8cbb128fbc152f0ff32d926edd8ff91b48`: Link application native dependencies into exported runtimes (formerly `export_native_deps.patch`).

## Synchronizing upstream

In a clean clone of this fork:

```sh
git remote add upstream https://github.com/Snapchat/Valdi.git  # once
git switch valdiroutes
git config pull.rebase false
git pull --no-rebase --no-ff upstream main
```

This creates a merge commit and preserves both histories. For validation before
the merge commit, add `--no-commit`, run the checks, then commit the merge.
A shallow clone may first need enough history to find the common ancestor.
Keep branch tracking on `origin/valdiroutes`; name `upstream main` explicitly
when pulling official changes.

Resolve conflicts in source, retaining only customizations still needed. Update the upstream base recorded above. Do not force-sync the maintained branch over its custom commits. Push `valdiroutes`, then pin that exact commit in ValdiNativeRoutes with `node scripts/upstream.mjs pin COMMIT` and run its application CI (npm, Android and iOS). Validate Release bytecode and native behavior for the resulting runtime.

The application owns its pages, host code and Node compiler port. Its companion diff report and binding goldens still need review when upstream code generation changes. Framework Actions are disabled in this fork; application CI is the validation entry point.

## Validation entry-point repairs (2026-11-13)

The Web Unicode implementation leaves the backing-buffer type of its private
encoding helpers inferred. This keeps `encodeString(): ArrayBuffer` compatible
with TypeScript 5.9 without changing the emitted JavaScript.

SnapDrawing tests resolve declared resources through `rules_cc` runfiles and
repository mapping, including manifest-only layouts, with an exact checkout-root
fallback for direct execution. Required-resource failures exit with a diagnostic
even in optimized builds.

The focused callback-attribution target uses real Node-compiled debug module
archives instead of the removed legacy static-resource generator. A target-only
GoogleTest environment registers those archives once, retaining their borrowed
storage across repeated environment setup. The shared asset rule also preserves
the NativeWeb runtime tests' existing namespace and archive paths. These changes
do not restore the legacy compiler or change the broader integration suite's
resource mechanism.

## Node-only module compiler (2026-10-29)

The maintained branch removes the old Swift Valdi compiler implementation,
its private vendors/tests, standalone compiler targets and launch helper.
`compiler/compiler` retains only `valdi.proto` and its C++ protocol targets,
which are runtime inputs. Native iOS Swift code and the platform Swift
compiler/toolchains remain intact. The C++ compiler toolbox, its private
engine bridge target and optimization wrapper are also removed. Exported
header rewriting uses `bzl/valdi/node_tools/rewrite_header.mjs`, invoked with
the registered Bazel Node toolchain; it has no npm or runtime library dependency.
The textual import, flattening and preserved-module behavior matches the former
C++ implementation. Run `node --test bzl/valdi/node_tools/rewrite_header.test.mjs`.
Legacy toolbox icon conversion and web path packaging fail explicitly;
ValdiNativeRoutes uses its own Node asset pipeline and native host app builds.

`valdi_compiled` now always delegates its established output/provider contract
to the consuming application's `//bzl/node_compiler:adapter.bzl` (loaded from
the main workspace). ValdiNativeRoutes owns that Node frontend, generated
bindings and package dependencies. There is no compiler-backend selector or
Swift fallback. The native export/library rules are retained. Upstream legacy
hotreload, projectsync and desktop static-resource compiler entry points fail
explicitly; the application uses its Node equivalents where supported.

Production built-in module sources exclude upstream test sources and test-only
dependencies. On future upstream merges, retain these compiler deletions and
review new runtime protocol/codegen contracts against the application's real
module tests and historical native ABI goldens. Do not restore the compiler
source or its old toolchain dependencies as an automatic fallback.
