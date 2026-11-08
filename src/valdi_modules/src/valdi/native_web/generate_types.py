#!/usr/bin/env python3
"""Generate declarations from pinned NSH metadata and native C++ additions."""

import argparse
from pathlib import Path
import re


FILES = (
    "event.rs", "iterator.rs", "url.rs", "encoding.rs", "blob.rs",
    "streams/mod.rs", "streams/readable.rs", "streams/writable.rs",
    "streams/transform.rs", "streams/text.rs", "fetch.rs", "websocket.rs",
)

DOM_EXCEPTION_CONSTANTS = (
    "INDEX_SIZE_ERR", "DOMSTRING_SIZE_ERR", "HIERARCHY_REQUEST_ERR",
    "WRONG_DOCUMENT_ERR", "INVALID_CHARACTER_ERR", "NO_DATA_ALLOWED_ERR",
    "NO_MODIFICATION_ALLOWED_ERR", "NOT_FOUND_ERR", "NOT_SUPPORTED_ERR",
    "INUSE_ATTRIBUTE_ERR", "INVALID_STATE_ERR", "SYNTAX_ERR",
    "INVALID_MODIFICATION_ERR", "NAMESPACE_ERR", "INVALID_ACCESS_ERR",
    "VALIDATION_ERR", "TYPE_MISMATCH_ERR", "SECURITY_ERR", "NETWORK_ERR",
    "ABORT_ERR", "URL_MISMATCH_ERR", "QUOTA_EXCEEDED_ERR", "TIMEOUT_ERR",
    "INVALID_NODE_TYPE_ERR", "DATA_CLONE_ERR",
)


def declarations(source):
    # DOMException is supplied by NativeWeb.cpp, not the NSH Rust class table.
    native_members = [
        "  constructor(message?: string, name?: string);",
        "  readonly message: string;",
        "  readonly name: string;",
        "  readonly code: number;",
    ]
    for value, constant in enumerate(DOM_EXCEPTION_CONSTANTS, 1):
        native_members += [
            f"  static readonly {constant}: {value};",
            f"  readonly {constant}: {value};",
        ]
    sections = [
        "export class DOMException extends Error {\n"
        + "\n".join(native_members) + "\n}",
    ]
    classes = []
    for relative in FILES:
        text = (source / relative).read_text()
        for types in re.findall(r'const TYPES: &str = r#"(.*?)"#;', text, re.S):
            sections.append(re.sub(
                r"^(type|interface) ", r"export \1 ", types, flags=re.M,
            ).rstrip())
        # Class metadata is deliberately kept in the upstream table format.
        # Match table entries, not braces inside their embedded TS declarations.
        starts = list(re.finditer(r"\bSpec \{\s*class:", text))
        for index, start in enumerate(starts):
            end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            entry = text[start.start():end]
            if not re.search(r"\bglobal: true,", entry):
                continue
            name = re.search(r'\bname: c"([^"]+)"', entry)
            metadata = re.search(
                r'\bhead: "([^"]*)",\s*members: r#"(.*?)"#,', entry, re.S,
            )
            if name is None or metadata is None:
                raise ValueError(f"Missing public class metadata in {relative}")
            head, members = metadata.groups()
            constants = re.search(
                r"\bconstants: &\[(.*?)\],\s*global:", entry, re.S,
            )
            body = []
            if constants:
                for constant in re.findall(r'\(c"([^"]+)",', constants.group(1)):
                    body += [
                        f"  static readonly {constant}: number;",
                        f"  readonly {constant}: number;",
                    ]
            body += ["  " + line for line in members.splitlines()]
            classes.append(f"export class {name.group(1)}{head} {{\n"
                           + "\n".join(body) + "\n}")
    sections += classes
    sections.append(
        "export function fetch(input: string | URL | Request, "
        "init?: RequestInit): Promise<Response>;"
    )
    sections.append(
        "/** Install this module's Web APIs on the current JS global once. */\n"
        "export function installNativeWeb(): void;"
    )
    return (
        "// Generated from NSH Rust Web API metadata and native C++ additions; do not edit.\n"
        "// Upstream: longbridge/nsh, Apache-2.0.\n"
        "// Commit: 701bdcee9512e0f6aed39042d1fe24564dcd0b43.\n\n"
        + "\n\n".join(sections) + "\n"
    )


def main():
    module = Path(__file__).resolve().parent
    fork = module.parents[4]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source", type=Path,
        default=fork / "libs/nsh_web/src/web",
    )
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args()
    output = module / "src/NativeWeb.d.ts"
    contents = declarations(options.source)
    if options.check:
        if not output.exists() or output.read_text() != contents:
            raise SystemExit("NativeWeb.d.ts is stale; run generate_types.py")
    elif not output.exists() or output.read_text() != contents:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(contents)


if __name__ == "__main__":
    main()
