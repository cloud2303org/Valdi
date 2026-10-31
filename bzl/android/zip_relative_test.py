"""Regression tests for bzl/android/zip_relative.sh.

These exercise the packaging helper the same way the Android rules call it:
zipper, java home, prefix to strip, output zip, then the inputs. The important
case is the shape Bazel's Linux sandbox uses for a TreeArtifact input: a real
directory whose entries are all symlinks back to the execroot copy. A directory
reached through a symlink is covered too, since both forms appear between the
rules' actions.

Runs under ``bazel test`` (paths come from ``$(location ...)``) and also
standalone: ``python3 zip_relative_test.py --script ./zip_relative.sh --zipper <zipper>``.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = None
ZIPPER = None


def _write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


class ZipRelativeTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="zip_relative_test_")
        self.addCleanup(shutil.rmtree, self.work, True)
        # A real tree with a nested directory, mirroring an unpacked asset dir.
        self.tree = os.path.join(self.work, "tree")
        _write(os.path.join(self.tree, "a.txt"), b"alpha")
        _write(os.path.join(self.tree, "sub", "b.bin"), b"\x00\x01beta")
        _write(os.path.join(self.tree, "sub", "deep", "c.json"), b"{}")
        # A standalone file and a symlinked view of the tree, which is what a
        # sandboxed action sees instead of a materialized TreeArtifact.
        self.single = os.path.join(self.work, "single.txt")
        _write(self.single, b"single")
        self.linked = os.path.join(self.work, "link")
        os.symlink(self.tree, self.linked)

    def zip(self, relative_dir, inputs, name="out.zip"):
        output = os.path.join(self.work, name)
        result = subprocess.run(
            ["/bin/bash", SCRIPT, ZIPPER, "unused-java-home", relative_dir, output, *inputs],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            self.fail(
                "zip_relative.sh failed (%d)\nstdout: %s\nstderr: %s"
                % (result.returncode, result.stdout, result.stderr)
            )
        with zipfile.ZipFile(output) as archive:
            return {info.filename: archive.read(info.filename) for info in archive.infolist()}

    def test_real_directory_entries_and_contents(self):
        entries = self.zip(self.tree, [self.tree])
        self.assertEqual(
            {"a.txt": b"alpha", "sub/b.bin": b"\x00\x01beta", "sub/deep/c.json": b"{}"},
            entries,
        )

    def test_symlinked_directory_matches_real_directory(self):
        # Both symlink forms have to produce identical archives: the starting
        # point itself, and the per-entry links used by a sandboxed action.
        self.assertEqual(self.zip(self.tree, [self.tree], "real.zip"),
                         self.zip(self.linked, [self.linked], "linked.zip"))

    def test_symlinked_directory_keeps_relative_archive_names(self):
        entries = self.zip(self.work, [self.linked])
        self.assertEqual(
            {
                "link/a.txt": b"alpha",
                "link/sub/b.bin": b"\x00\x01beta",
                "link/sub/deep/c.json": b"{}",
            },
            entries,
        )

    def test_plain_file_input(self):
        entries = self.zip(self.work, [self.single])
        self.assertEqual({"single.txt": b"single"}, entries)

    def test_mixed_file_and_directory_inputs(self):
        entries = self.zip(self.work, [self.single, os.path.join(self.tree, "a.txt")])
        self.assertEqual({"single.txt": b"single", "tree/a.txt": b"alpha"}, entries)

    def _sandbox_treeartifact(self):
        """Reproduce how Bazel hands a TreeArtifact to a sandboxed action.

        The action's working directory holds a real directory whose every entry
        is a symlink back to the execroot copy of the same file.
        """
        execroot = os.path.join(self.work, "execroot", "bin", "assets_dir")
        for name, data in (
            ("app_pages.valdimodule", b"module-app"),
            ("app_pages.map.json", b"{\"map\":1}"),
            ("sub", None),
        ):
            if data is None:
                _write(os.path.join(execroot, name, "deep.valdimodule"), b"module-deep")
            else:
                _write(os.path.join(execroot, name), data)
        sandbox = os.path.join(self.work, "sandbox", "bin", "assets_dir")
        os.makedirs(os.path.join(sandbox, "sub"))
        for rel in ("app_pages.valdimodule", "app_pages.map.json", "sub/deep.valdimodule"):
            os.symlink(os.path.join(execroot, rel), os.path.join(sandbox, rel))
        return execroot, sandbox

    def test_treeartifact_symlink_entries_are_packaged(self):
        # Regression for the real failure: collect_android_assets zipped a
        # TreeArtifact whose entries are symlinks, the walk saw no regular
        # files, and the zipper aborted with "Can't create zip without input
        # files specified".
        execroot, sandbox = self._sandbox_treeartifact()
        entries = self.zip(sandbox, [sandbox])
        self.assertEqual(
            {
                "app_pages.valdimodule": b"module-app",
                "app_pages.map.json": b'{"map":1}',
                "sub/deep.valdimodule": b"module-deep",
            },
            entries,
        )

    def test_treeartifact_matches_materialized_directory(self):
        # The sandboxed action must produce the same archive as a local
        # (unsandboxed) run over the real execroot files.
        execroot, sandbox = self._sandbox_treeartifact()
        self.assertEqual(
            self.zip(execroot, [execroot], "execroot.zip"),
            self.zip(sandbox, [sandbox], "sandbox.zip"),
        )

    def test_entry_names_never_leak_resolved_paths(self):
        execroot, sandbox = self._sandbox_treeartifact()
        prefix = os.path.relpath(sandbox, self.work)
        entries = self.zip(self.work, [sandbox], "prefix.zip")
        self.assertEqual(
            {
                prefix + "/app_pages.valdimodule": b"module-app",
                prefix + "/app_pages.map.json": b'{"map":1}',
                prefix + "/sub/deep.valdimodule": b"module-deep",
            },
            entries,
        )
        for name in entries:
            self.assertNotIn("execroot", name)

    def test_symlink_loop_inside_tree_does_not_stall_the_walk(self):
        tree = os.path.join(self.work, "loop")
        _write(os.path.join(tree, "real.txt"), b"real")
        os.symlink(tree, os.path.join(tree, "self"))
        entries = self.zip(tree, [tree])
        self.assertEqual({"real.txt": b"real"}, entries)

    def test_dangling_link_keeps_the_other_entries(self):
        tree = os.path.join(self.work, "dangling")
        _write(os.path.join(tree, "real.txt"), b"real")
        os.symlink(os.path.join(self.work, "missing.txt"), os.path.join(tree, "gone.txt"))
        entries = self.zip(tree, [tree])
        self.assertEqual({"real.txt": b"real"}, entries)


def _resolve_candidates(values):
    paths = []
    for value in values:
        paths.extend(part for part in value.split() if part)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script", action="append", required=True,
                        help="Path to zip_relative.sh")
    parser.add_argument("--zipper", action="append", required=True,
                        help="Candidate paths to the Bazel zipper executable")
    parser.add_argument("--java-home", default=None,
                        help="Unused on POSIX; kept for the Windows branch")
    args, remaining = parser.parse_known_args()

    global SCRIPT, ZIPPER
    script_candidates = _resolve_candidates(args.script)
    SCRIPT = next((p for p in script_candidates if os.path.isfile(p)), None)
    if SCRIPT is None:
        raise SystemExit("zip_relative.sh not found among %r" % (script_candidates,))

    zipper_candidates = _resolve_candidates(args.zipper)
    ZIPPER = next(
        (p for p in zipper_candidates if os.path.isfile(p) and os.access(p, os.X_OK)), None
    )
    if ZIPPER is None:
        raise SystemExit("zipper executable not found among %r" % (zipper_candidates,))

    unittest.main(argv=[sys.argv[0], *remaining])


if __name__ == "__main__":
    main()
