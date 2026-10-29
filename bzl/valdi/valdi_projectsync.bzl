"""Legacy projectsync is unavailable; editor configs come from our Node tools."""
load(":valdi_compiled.bzl", "ValdiModuleInfo")

def _valdi_projectsync_impl(ctx):
    fail("The legacy compiler and projectsync have been removed; run node scripts/prepare-valdi.mjs")

valdi_projectsync = rule(
    implementation = _valdi_projectsync_impl,
    toolchains = [],
    attrs = {
        "target": attr.label(
            doc = "The module target",
            mandatory = True,
            providers = [[ValdiModuleInfo]],
        ),
        "ids_yaml": attr.label(
            doc = "Optional ids.yaml file for this module",
            allow_single_file = ["ids.yaml"],
        ),
        "strings_json_srcs": attr.label_list(
            doc = "String source files for this module",
            allow_files = True,
        ),
        "res": attr.label_list(
            doc = "List of resources for this module",
            allow_files = True,
        ),
        "sql_srcs": attr.label_list(
            doc = "List of SQL files for this module",
            allow_files = [".sql", ".sq", ".sqm", "sql_types.yaml", "sql_manifest.yaml"],
        ),
        "sql_db_names": attr.string_list(),
        "log_level": attr.label(
            doc = "The log level for the Valdi compiler",
            default = "@valdi//bzl/valdi:compilation_log_level",
        ),
        "_template": attr.label(
            doc = "The template config.yaml file",
            allow_single_file = True,
            default = "valdi_config.yaml.tpl",
        ),
        "_native_api_min_version": attr.label(
            default = "@valdi//bzl/valdi:native_api_min_version",
        ),
    },
)
