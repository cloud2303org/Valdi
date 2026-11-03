# Typed transport compatibility

This topic branch starts at `15feb6530ceecb971f4c8519ea8ab77474b959db`,
the consuming application's existing runtime pin. It contains the framework
changes needed by the Android/iOS structured-object migration, without the
separate Harmony implementation or later compiler migrations.

## Runtime changes

- Native maps preserve an own `__proto__` data property instead of invoking
  JavaScript's legacy prototype setter. Both untyped `valueToJSValue` conversion
  and the generated-model `JavaScriptValueDelegate` map path use the same helper.
- The JavaScript intrinsics used by that helper are captured during context
  initialization. The helper does not evaluate JavaScript source, so lean
  bytecode-only Hermes remains supported.
- Foundation `CFBoolean` values become Valdi booleans; numeric `NSNumber`
  zero and one remain numeric values.
- Android producers may use `PromiseRejectionException` to opt into verbatim
  rejection text, including the empty string. Other exceptions retain their
  diagnostic cause chains. Existing error-code and cancellation meanings remain
  unchanged.

Application route/storage/wheel schemas, generated application bindings, native
screens, navigation, BLE services and UI components belong to
`cloud2303org/ValdiNativeRoutes`; they are not framework sources in this branch.

## Regression evidence

The generic regression drivers and real consumer fixtures now live in this
fork, outside the main runtime test glob:

```sh
node --test --test-concurrency=1 tools/validation/typed-transport/*.test.mjs
```

The drivers compile/read this checkout's production sources directly. Baseline
negative controls use `git show` at the base commit above; no production patch
application occurs in the tests. The normal source/Kotlin checks do not depend
on an application checkout. Linked full/LEAN Hermes and Foundation host
consumers require explicit, compatible existing build-cache inputs and skip
when those inputs are absent. Prerequisites, commands, fixture hashes, and
evidence boundaries are documented in
[`tools/validation/typed-transport/README.md`](../tools/validation/typed-transport/README.md).

On November 3, 2026, the fork's default entry point passed five tests and
explicitly skipped the two optional native-cache tests. The actual Kotlin/JNI
consumer passed all 19 current-source cases; substituting the baseline callback
reproduced exactly nine verbatim-message failures.

A separate sequential host run with explicit existing cache inputs passed both
native tests. Full and LEAN Hermes passed both current-source map paths, while
independently substituting each baseline map call site failed the LEAN consumer.
The full current Objective-C conversion unit compiled, and its extracted
NSNumber branch passed the linked Foundation consumer; the baseline branch
failed on `CFBoolean`. This used previously built macOS arm64 dependency
archives and Hermes `260318099.0.4`, recompiling the current fork's tested units
against its headers. It did not rebuild the dependency caches.

These checks do not establish a newly exported runtime AAR/framework, complete
Objective-C bridge linkage, Android native-to-JavaScript Promise delivery, or
device acceptance. No full runtime export, whole-workspace Bazel build, or
Harmony implementation/build/test was performed.
