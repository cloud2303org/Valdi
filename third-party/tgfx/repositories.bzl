"""Local, lazily materialized official-source TGFX/ThorVG archives."""

def _tgfx_prepared_repository_impl(ctx):
    root = ctx.path(ctx.attr._workspace_marker).dirname.get_child(".tgfx-deps")
    receipt_path = root.get_child("receipt.json")
    if not receipt_path.exists:
        fail(
            "TGFX was selected but .tgfx-deps/receipt.json is missing. " +
            "Build and prepare the selected profile with " +
            "python3 third-party/tgfx/prepare.py build --help. " +
            "The default Skia backend does not require these inputs.",
        )
    receipt = json.decode(ctx.read(receipt_path))
    pins = json.decode(ctx.read(ctx.attr._pins))
    if receipt.get("schema") != 1 or receipt.get("pins") != pins:
        fail("TGFX prepared receipt does not match third-party/tgfx/pins.json; prepare again.")
    profiles = receipt.get("profiles", {})
    if not profiles:
        fail("TGFX receipt has no built profiles.")
    test_defines = {}
    for profile in profiles:
        if profile not in pins["profiles"]:
            fail("Unknown TGFX profile in receipt: " + profile)
        profile_root = root.get_child(profile)
        manifest_path = profile_root.get_child("manifest.json")
        if not manifest_path.exists:
            fail("TGFX profile manifest is missing: " + profile)
        # file:// copies verify local archive/manifest hashes without a network
        # fetch. Only the prepared include tree is symlinked, not build caches.
        ctx.watch(manifest_path)
        ctx.download(
            url = "file://" + str(manifest_path),
            output = "prepared/" + profile + "/manifest.json",
            sha256 = profiles[profile]["manifest_sha256"],
        )
        manifest = json.decode(ctx.read(manifest_path))
        if manifest.get("pins") != pins or manifest.get("profile") != profile:
            fail("TGFX profile manifest does not match its source pins/profile: " + profile)
        archives = ["lib/tgfx.a", "lib/libthorvg-1.a"]
        if "lib/libgif.a" in manifest["files"]:
            archives.append("lib/libgif.a")
        for archive in archives:
            archive_path = profile_root.get_child(archive)
            ctx.watch(archive_path)
            ctx.download(
                url = "file://" + str(archive_path),
                output = "prepared/" + profile + "/" + archive,
                sha256 = manifest["files"][archive],
            )
        ctx.symlink(profile_root.get_child("include"), "prepared/" + profile + "/include")
        support = manifest.get("test_support")
        if support:
            if support.get("schema") != 1 or support.get("profile") != profile:
                fail("TGFX internal-test support has the wrong schema/profile: " + profile)
            if support.get("archive_sha256") != manifest["files"]["lib/tgfx.a"]:
                fail("TGFX internal-test support belongs to another native archive: " + profile)
            for key in ["tgfx", "tgfx/third_party/concurrentqueue", "tgfx/third_party/skcms"]:
                digest = support["source_snapshots_sha256"][key]
                source = manifest["sources"][key]
                if digest != source.get("tracked_files_sha256", source.get("verified_files_sha256")):
                    fail("TGFX internal-test source snapshot mismatch: " + key)
            native = support["native_configuration"]
            if native["profile"] != profile or native["archive_sha256"] != support["archive_sha256"]:
                fail("TGFX internal-test native configuration profile/archive mismatch: " + profile)
            if support["native_configuration"]["defines"] != support["defines"]:
                fail("TGFX internal-test definitions differ from the native configuration: " + profile)
            for header, digest in native["abi_headers_sha256"].items():
                prepared_header = header
                if header.startswith("src/"):
                    prepared_header = "test_include/tgfx_src/" + header[len("src/"):]
                if manifest["files"].get(prepared_header) != digest:
                    fail("TGFX internal-test ABI header differs from native snapshot: " + header)
            required = [
                "test_include/tgfx_test_native_profile.h",
                "test_include/tgfx_src/gpu/DrawingManager.h",
                "test_include/tgfx_src/gpu/ProxyProvider.h",
                "test_include/tgfx_src/gpu/VertexProvider.h",
                "test_include/tgfx_src/gpu/tasks/GPUBufferUploadTask.h",
                "test_include/tgfx_src/core/utils/TaskGroup.h",
                "test_include/concurrentqueue/concurrentqueue.h",
                "test_include/concurrentqueue/lightweightsemaphore.h",
                "test_include/skcms/skcms.h",
                "test_include/skcms/src/skcms_public.h",
            ]
            for header in required:
                if header not in support["headers_sha256"]:
                    fail("TGFX internal-test receipt is missing a required header: " + header)
            # Watch the prepared snapshot as one input tree; all copied members
            # below still require their individual manifest SHA256.
            ctx.watch_tree(profile_root.get_child("test_include"))
            header_copies = []
            for header, digest in support["headers_sha256"].items():
                if not header.startswith("test_include/") or ".." in header.split("/"):
                    fail("Invalid TGFX internal-test header path: " + header)
                if manifest["files"].get(header) != digest:
                    fail("TGFX internal-test header hash differs from the profile manifest: " + header)
                header_path = profile_root.get_child(header)
                header_copies.append(ctx.download(
                    url = "file://" + str(header_path),
                    output = "prepared/" + profile + "/" + header,
                    sha256 = digest,
                    block = False,
                ))
            # Keep each hash check, while allowing independent local copies to
            # use Bazel's download queue instead of serial repository waits.
            for copy in header_copies:
                copy.wait()
            test_defines[profile] = support["defines"]
    ctx.file(
        "test_support_config.bzl",
        "TEST_SUPPORT_DEFINES = " + repr(test_defines) + "\n",
    )
    ctx.symlink(ctx.attr._build_file, "BUILD.bazel")
    ctx.file("WORKSPACE.bazel", "")

tgfx_prepared_repository = repository_rule(
    implementation = _tgfx_prepared_repository_impl,
    attrs = {
        "_workspace_marker": attr.label(default = Label("//:MODULE.bazel")),
        "_pins": attr.label(default = Label("//third-party/tgfx:pins.json")),
        "_build_file": attr.label(default = Label("//third-party/tgfx:prepared.BUILD.bazel")),
    },
    local = True,
)
