"""Expose real Node-compiled module archives to native runtime tests."""

def _runtime_test_assets_impl(ctx):
    archives = depset(transitive = [dep[DefaultInfo].files for dep in ctx.attr.srcs])
    files = archives.to_list()
    header = ctx.actions.declare_file(ctx.label.name + ".hpp")
    runfile_paths = [
        file.short_path[3:] if file.short_path.startswith("../") else ctx.workspace_name + "/" + file.short_path
        for file in files
    ]
    ctx.actions.write(
        header,
        "#pragma once\nnamespace " + ctx.attr.namespace + " {\n" +
        "inline constexpr const char* paths[] = {\n" +
        "".join(["    " + json.encode(file.short_path) + ",\n" for file in files]) +
        "};\ninline constexpr const char* runfilePaths[] = {\n" +
        "".join(["    " + json.encode(path) + ",\n" for path in runfile_paths]) +
        "};\n}\n",
    )
    return [DefaultInfo(
        files = depset([header]),
        runfiles = ctx.runfiles(transitive_files = archives),
    )]

runtime_test_assets = rule(
    implementation = _runtime_test_assets_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".valdimodule"]),
        "namespace": attr.string(default = "ValdiRuntimeTestAssets"),
    },
)
