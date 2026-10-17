#!/usr/bin/env python3
"""Acquire the same pinned ADB source closure for ordinary and environment CI."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

from prefetch_adb_libusb_fallback import prefetch_libusb
from prefetch_adb_manifest_fallback import prefetch_manifest

MAX_CACHE_BYTES = 536870912
MAX_DOWNLOAD_WORKERS = 2


def prefetch_closure(lock_path: pathlib.Path, download_root: pathlib.Path, *,
                     workers: int = MAX_DOWNLOAD_WORKERS,
                     max_cache_bytes: int = MAX_CACHE_BYTES,
                     manifest=prefetch_manifest, libusb=prefetch_libusb,
                     runner=subprocess.run) -> None:
    """Seed only reviewed fallbacks before the unchanged full SHA/size validator."""
    if (type(workers) is not int or not 1 <= workers <= MAX_DOWNLOAD_WORKERS
            or type(max_cache_bytes) is not int or not 1 <= max_cache_bytes <= MAX_CACHE_BYTES):
        raise RuntimeError("unreviewed ADB source concurrency or cache byte limit")
    lock_path = pathlib.Path(lock_path)
    download_root = pathlib.Path(download_root)
    manifest(lock_path, download_root)
    libusb(lock_path, download_root)
    command = [
        sys.executable, str(pathlib.Path(__file__).with_name("prefetch_adb_source_context.py")),
        "--lock", str(lock_path), "--download-root", str(download_root),
        "--workers", str(workers), "--max-cache-bytes", str(max_cache_bytes),
    ]
    try:
        result = runner(command, check=False, capture_output=True, text=True, timeout=1800)
    except (OSError, subprocess.SubprocessError) as error:
        raise RuntimeError("locked ADB source closure process failed") from error
    if result.returncode:
        diagnostic = result.stderr or ""
        sys.stderr.write(diagnostic[-8192:])
        terminal = diagnostic.strip().splitlines()[-1] if diagnostic.strip() else "no diagnostic"
        raise RuntimeError("locked ADB source closure failed: " + terminal)
    sys.stdout.write(result.stdout or "")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--download-root", required=True, type=pathlib.Path)
    parser.add_argument("--workers", type=int, default=MAX_DOWNLOAD_WORKERS)
    parser.add_argument("--max-cache-bytes", type=int, default=MAX_CACHE_BYTES)
    args = parser.parse_args()
    prefetch_closure(args.lock, args.download_root,
                     workers=args.workers, max_cache_bytes=args.max_cache_bytes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
