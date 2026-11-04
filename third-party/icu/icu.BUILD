# Work in progress

genrule(
    name = "icu_data_bytes",
    srcs = ["source/data/in/icudt68l.dat"],
    outs = ["icudt68_data.inc"],
    cmd = 'cp "$(location source/data/in/icudt68l.dat)" "$@"',
)

genrule(
    name = "icu_data_asm",
    srcs = [":icu_data_bytes"],
    outs = ["icudt68_data.S"],
    cmd = """cat > "$@" <<'EOF'
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The pinned ICU data package is little endian"
#endif
#if defined(__APPLE__)
.section __TEXT,__const
.globl _icudt68_dat
.p2align 4
_icudt68_dat:
#else
.section .rodata
.globl icudt68_dat
.type icudt68_dat, %object
.p2align 4
icudt68_dat:
#endif
.incbin "$(execpath :icu_data_bytes)"
#if !defined(__APPLE__)
.size icudt68_dat, .-icudt68_dat
.section .note.GNU-stack,"",%progbits
#endif
EOF
""",
)

# TGFX links the real pinned data package independently of Skia.
cc_library(
    name = "icu_data",
    srcs = [":icu_data_asm"],
    textual_hdrs = [":icu_data_bytes"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "icu",
    srcs = glob([
        "source/common/**/*.cpp",
    ]),
    hdrs = glob(
        [
            "source/common/**/*.h",
        ],
    ),
    copts = [
        "-Wno-deprecated-pragma",
        "-Wno-unused-but-set-variable",
        "-Wno-unused-private-field",
        "-Wno-deprecated-anon-enum-enum-conversion",
        "-Wno-unused-variable",
        "-Wno-deprecated-declarations",
        "-Wno-unknown-warning-option",
        "-Wno-deprecated-enum-enum-conversion",
    ],
    local_defines = [
        "UCONFIG_NO_BREAK_ITERATION=1",
        "UCONFIG_NO_LEGACY_CONVERSION=1",
        "U_CHARSET_IS_UTF8=1",
        "U_CHAR_TYPE=uint_least16_t",
        "U_HAVE_ATOMIC=1",
        "U_HAVE_STRTOD_L=0",
        "_REENTRANT",
        "U_COMMON_IMPLEMENTATION",
    ],
    strip_include_prefix = "source/common",
    visibility = ["//visibility:public"],
    deps = [],
)
