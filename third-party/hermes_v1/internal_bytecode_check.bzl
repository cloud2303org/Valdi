"""Validate prepared internal HBC without depending on any Hermes VM target."""

def _internal_bytecode_check_impl(ctx):
    # ONLY a standalone filegroup's DefaultInfo: CcInfo would cycle through
    # Hermes' headers -> @valdi internal_bytecode -> this validation rule.
    files = ctx.attr.sources[DefaultInfo].files.to_list()
    headers = [f for f in files if f.path.endswith("/include/hermes/BCGen/HBC/BytecodeVersion.h")]
    sources = [f for f in files if f.extension == "js" and f.dirname.endswith("/lib/InternalJavaScript")]
    if len(headers) != 1 or not sources or len(files) != len(headers) + len(sources):
        fail("Expected InternalJavaScript/*.js and one BytecodeVersion.h from Hermes source filegroup")
    stamp = ctx.actions.declare_file(ctx.label.name + ".h", sibling = ctx.file.inc)
    args = ctx.actions.args()
    args.add("--inc", ctx.file.inc)
    args.add("--bytecode-version-header", headers[0])
    args.add_all(sources, before_each = "--source")
    args.add("--stamp", stamp)
    ctx.actions.run(
        executable = ctx.attr._validator[DefaultInfo].files_to_run,
        arguments = [args],
        inputs = depset([ctx.file.inc], transitive = [ctx.attr.sources[DefaultInfo].files]),
        outputs = [stamp],
        mnemonic = "ValidateHermesInternalHbc",
        progress_message = "Validating patched Hermes internal HBC %{label}",
    )
    return [DefaultInfo(files = depset([stamp]))]

internal_bytecode_check = rule(
    implementation = _internal_bytecode_check_impl,
    attrs = {
        "inc": attr.label(allow_single_file = [".inc"], mandatory = True),
        "sources": attr.label(default = Label("@hermes//:internal_bytecode_validation_sources")),
        "_validator": attr.label(
            default = Label("//third-party/hermes_v1:check_internal_bytecode"),
            executable = True,
            cfg = "exec",
        ),
    },
)
