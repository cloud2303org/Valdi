"""Desktop static resources are outside this native mobile Node backend."""

def _valdi_static_resource_impl(ctx):
    fail("Legacy compiler static-resource generation has been removed")

valdi_static_resource = rule(
    implementation = _valdi_static_resource_impl,
    toolchains = [],
    attrs = {
        "srcs": attr.label_list(
            cfg = "exec",
            doc = "List of files to package as static resources",
            mandatory = True,
        ),
    },
)
