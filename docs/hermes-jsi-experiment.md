# Public JSI Hermes adapter experiment

The local `experiment/hermes-jsi` branch adds an optional implementation of
Valdi's existing `IJavaScriptContext` using public JSI and public Hermes
extensions. The experiment builds on maintained Valdi fork commit
`387568f9f333e21ca504354aa96dbd507843e455`, with Hermes `260318099.0.4` (HBC 99).
The downstream application follows that maintenance commit; this backend remains
opt-in and is not selected by the application pin.

## Selection and implementation

Enable `--//bzl/valdi:hermes_use_jsi=true` when building this repository as the
main module, or `--@valdi//bzl/valdi:hermes_use_jsi=true` when consumed as `@valdi`.
The factory then creates `HermesJsiJavaScriptContext`. The legacy context,
compiler, VM utilities, and bytecode cache translation units are excluded from
that build. Without the flag, the existing implementation remains selected.

The adapter retains Valdi module loading, native binding contracts, exception
tracking, Long wrappers, scheduling, and worker ownership. Native classes and
property descriptors use captured JavaScript intrinsics; typed-array kind uses
the intrinsic typed-array tag getter. External buffers retain native backing
storage, and typed-array conversions preserve offsets and backing ownership.
Unsupported BigInt typed arrays produce conversion errors.

Hermes background finalizers enqueue native-owner cleanup into a private queue.
An asynchronous, coalesced wakeup drains it on the serial dispatch queue on
which the context was created. The callback retains neither the runtime nor its
scheduler. VM entry/exit, explicit GC, and teardown also drain it.
Weak JSI handles are invalidated before runtime destruction; the final cleanup
drain follows Hermes executor shutdown. Teardown invalidates queued wakeups
before destroying the runtime. Bare contexts created outside a Valdi dispatch
queue retain explicit boundary/GC/teardown cleanup.

Native Promise ownership uses public JSI weak handles and a small pinned Hermes
Promise-builtin extension shared with the maintained adapter. The builtin
notifies adoption, transfers ownership before outer-promise collection, skips
new owned rejections, and retires previously tracked owned rejection records.
Marking ownership itself runs no JS and reads no user properties. The source
patch, its compiled internal HBC, and the adapter protocol must travel together.
Weak-table scans are linear; propagation across a large live graph can be quadratic.

`HermesJsiJavaScriptContext.{hpp,cpp}`, `HermesJsiNativeBindings.cpp`, and
`HermesJsiHelpers.js` implement the adapter. They do not include private VM
headers or call `getVMRuntimeUnsafe()`. Public `RuntimeConfig`, instrumentation,
timeout interruption, and the existing public CDP integration remain
Hermes-specific dependencies. JSI does not guarantee binary compatibility
across Hermes releases.

Full builds link the public `compileJS` implementation for Valdi's runtime
precompilation API. Lean builds link `hermes_lean` and `hermes_api_lean` and use
precompiled HBC helpers. Source evaluation/precompilation is unavailable in
lean builds. `hermes_force_lean=true` allows host tests to exercise this linkage
despite desktop builds normally selecting the full engine.

## Regenerate embedded bytecode

Use the compiler from the matching `hermes-compiler@260318099.0.4` package:

```sh
python3 third-party/hermes_v1/prepare_jsi_helpers.py --hermesc /absolute/path/to/hermesc
python3 third-party/hermes_v1/prepare_jsi_helpers.py --hermesc /absolute/path/to/hermesc --check
```

The script regenerates both the runtime helper and the lean-compatible smoke
fixture. Inputs, compiler release, HBC version, and source digest are recorded
in checked-in `.inc` files. Async-break checks remain enabled so pure JS loops
can be interrupted through public Hermes APIs.

Bazel now validates source digests, pins, HBC headers, file length, and footer
checksums automatically before compiling JSI code/tests. The maintained
`internal_bytecode` target separately validates its sourceHash against the exact
sorted concatenation of the patched Hermes repository JS sources. These checks
run without `hermesc` or a runtime compiler dependency. A stale-helper build
probe was rejected as expected. Regenerate the internal builtin explicitly with
`third-party/hermes_v1/regenerate_internal_bytecode.py` and matching official
Hermes source/compiler when changing the builtin patch.

## Host validation commands

From a downstream Bazel module that consumes Valdi, point `--override_module`
at this checkout. Use an isolated output base and external symlink prefix if
the application checkout is shared:

```sh
bazelisk --output_base=/private/var/tmp/vnr-hermes-boundary-build test \
  @valdi//valdi:test_hermes_jsi \
  --override_module=valdi=/absolute/path/to/hermes-jsi \
  --@valdi//bzl/valdi:hermes_use_jsi=true \
  --lockfile_mode=off --symlink_prefix=/private/var/tmp/vnr-hermes-jsi-links/ \
  --test_filter='*Hermes*' --test_output=errors --jobs=6

bazelisk --output_base=/private/var/tmp/vnr-hermes-boundary-build test \
  @valdi//valdi:test_hermes_jsi_lean \
  --override_module=valdi=/absolute/path/to/hermes-jsi \
  --@valdi//bzl/valdi:hermes_use_jsi=true \
  --@valdi//bzl/valdi:hermes_force_lean=true \
  --lockfile_mode=off --symlink_prefix=/private/var/tmp/vnr-hermes-jsi-lean-links/ \
  --test_filter='*Hermes*' --test_output=errors --jobs=6
```

The full target includes existing integration/native-class tests and boundary
regressions. The lean target executes raw embedded HBC through the factory,
covering binary views, intrinsic capture, native identity, descriptors,
exceptions, lifetime handling, and cross-thread pure-JS interruption.

Validated on the macOS arm64 host on 2026-11-01:

| Check | Result |
| --- | --- |
| Full JSI integration target | 99 passed, 8 existing engine-specific skips |
| Lean JSI integration target | 16 passed, 0 skipped; 10 consecutive runs (160 case executions) passed |
| Maintained legacy adapter boundary target | 13 passed |
| Legacy lean tests with adapter-source ASan instrumentation | 16 passed; reproduced pre-fix shutdown UAF now absent |
| Compiler-free bytecode validator Python tests | 16 helper tests and 20 internal-builtin tests passed |
| Three finalizer regressions repeated 20 times in lean | 60 executions passed |
| Blocked debugger-command/removal regression repeated 10 times | 10 executions passed |
| Embedded HBC regeneration check | Both files match |
| Lean binary symbol inspection | JSI adapter present; legacy context, `compileJS`, CDP Agent absent |

The skipped full tests retain upstream engine conditions; they are not counted
as validated functionality. A separate boundary test covers rejection reason
identity through the Hermes rejection-tracker timer callback. The unsafe VM
accessor can still appear as an implementation symbol inside Hermes itself;
the JSI adapter does not call it.

Debugger removal now waits for in-flight command submission before destroying
the agent, debug API, and runtime on the owner JS thread. Same-stack teardown
from inside an active debugger command remains unsupported and fails explicitly.
This concurrency regression uses a controlled native scheduler; it does not
exercise a WebSocket client or a complete interactive debugger session.

## Boundary microbenchmark

`test_hermes_adapter_benchmark` uses the same public context, JS fixtures,
compiler/VM, workload checksums, warmups, and seven samples per workload for
both factory selections. Run with `-c opt`, `--nocache_test_results`,
`--test_filter='*HermesAdapterBenchmark*'`, and
`--test_arg=--gtest_also_run_disabled_tests`; toggle `hermes_use_jsi` only.
Reported ns/op includes natural GC and handle destruction, excluding context
creation and source evaluation. A property set/get is a round trip; JS-to-native
timing includes the JS loop. Legacy and optimized JSI were rerun on the final
maintenance baseline; initial JSI records the pre-optimization snapshot:

| Workload, median ns/op | Legacy | Initial JSI | Optimized JSI |
| --- | ---: | ---: | ---: |
| Number create/read | 6.48 | 85.57 | 6.19 |
| Short string create/read | 56.05 | 192.13 | 104.10 |
| Property get/int | 14.37 | 91.56 | 16.10 |
| Property set/get/int | 23.06 | 102.70 | 28.05 |
| Native to JS increment | 40.06 | 161.25 | 46.18 |
| Batched JS to native increment | 40.03 | 187.90 | 45.29 |

These small host measurements establish the removed boundary overhead, not
application-level performance or a universal advantage over the legacy adapter.

## Validation boundary

This is an experimental adapter. Host tests establish compiled and linked
behavior for their exercised paths. They do not establish iOS/Android app
integration, device behavior, debugger-client interoperability, release startup
time, memory, bundle size, or end-to-end performance relative to the existing adapter.
Those require separate application builds and measurements before changing the
default implementation.

Primitive payloads now use Valdi's own 16-byte encoding, without depending on
the layout of JSI values. Pointer values and property names retain owned handles.
Common scalar/string conversions use public native fast paths; object coercion
continues through captured JS helpers. String/binary-heavy boundaries and large
Promise graphs still need representative application measurements.
