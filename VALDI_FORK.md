# ValdiNativeRoutes framework maintenance

This fork retains upstream Git history. `valdiroutes` is the maintained runtime branch; `main` remains the upstream branch. The application pins an immutable commit from this branch.

## Current upstream base

- Repository: https://github.com/Snapchat/Valdi
- Tag: `beta-0.2.1`
- Commit: `f26bee4dc170b6c929f71d4dd07d44e033f47176`

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
git fetch upstream --tags
git switch -c upgrade/valdi-TAG origin/valdiroutes
git merge --no-ff TAG
```

Resolve conflicts in source, retaining only customizations still needed. Update the upstream base recorded above. Do not force-sync the maintained branch over its custom commits. Push the upgrade branch, then pin that exact commit in ValdiNativeRoutes with `node scripts/upstream.mjs pin COMMIT` and run its application CI (npm, Android and iOS). Validate Release bytecode and native behavior before promoting the upgrade to `valdiroutes`.

The application owns its pages, host code and Node compiler port. Its companion diff report and binding goldens still need review when upstream code generation changes. Framework Actions are disabled in this fork; application CI is the validation entry point.
