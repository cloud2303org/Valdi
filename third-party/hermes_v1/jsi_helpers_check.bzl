"""Compiler-free gate for checked-in JSI HBC, using the pinned VM's headers."""

def _jsi_helpers_check_impl(ctx):
    # Read only CcInfo: no VM/compiler archive becomes an action input or output.
    # The public lean target carries the private headers target transitively.
    headers = ctx.attr.hermes[CcInfo].compilation_context.headers.to_list()
    versions = [h for h in headers if h.path.endswith("/hermes/BCGen/HBC/BytecodeVersion.h")]
    if len(versions) != 1:
        fail("Expected one BytecodeVersion.h from pinned Hermes, got %s" % versions)
    # Keep the header alongside the inc, inside valdi_hermes' src strip prefix.
    stamp = ctx.actions.declare_file(ctx.label.name + ".h", sibling = ctx.file.inc)
    args = ctx.actions.args()
    args.add("--validate-only")
    args.add("--module", ctx.file.module)
    args.add("--config", ctx.file.config)
    args.add("--bytecode-version-header", versions[0])
    args.add("--source", ctx.file.source)
    args.add("--inc", ctx.file.inc)
    args.add("--symbol", ctx.attr.symbol)
    args.add("--stamp", stamp)
    ctx.actions.run(
        executable = ctx.attr._validator[DefaultInfo].files_to_run,
        arguments = [args],
        inputs = [ctx.file.module, ctx.file.config, versions[0], ctx.file.source, ctx.file.inc],
        outputs = [stamp],
        mnemonic = "ValidateHermesJsiHbc",
        progress_message = "Validating embedded Hermes HBC %{label}",
    )
    return [DefaultInfo(files = depset([stamp]))]

jsi_helpers_check = rule(
    implementation = _jsi_helpers_check_impl,
    attrs = {
        "source": attr.label(allow_single_file = [".js"], mandatory = True),
        "inc": attr.label(allow_single_file = [".inc"], mandatory = True),
        "symbol": attr.string(mandatory = True),
        "module": attr.label(default = Label("//:MODULE.bazel"), allow_single_file = True),
        "config": attr.label(default = Label("//third-party/hermes_v1:hermes.BUILD"), allow_single_file = True),
        "hermes": attr.label(default = Label("@hermes//:hermes_lean"), providers = [CcInfo]),
        "_validator": attr.label(
            default = Label("//third-party/hermes_v1:prepare_jsi_helpers"),
            executable = True,
            cfg = "exec",
        ),
    },
)
