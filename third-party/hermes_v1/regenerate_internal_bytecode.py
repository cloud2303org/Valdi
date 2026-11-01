#!/usr/bin/env python3
"""Regenerate Valdi's patched Hermes 260318099.0.4 InternalBytecode.inc.

Requires the official pinned Hermes source and matching hermesc. Reads the source
only; patches a temporary copy. Mirrors lib/InternalJavaScript/CMakeLists.txt:
sorted *.js concatenation, hermesc -O -Wno-undefined-variable -g0 -emit-binary,
then upstream xxd.py. MODULE.bazel applies the same source patch to the archive.

Example:
  python3 regenerate_internal_bytecode.py --hermes-source /path/to/hermes \
      --hermesc /path/to/hermes-compiler/hermesc/osx-bin/hermesc
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


VERSION = "260318099.0.4"
HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hermes-source", type=Path, required=True)
    parser.add_argument("--hermesc", type=Path, required=True)
    parser.add_argument("--output", type=Path,
                        default=HERE / "prepared_include/internal_js/InternalBytecode.inc")
    args = parser.parse_args()
    source = args.hermes_source.resolve()
    compiler = args.hermesc.resolve()
    package = json.loads((source / "npm/hermes-compiler/package.json").read_text())
    if package["version"] != VERSION:
        parser.error(f"Hermes source must be {VERSION}, got {package['version']}")
    version = subprocess.check_output([str(compiler), "-version"], text=True)
    if not re.search(r"Hermes release version:\s*" + re.escape(VERSION) + r"\s", version):
        parser.error(f"hermesc must report release {VERSION}")
    patch = HERE / "promise_native_ownership.patch"
    with tempfile.TemporaryDirectory(prefix="valdi-hermes-internal-") as temp:
        root = Path(temp)
        internal = root / "lib/InternalJavaScript"
        shutil.copytree(source / "lib/InternalJavaScript", internal)
        subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
                       cwd=root, check=True)
        sources = sorted(internal.glob("*.js"))
        combined = root / "InternalBytecode.js"
        combined.write_bytes(b"".join(path.read_bytes() for path in sources))
        bytecode = root / "InternalBytecode.hbc"
        subprocess.run([str(compiler), "-O", "-Wno-undefined-variable", "-g0", "-emit-binary",
                        f"-out={bytecode}", str(combined)], check=True)
        include = subprocess.check_output([sys.executable, str(internal / "xxd.py"), str(bytecode)])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(include)
        print(f"Hermes {VERSION}; inputs: {', '.join(path.name for path in sources)}")
        print(f"patched source SHA256: {hashlib.sha256(combined.read_bytes()).hexdigest()}")
        print(f"patch SHA256: {hashlib.sha256(patch.read_bytes()).hexdigest()}")
        print(f"HBC: {bytecode.stat().st_size} bytes; SHA256: "
              f"{hashlib.sha256(bytecode.read_bytes()).hexdigest()}")
        print(f"include: {args.output}; {len(include)} bytes; "
              f"SHA256: {hashlib.sha256(include).hexdigest()}")


if __name__ == "__main__":
    main()
