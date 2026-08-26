#!/usr/bin/env python3
"""Extract a cross-job tar artifact with the shared fail-closed member policy."""

from __future__ import annotations

import argparse
import pathlib

from prefetch_adb_helper_sources import safe_extract


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True, type=pathlib.Path)
    parser.add_argument("--destination", required=True, type=pathlib.Path)
    args = parser.parse_args()
    if not args.archive.is_file() or args.archive.is_symlink():
        raise RuntimeError(f"missing or invalid tar artifact: {args.archive}")
    safe_extract(args.archive, args.destination)
    print(f"safely extracted tar artifact: {args.archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
