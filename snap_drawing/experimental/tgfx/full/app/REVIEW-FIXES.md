# App source handoff revision 2

This revision addresses the two independent app review findings. It is
source-only: no compiler, bridge execution, source-state regression, native
build/configuration, application, or device was run by the child. The parent
must compile the revised module into a fresh diagnostic output directory.

The source freeze now contains **21 files**, excluding `SOURCE-FREEZE.sha256`
itself and all app-local diagnostics/staging/build outputs. The previous
20-file freeze and all original evidence remain available.

## Historical evidence preserved before edits

`.local/review-fixes-01/before/` contains:

* `source/`: all 20 matching original source files and the original
  `SOURCE-FREEZE.sha256` (SHA256
  `931ed5239e8fc57afd02483d257da115b24bae82421bc20d3c6e078a80120728`).
* `evidence/node-diagnostics-01/`: the actual generated sources, resources,
  archives, compile inputs, and completion receipt from compile1.
* `evidence/node-diagnostics-01.log`, `app-node-compile1.json`, and
  `app-source-staging-checks1.json`: actual parent logs/records.
* `evidence/export-consumer/`: the already staged source consumer, preserving
  symlinks without following them.
* `snapshot.json`: source file list, count, original freeze hash, and evidence
  inventory. All originals remain in their original locations.

Compile1's exit 0 in 1.869931292 seconds is valid evidence for the **previous**
TS source, whose hash was
`104c75ffddb679988bb1910f6f7a9b25533d7762ade042c8f453832450e600dd`.
The revised TS hash is
`2253a20fbbb40e47bfde17c5ef5ba9a5c0d9065de61da358b8dbedb5dacebce4`.
Compile1 does not validate this revised source. Its runtime remains unbound.

The parent syntax/staging/negative-input results apply to revision 1.
No positive runtime-bound stage or native application acceptance has been
established.

## P1: use the supported managed asset contract

`valdi/src/valdi/snap_drawing/Layers/Classes/ImageLayerClass.cpp` and
`AnimatedImageLayerClass.cpp` bind asset attributes through
`valdi/src/valdi/runtime/Attributes/AssetAttributes.cpp`. The shared binding
includes `onAssetLoad`; the managed classes do not bind `onImageDecoded`.

`Acceptance.tsx` removes the unsupported handler, its `decoded` counter, and its
required assertion. It adds no synthetic callback, fake count, or runtime API.
The actual acceptance requirements remain:

* All nine original ordered cases, including both labels and fixed-time Lottie
  frames, retain their content and difference assertions.
* Each image case requires a managed tracker count, an actual successful
  `onAssetLoad`, and no callback/tracker asset errors.
* Every capture checks RGBA readback size and expected rendered content, then
  encodes PNG and uses real `drawing.decodeBitmap()` dimensions, format, and
  normalized pixel-content round-trip checks.
* The native hosts still decode and check each 160x120 PNG before completion.
  Their generated context APIs, class names, constructors, and host source are
  unchanged.

## P2: conservative local runtime source/build envelope

`bridge/source-state.mjs` replaces the narrow root list with policy
`local-runtime-v2`. The policy identifier is included in the hashed entries.
The SHA256 path/content algorithm and selected-profile TGFX receipt handling
are otherwise retained.

The actual source BUILD chain includes:

| Runtime/build input | Existing declaration |
| --- | --- |
| Runtime encoding, protobuf and Yoga | `valdi/BUILD.bazel`, target `valdi_runtime` |
| Core utility and encoding implementations | `valdi_core/BUILD.bazel`, target `valdi_core_cc` |
| Protobuf implementation and descriptor wire proto | `valdi_protobuf/BUILD.bazel` |
| Utility base/core/encoding/logging/crypto and platform code | `libs/utils/BUILD.bazel` |
| Reported missing concrete input | `libs/utils/src/utils/encoding/Base64Utils.cpp`, included by `utils_encoding_cc`'s source glob |
| Runtime wire-format proto | `valdi/BUILD.bazel` alias `valdi_proto_cc` to `compiler/compiler:valdi_proto_cc` |
| SnapDrawing, TGFX and local JNI/Yoga dependencies | `snap_drawing/BUILD.bazel`, local `third-party` declarations |
| Export/package helper and local registry patches | `bzl/valdi/valdi_exported_library.bzl`, `bzl/android/package_aar.bzl`, `bzl/dependencies.bzl`; these reference `tools/zstd` and local `registry` patches |

All roots below exist in the actual experimental checkout. The helper hashes
their filesystem contents directly, including untracked production port files;
it does not obtain a tracked-file list from Git:

* Root `BUILD.bazel`, `MODULE.bazel`, `MODULE.bazel.lock`, `.bazelrc`,
  `.bazelversion`, `.bazelignore`, `package.json`, and root `*_install.json`.
* Entire `bin`, `bzl`, `bzlmod`, `compiler`, `libs`, `modules`, `npm_modules`,
  `registry`, `scripts`, `snap_drawing`, `src`, `third-party`, `tools`, `tsn`,
  `valdi`, `valdi_core`, and `valdi_protobuf` trees, subject to the exclusions
  below.
* `.tgfx-deps/receipt.json`, normalized to its schema, common pins and selected
  profile; `.tgfx-deps/<profile>/manifest.json`.

This is deliberately broader than the configured native export closure: some
unused modules, tools and tests within those roots also invalidate the source
hash. It avoids omitting local implementations such as `libs/utils` or local
build declarations simply because the earlier app source list did not name
them.

Explicit exclusions are `snap_drawing/experimental` (the isolated apps and full
native suite shells), entries named `.git`, `.local`, `.cache`, `.gradle`,
`.build`, `.DS_Store`, `node_modules`, or `__pycache__`, and `bazel-*` output
entries. Unlike revision 1, other hidden source directories, and directories
named `build` or `dist`, are not blanket-excluded. Symlink text is recorded using
the existing hash convention. Original acceptance fixture bytes are separately
hashed and packaged by the actual compiler bridge.

This envelope covers the local worktree source/build roots used by this
consumer; it is not a snapshot of external Bazel repositories, installed Node
packages, SDKs, toolchains, or their caches. External dependency pins and
selected TGFX import receipt/manifest remain separate evidence, alongside the
parent's actual configured graph and export build record. Runtime artifact
hashing does not replace local source binding. A fresh expanded source hash and
matching safety/export receipts are required before binding a native runtime.

The parent will run the actual source-hash regression for
`libs/utils/src/utils/encoding/Base64Utils.cpp`. No regression result is claimed
by this source handoff.

## Existing staged consumer and next parent command

No restaging is needed for these two fixes. A read-only byte comparison found
all eight copied consumer/config/adapter/patch files match their current inputs;
the original main MODULE input hash and staged tools symlink also match.
The staged BUILD and `stage-consumer.mjs` have not changed.
Neither the app TS module nor `source-state.mjs` is copied into that consumer.
Preserve `.local/export-consumer` and all diagnostic history.

The compiler entry point, artifact helper, native hosts, and generated context
API contracts are unchanged. The parent can rerun the actual Node compiler into
a **fresh** output directory:

```sh
TGFX_SOURCE=/Users/cloud/.config/superpowers/worktrees/Valdi-node-only/tgfx-prototype-20261104
TGFX_APP="$TGFX_SOURCE/snap_drawing/experimental/tgfx/full/app"
TGFX_TOOLS=/Users/cloud/project/ValdiNativeRoutes/tools/valdi-rolldown
node "$TGFX_TOOLS/node_modules/tsx/dist/cli.mjs" "$TGFX_APP/bridge/compile-only.mts" \
  --tools "$TGFX_TOOLS" --source-root "$TGFX_SOURCE" \
  --source-revision "$(git -C "$TGFX_SOURCE" rev-parse HEAD)" \
  --runtime-binding unbound --out "$TGFX_APP/.local/node-diagnostics-02"
```

That command is a parent handoff, not evidence of execution. Existing runtime
safety blockers remain with their production source owners. Native export,
host builds, runtime binding, and execution remain parent-owned and pending.
