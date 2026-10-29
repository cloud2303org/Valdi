# Build file for the Hermes V1 source archive, replacing Valdi's third-party/hermes/hermes.BUILD (which targets a
# 2024 Hermes). Source lists come from sources.bzl, generated from Hermes' own CMake build; the files CMake
# generates live in third-party/hermes_v1. Target names match Valdi's so valdi/BUILD.bazel is unchanged.

load(
    "@valdi//third-party/hermes_v1:sources.bzl",
    "API_SRCS",
    "COMPILER_SRCS",
    "DTOA_SRCS",
    "LEAN_BACKEND_SRCS",
    "LLVH_SRCS",
    "VM_SRCS",
)
load("@valdi//third-party/hermes:cmake_configure.bzl", "cmake_configure")

COMPILER_FLAGS_COMPAT = [
    "-O3",  # Always set optimization on
    "-DNDEBUG",
    "-Wno-deprecated-volatile",
    "-Wno-parentheses",
    "-Wno-unused-label",
    "-Wno-shorten-64-to-32",
    "-Wno-deprecated-this-capture",
    "-Wno-deprecated-pragma",
    "-Wno-unused-variable",
    "-Wno-implicit-fallthrough",
    "-Wno-nontrivial-memcall",
    "-Wno-unknown-warning-option",
    # copts are shell-tokenized, so the quotes need escaping to reach the compiler.
    '-DHERMES_RELEASE_VERSION=\\"260318099.0.4\\"',
]

# Hermes' CMake builds C++ as C++17 (C++20 only for MSVC); Valdi's .bazelrc sets C++20, under which newer libc++
# rejects llvh's JSON.cpp. C files (zip, dtoa) must not get this flag.
CXX17 = ["-std=c++17"]

COMPILER_FLAGS = COMPILER_FLAGS_COMPAT + [
    "-fno-exceptions",
]

ZIP_SRCS = [src for src in VM_SRCS if src.startswith("external/zip/")]

VM_CXX_SRCS = [src for src in VM_SRCS if src not in ZIP_SRCS]

LLVH_C_SRCS = [src for src in LLVH_SRCS if src.endswith(".c")]

LLVH_CXX_SRCS = [src for src in LLVH_SRCS if not src.endswith(".c")]

# What Hermes' CMake passes, minus Boost.Context (only lets the compiler run on a separate stack; without it Hermes
# uses a thread) and with Valdi's lite Unicode tables. Heap layout, GC kind and segment size are in
# libhermesvm-config.h.
DEFINES_BASE = [
    "HERMESVM_ALLOW_CONCURRENT_GC",
    "HERMESVM_ALLOW_INLINE_ASM",
    "HERMESVM_INDIRECT_THREADING",
    "HERMES_CHECK_NATIVE_STACK",
    "HERMES_ENABLE_UNICODE_REGEXP_PROPERTY_ESCAPES",
    "HERMES_USE_BOOST_CONTEXT=0",
    "STATIC_HERMES",
    "HERMES_PLATFORM_UNICODE=HERMES_PLATFORM_UNICODE_LITE",
]

DEFINES_FULL = DEFINES_BASE + [
    "HERMES_ENABLE_DEBUGGER",
    "HERMES_MEMORY_INSTRUMENTATION",
]

# Hermes V1 dropped HERMESVM_LEAN, but Valdi's adapter still uses it to leave out the compiler path in lean builds.
DEFINES_LEAN = DEFINES_BASE + ["HERMESVM_LEAN"]

cc_library(
    name = "headers",
    hdrs = glob([
        "include/**/*.h",
        "include/**/*.def",
        "include/**/*.inc",
        "public/**/*.h",
        "API/hermes/**/*.h",
        "API/jsi/**/*.h",
        "external/zip/**/*.h",
        "external/dtoa/**/*.h",
        "external/dragonbox/**/*.h",
        "external/fast_float/**/*.h",
    ]),
    includes = [
        "API",
        "API/jsi",
        "external",
        "external/dragonbox",
        "external/fast_float",
        "include",
        "public",
    ],
    deps = [
        "@valdi//third-party/hermes_v1:internal_bytecode",
        "@valdi//third-party/hermes_v1:platform_headers",
    ],
)

ALWAYS_ENABLED = ["//conditions:default"]

cmake_configure(
    name = "llvm-config.h",
    src = "external/llvh/include/llvh/Config/llvm-config.h.cmake",
    cmake_vars = {
        "LLVM_HOST_TRIPLE": {
            "@valdi//bzl/conditions:ios_arm64": "arm-apple-darwin22.6.0",
            "@valdi//bzl/conditions:ios_arm64e": "arm-apple-darwin22.6.0",
            "@valdi//bzl/conditions:ios_x86_64": "x86_64-apple-darwin23.0.0",
            "@valdi//bzl/conditions:ios_arm64_sim": "arm-apple-darwin22.6.0",
            "@valdi//bzl/conditions:macos_x86_64": "x86_64-apple-darwin23.0.0",
            "@valdi//bzl/conditions:macos_arm64": "arm-apple-darwin22.6.0",
            "@valdi//bzl/conditions:android_arm32": "arm-none-android",
            "@valdi//bzl/conditions:android_arm64": "arm-none-android",
            "@valdi//bzl/conditions:android_x64": "x86_64-none-android",
            "@valdi//bzl/conditions:linux": "x86_64-unknown-linux-gnu",
        },
        "LLVM_VERSION_MAJOR": {"//conditions:default": ""},
        "LLVM_VERSION_MINOR": {"//conditions:default": ""},
        "LLVM_VERSION_PATCH": {"//conditions:default": ""},
        "PACKAGE_VERSION": {"//conditions:default": "8.0.0svn"},
        "LLVM_ON_UNIX": {"//conditions:default": "1"},
    },
    cmakedefines = {
        "LLVM_ENABLE_DUMP": [],
        "LINK_POLLY_INTO_TOOLS": [],
        "LLVM_DEFAULT_TARGET_TRIPLE": [],
        "LLVM_NATIVE_ARCH": [],
        "LLVM_NATIVE_ASMPARSER": [],
        "LLVM_NATIVE_ASMPRINTER": [],
        "LLVM_NATIVE_DISASSEMBLER": [],
        "LLVM_NATIVE_TARGET": [],
        "LLVM_NATIVE_TARGETINFO": [],
        "LLVM_NATIVE_TARGETMC": [],
        "LLVM_ON_UNIX": ALWAYS_ENABLED,
        "LLVM_HOST_TRIPLE": ALWAYS_ENABLED,
    },
    cmakedefines_01 = {
        "LLVM_ENABLE_THREADS": ALWAYS_ENABLED,
        "LLVM_HAS_ATOMICS": ALWAYS_ENABLED,
        "LLVM_ON_UNIX": ALWAYS_ENABLED,
        "LLVM_USE_INTEL_JITEVENTS": [],
        "LLVM_USE_OPROFILE": [],
        "LLVM_USE_PERF": [],
        "LLVM_FORCE_ENABLE_STATS": [],
    },
    output = "llvh/Config/llvm-config.h",
)

cc_library(
    name = "zip",
    srcs = ZIP_SRCS,
    copts = COMPILER_FLAGS,
    tags = ["app_size_owners=VALDI"],
    deps = [":headers"],
)

cc_library(
    name = "llvh_c",
    srcs = LLVH_C_SRCS,
    hdrs = glob([
        "external/llvh/include/**/*.h",
        "external/llvh/lib/**/*.h",
        "external/llvh/lib/**/*.inc",
    ]),
    copts = COMPILER_FLAGS,
    includes = ["external/llvh/include"],
    tags = ["app_size_owners=VALDI"],
    deps = [
        ":llvm-config.h",
        "@valdi//third-party/hermes_v1:platform_headers",
    ],
)

cc_library(
    name = "llvh",
    srcs = LLVH_CXX_SRCS,
    hdrs = glob([
        "external/llvh/include/**/*.h",
        "external/llvh/include/**/*.def",
        "external/llvh/include/**/*.inc",
        "external/llvh/gen/include/**/*.inc",
        "external/llvh/lib/**/*.h",
        "external/llvh/lib/**/*.inc",
    ]),
    copts = COMPILER_FLAGS + CXX17,
    includes = [
        "external/llvh/gen/include",
        "external/llvh/include",
    ],
    tags = ["app_size_owners=VALDI"],
    deps = [
        ":llvh_c",
        ":llvm-config.h",
        "@valdi//third-party/hermes_v1:platform_headers",
    ],
)

cc_library(
    name = "dtoa",
    srcs = DTOA_SRCS,
    hdrs = glob([
        "external/dtoa/**/*.h",
        "external/dtoa/**/*.inc",
    ]),
    copts = COMPILER_FLAGS,
    include_prefix = "dtoa",
    local_defines = [
        "IEEE_8087",
        "Long=int",
        "NO_HEX_FP",
        "NO_INFNAN_CHECK",
        "MULTIPLE_THREADS",
    ],
    tags = ["app_size_owners=VALDI"],
)

LIB_HDRS = glob([
    "lib/**/*.h",
    "lib/**/*.inc",
    "lib/**/*.def",
])

# VM plus compiler: can run JS source. Used by debug builds and desktop tools (bytecode precompilation).
cc_library(
    name = "hermes",
    srcs = VM_CXX_SRCS + COMPILER_SRCS,
    hdrs = LIB_HDRS,
    copts = COMPILER_FLAGS + CXX17,
    defines = DEFINES_FULL,
    tags = ["app_size_owners=VALDI"],
    visibility = ["//visibility:public"],
    deps = [
        ":dtoa",
        ":headers",
        ":llvh",
        ":zip",
    ],
)

# VM only: runs precompiled bytecode.
cc_library(
    name = "hermes_lean",
    srcs = VM_CXX_SRCS + LEAN_BACKEND_SRCS,
    hdrs = LIB_HDRS,
    copts = COMPILER_FLAGS + CXX17,
    defines = DEFINES_LEAN,
    tags = ["app_size_owners=VALDI"],
    visibility = ["//visibility:public"],
    deps = [
        ":dtoa",
        ":headers",
        ":llvh",
        ":zip",
    ],
)

# The Chrome DevTools agent, which CMake adds to the API library when HERMES_ENABLE_DEBUGGER is on (sources.bzl is
# generated from a build without it). Only the full API enables the debugger.
CDP_SRCS = ["API/hermes/cdp/{}.cpp".format(name) for name in [
    "CDPAgent",
    "CDPDebugAPI",
    "CallbackOStream",
    "ConsoleMessage",
    "DebuggerDomainAgent",
    "DebuggerDomainCoordinator",
    "DomainState",
    "HeapProfilerDomainAgent",
    "JSONValueInterfaces",
    "MessageConverters",
    "MessageInterfaces",
    "MessageTypes",
    "ProfilerDomainAgent",
    "RemoteObjectConverters",
    "RemoteObjectsTable",
    "RuntimeDomainAgent",
]]

# jsi::Runtime on top of the VM (debugger APIs, JSI extensions). hermes.cpp implements jsi, so jsi.cpp is linked too.
cc_library(
    name = "hermes_api",
    srcs = API_SRCS + CDP_SRCS + ["API/jsi/jsi/jsi.cpp"],
    hdrs = glob(["API/hermes/cdp/*.h"]),
    copts = COMPILER_FLAGS_COMPAT + CXX17,
    defines = DEFINES_FULL,
    # Valdi has no fbjni; see no_fbjni.patch.
    local_defines = ["HERMES_NO_FBJNI"],
    tags = ["app_size_owners=VALDI"],
    visibility = ["//visibility:public"],
    deps = [
        ":headers",
        ":llvh",
        "@valdi//third-party/hermes_v1:extensions_bytecode",
    ],
)

cc_library(
    name = "hermes_api_lean",
    srcs = API_SRCS + ["API/jsi/jsi/jsi.cpp"],
    copts = COMPILER_FLAGS_COMPAT + CXX17,
    defines = DEFINES_LEAN,
    # Valdi has no fbjni; see no_fbjni.patch.
    local_defines = ["HERMES_NO_FBJNI"],
    tags = ["app_size_owners=VALDI"],
    visibility = ["//visibility:public"],
    deps = [
        ":headers",
        ":llvh",
        "@valdi//third-party/hermes_v1:extensions_bytecode",
    ],
)
