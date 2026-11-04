# Parent iOS simulator import handoff

Source/metadata/hash inspection on November 4, 2026. No helper, native tool,
configure, build, test, network request, or profile publication was executed
for this handoff. Inputs below belong to the parent's native slot. Existing
profiles and source pins are unchanged.

## Actual configuration and source requirements

The fresh recipe now selects `TGFX_USE_FREETYPE=OFF` for all Apple profiles,
with matching source staging. TGFX's pinned `CMakeLists.txt` then selects
`src/core/vectors/coregraphics` and `TGFX_USE_CORE_GRAPHICS`. SnapDrawing's
actual Apple `NativeFontManager.cpp` uses `tgfx::CTTypeface::MakeFromCTFont`
and `GetCTFont`, so the full Apple port requires that CoreGraphics backend.
Android continues to select/stage FreeType with `TGFX_USE_FREETYPE=ON`.

The inspected parent cache in
`/Users/cloud/project/ValdiNativeRoutes/.tools/build/tgfx-full-ios-sim-metal`
already has Metal on, OpenGL off, FreeType off, and the correct TGFX source
root. ThorVG's inspected Meson metadata describes static/PIC CPU rendering,
all seven required loaders, SIMD on, no extras, and
`arm64-apple-ios15.0-simulator` arguments. Import still validates completed
artifacts and actual object target evidence independently.

The two actual Git roots remain pinned to:

* TGFX `4cf8991d5a3a010c9314538dd2fea937593b206b`
* ThorVG `f0fd698d56b3b8ae797a242cf06aab6b70aed840`

Observed current complete tracked-diff SHA256 values:

```text
tgfx   7b868536a30645cbb68f05785c1c86582f1c02e73ac8280b6c28208b77975061
thorvg 132fbf6268a7f44b891d6e9afc03425e5e2aca7a5a6239b7ea055908626d9f31
```

These are working-tree patch approvals, not new source pins or permanent
constants. If either tree changes, review its new complete binary Git diff
and supply the new hash for the corresponding rebuilt archives/headers.

## Exact current CLI recipe

Run from the experimental worktree root only after the parent Metal build is
complete and its archive is stable. Set:

* `TGFX_IOS_SIM_ARCHIVE_SHA256` to the completed
  `tgfx-full-ios-sim-metal/tgfx.a` hash.
* `PROTOTYPE_DEPENDENCY_RECEIPTS` to the actual additional receipt JSON
  covering the seven initial dependencies listed below. No such complete
  second receipt was found in the supplied prototype/full evidence directories.

The ThorVG and giflib hashes shown below were read from the actual existing
files. Refresh them if the parent rebuilds those files after this observation.
Use the actual matching final build log if a newer rebuild supersedes the
named log.

```sh
APP_ROOT=/Users/cloud/project/ValdiNativeRoutes

python3 third-party/tgfx/prepare.py import-existing \
  --profile ios_sim_arm64 \
  --allow-coregraphics \
  --tgfx-source "$APP_ROOT/.tools/vendor/tgfx" \
  --thorvg-source "$APP_ROOT/.tools/vendor/thorvg" \
  --allow-patch tgfx=7b868536a30645cbb68f05785c1c86582f1c02e73ac8280b6c28208b77975061 \
  --allow-patch thorvg=132fbf6268a7f44b891d6e9afc03425e5e2aca7a5a6239b7ea055908626d9f31 \
  --tgfx-build "$APP_ROOT/.tools/build/tgfx-full-ios-sim-metal" \
  --thorvg-build "$APP_ROOT/.tools/build/thorvg-full-ios-sim" \
  --tgfx-sha256 "${TGFX_IOS_SIM_ARCHIVE_SHA256:?Set completed Metal archive SHA256}" \
  --thorvg-sha256 93587312094e34a875e508615b5063d061d5c8501bd42aa15fcc6e7173f37171 \
  --thorvg-simd-sha256 5254e8424bafc429de0ed2dfb02fe2c8856595f7ce472a678310479027b4ed49 \
  --tgfx-build-log /tmp/valdi-tgfx-full-20261104/tgfx-full-ios-metal-build1.log \
  --thorvg-build-log /tmp/valdi-tgfx-full-20261104/thorvg-ios-sim-prefix-tables-rebuild.log \
  --dependency-receipts /tmp/valdi-tgfx-full-20261104/dependencies/receipts.json \
  --dependency-receipts "${PROTOTYPE_DEPENDENCY_RECEIPTS:?Set actual initial-dependency receipt JSON}"

python3 third-party/tgfx/prepare.py add-giflib \
  --profile ios_sim_arm64 \
  --source "$APP_ROOT/.tools/vendor/giflib" \
  --commit a8e3114a81f0987a61d06a41c99fd7cc2d58232c \
  --archive "$APP_ROOT/.tools/build/giflib-full-ios-sim/libgif.a" \
  --archive-sha256 40b4523093728c23c5ba0f970918bc6ec814e0df7df9fd7dcdca955dc32731c5 \
  --build-log /tmp/valdi-tgfx-full-20261104/giflib-ios-sim-build.log
```

`import-existing` consumes the original Meson
`src/libthorvg-1.a` and
`src/renderer/cpu_engine/libthorvg-sw-simd.a`. The inspected Ninja graph
places the SIMD object in the main library via Meson `link_whole`. The importer
requires the real side archive's object hashes to be a subset of the main
archive's object hashes. It does not silently omit SIMD or run an archiver.

The first command publishes TGFX/ThorVG input files only when run by the parent.
The separate `add-giflib` step completes the full backend's GIF input. Neither
command is executed by this handoff. An already prepared `ios_sim_arm64` is
rejected; the commands do not replace it or `macos_arm64`.

## Exact receipt/manifest contract and gaps

For each extracted dependency, source-archive input requires an explicit
`name`, full `commit` matching TGFX `DEPS`, archive SHA256, and existing tarball.
The optional `archive` path is relative to the receipt or absolute; when
absent the filename must be `NAME.tar.gz` beside the receipt. Every archived
source member must match the extracted tree. There is no synthesized Git
fallback and no approval of modified archive members.

The existing full receipt covers shaderc, libwebp, libjpeg-turbo, glslang,
spirv-headers, spirv-tools, and SPIRV-Cross. Apple still needs explicit
receipts/tarballs for **vendor_tools, pathkit, skcms, zlib, libpng,
concurrentqueue, and highway** if those directories are extracted rather than
real Git roots. FreeType is no longer an Apple source/receipt requirement.
The prototype directory contains a pathkit tarball, but that alone is not the
complete seven-dependency receipt. Missing archives/receipts remain explicit
input gaps; hashes must not be invented from the extracted directory.

The importer additionally requires the original CMake cache, target
`CMakeFiles/*/CMakeSystem.cmake`, Ninja graph, Meson machine/options/target/
compiler/source introspection JSON, generated ThorVG `config.h`, matching
build logs, and stable expected archive hashes. Both source patch approvals
above are required for the currently patched Git roots. Source snapshots,
metadata and artifacts must remain stable throughout import.

Generated zlib headers are taken from
`tgfx/third_party/out/zlib/ios/include/{zlib.h,zconf.h}` with **no architecture
component**, then copied to the import's header snapshot. The input libraries
must contain inspectable arm64 Mach-O objects for iOS simulator platform 7.
Bitcode/thin archives are unsupported by the inspection path. Maximum actual
object deployment requirements are recorded; an artifact above the configured
iOS 15 floor requires an explicit, consumer-compatible higher deployment
decision through `--allow-higher-deployment`.

The prepared output is:

* `.tgfx-deps/receipt.json`: `schema: 1`, `pins` equal to the complete unchanged
  `third-party/tgfx/pins.json` object, and
  `profiles.ios_sim_arm64.manifest_sha256`.
* `.tgfx-deps/ios_sim_arm64/manifest.json`: `schema: 1`,
  `profile: ios_sim_arm64`, matching `pins`, `files` mapping prepared relative
  paths to SHA256, source snapshots/approved patch hashes, exact original
  build metadata/log hashes, effective options, artifact target/deployment
  evidence, SIMD closure, and `vector_backend: apple_coregraphics`.
* `files` covers `lib/tgfx.a`, `lib/libthorvg-1.a`, the SIMD side archive
  retained for evidence, and actual TGFX/ThorVG/WebP/generated-zlib headers.
  `add-giflib` adds `lib/libgif.a`, `include/gif_lib.h`, source/commit/log
  binding, and the updated manifest hash in the root receipt.

The Bazel repository verifies root receipt pins/schema, profile identity,
manifest hash, and selected archive hashes. It exposes the real header tree;
`prepare.py verify` separately checks the complete prepared header/file hash
map. `add-giflib` binds an explicit source commit, actual header, archive and
log hashes; it does not independently inspect giflib object platform or attest
its earlier compiler configuration. That native target validation remains
with the parent.

All receipts describe import-time source/configuration/artifact evidence,
not a hermetic or retrospective build attestation. This handoff does not
claim iOS runtime acceptance.
