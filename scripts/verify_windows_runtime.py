#!/usr/bin/env python3
"""Verify app-local MSVC runtimes in an extracted or installed Windows package."""

import argparse
import hashlib
from pathlib import Path
import sys


REQUIRED_RUNTIME = (
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
)


def runtime_digest(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"required runtime missing, empty, or not a regular file: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.digest()


def verify_runtime(package_root: Path, staged_root: Path, arch: str) -> None:
    if arch != "x64":
        raise ValueError(f"unsupported Windows runtime architecture: {arch}")
    if package_root.resolve() == staged_root.resolve():
        raise ValueError("final package and staging directories must be distinct")

    # Inspect only the final application directory, never PATH or system runtimes.
    # A fixed required set also catches omissions shared by staging and packaging.
    for name in REQUIRED_RUNTIME:
        packaged = package_root / name
        staged = staged_root / name
        if runtime_digest(packaged) != runtime_digest(staged):
            raise ValueError(f"packaged runtime {packaged} differs from staged {staged}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--staged-root", type=Path, required=True)
    parser.add_argument("--arch", choices=("x64",), required=True)
    args = parser.parse_args(argv)
    try:
        verify_runtime(args.package_root, args.staged_root, args.arch)
    except (OSError, ValueError) as error:
        print(f"Windows runtime verification failed: {error}", file=sys.stderr)
        return 1
    print(f"Verified {args.arch} app-local MSVC runtime in {args.package_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
