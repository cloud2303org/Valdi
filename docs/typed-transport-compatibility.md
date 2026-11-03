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

The consuming application's regression entry points are:

```sh
node --test scripts/tests/upstream-runtime-patches.test.mjs
node --test scripts/tests/android-promise-rejection.test.mjs
```

Historical baseline patches are test fixtures only after the application adopts
this branch's immutable commit. They are not applied again during application
builds. Those tests check that their expected corrected bytes are present in the
pinned fork before exercising the corresponding regression controls.

The focused migration run on November 3, 2026 passed the canonical-file/drift
checks and the actual Kotlin/JNI consumer: all 19 corrected consumer cases
passed, while the baseline reproduced nine verbatim-message failures. This
does not establish a newly exported runtime AAR/framework or device acceptance.
