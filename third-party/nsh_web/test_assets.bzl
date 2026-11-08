"""Runfile paths for real Node-compiled modules used by the runtime test."""

def _runtime_test_assets_impl(ctx):
    archives = depset(transitive = [dep[DefaultInfo].files for dep in ctx.attr.srcs])
    header = ctx.actions.declare_file(ctx.label.name + ".hpp")
    ctx.actions.write(
        header,
        "#pragma once\nnamespace NativeWebTestAssets {\n" +
        "inline constexpr const char* paths[] = {\n" +
        "".join(["    " + json.encode(file.short_path) + ",\n" for file in archives.to_list()]) +
        "};\n}\n",
    )
    return [DefaultInfo(
        files = depset([header]),
        runfiles = ctx.runfiles(transitive_files = archives),
    )]

runtime_test_assets = rule(
    implementation = _runtime_test_assets_impl,
    attrs = {"srcs": attr.label_list(allow_files = [".valdimodule"])},
)
