#!/usr/bin/env python3
"""Check local clang-tidy header scope without changing authenticated policy."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys


HEADER_EXTENSIONS = {".h", ".hh", ".hpp", ".hxx"}
SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx"}


def header_filter_from_dump(config):
    """Read only a top-level scalar emitted by --dump-config, not arbitrary YAML."""
    values = re.findall(r"^HeaderFilterRegex:[ \t]*(.*)$", config, re.MULTILINE)
    if len(values) != 1:
        raise ValueError("expected exactly one HeaderFilterRegex in clang-tidy --dump-config")
    value = values[0].strip()
    if re.fullmatch(r"'(?:[^']|'')*'", value):
        return value[1:-1].replace("''", "'")
    if value.startswith('"'):
        decoded = json.loads(value)
        if isinstance(decoded, str):
            return decoded
    # LLVM emits plain scalars too. Reject YAML collections, aliases, tags,
    # block scalars and comments instead of guessing their meaning.
    if value and value[0] not in "'\"|>*&!{[" and not re.search(r"\s|#", value):
        if value not in {"null", "~", "true", "false"}:
            return value
    raise ValueError("unsupported HeaderFilterRegex scalar in clang-tidy --dump-config")


def check_scope(pattern, headers, source_root):
    if not pattern:
        raise ValueError("empty HeaderFilterRegex suppresses first-party header diagnostics")
    try:
        regex = re.compile(pattern)
    except re.error as error:
        raise ValueError(f"invalid HeaderFilterRegex: {error}") from error
    for header in headers:
        if not regex.search(header.as_posix()):
            raise ValueError(f"HeaderFilterRegex suppresses first-party header: {header}")
    for relative in ("3rdparty/example/include/example.h", "cpm_cache/example/include/example.h"):
        dependency = source_root.parent / relative
        if regex.search(dependency.as_posix()):
            raise ValueError(f"HeaderFilterRegex includes dependency header: {dependency}")


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path("src"))
    parser.add_argument("--clang-tidy", default="clang-tidy")
    args = parser.parse_args(arguments)
    root = args.source_root.resolve()
    try:
        files = sorted(path.resolve() for path in root.rglob("*") if path.is_file())
        sources = [path for path in files if path.suffix.lower() in SOURCE_EXTENSIONS]
        headers = [path for path in files if path.suffix.lower() in HEADER_EXTENSIONS]
        if not sources or not headers:
            raise ValueError(f"source root must contain actual sources and headers: {root}")
        result = subprocess.run(
            [args.clang_tidy, "--dump-config", str(sources[0])],
            check=True, capture_output=True, text=True,
        )
        check_scope(header_filter_from_dump(result.stdout), headers, root)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: clang-tidy scope preflight failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        print("Do not accept a clean analysis with unverified header scope. Use the locked "
              "clang-tidy tool and configure the checkout/build at a canonical /.../klogg "
              "path (source root /.../klogg/src) until genuine environment requalification; "
              "do not edit signed policy inputs.", file=sys.stderr)
        return 1
    print(f"clang-tidy scope: {len(headers)} first-party headers included; dependency examples excluded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
