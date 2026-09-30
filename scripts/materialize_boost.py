#!/usr/bin/env python3
"""Verify the catalog-pinned Boost source archive before installing its tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import sys
import tempfile

from prefetch_adb_helper_sources import download as download_source, safe_extract

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "ci/environments/materials.json"
EXPECTED_VERSION = 108600


def boost_asset(manifest: pathlib.Path) -> tuple[str, str]:
    document = json.loads(manifest.read_text(encoding="utf-8"))
    if document.get("schema_version") != 1 or not isinstance(document.get("assets"), dict):
        raise RuntimeError("invalid Boost materials catalog")
    asset = document["assets"].get("boost1860")
    if not isinstance(asset, dict):
        raise RuntimeError("Boost source is missing from materials catalog")
    url, digest = asset.get("url"), asset.get("sha256")
    if (not isinstance(url, str) or not url.startswith("https://archives.boost.io/")
            or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)):
        raise RuntimeError("Boost source must use the official HTTPS archive and SHA-256")
    return url, digest


def check_archive(archive: pathlib.Path, digest: str) -> None:
    if archive.is_symlink() or not archive.is_file():
        raise RuntimeError(f"prefetched Boost archive is missing or not a regular file: {archive}")
    actual = hashlib.sha256()
    with archive.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            actual.update(chunk)
    if actual.hexdigest() != digest:
        raise RuntimeError(f"Boost archive SHA-256 mismatch: {archive}")


def materialize(
    archive: pathlib.Path,
    destination: pathlib.Path,
    manifest: pathlib.Path = MANIFEST,
    *,
    require_prefetched: bool = False,
    verify_only: bool = False,
    downloader=None,
) -> None:
    archive, destination = pathlib.Path(archive), pathlib.Path(destination)
    url, digest = boost_asset(pathlib.Path(manifest))
    if not archive.exists() and not archive.is_symlink():
        if require_prefetched:
            raise RuntimeError(f"prefetched Boost archive is required but missing: {archive}")
        archive.parent.mkdir(parents=True, exist_ok=True)
        # Only publish verified bytes; never leave a partially downloaded cache entry.
        with tempfile.TemporaryDirectory(prefix="boost-download-", dir=archive.parent) as temporary:
            candidate = pathlib.Path(temporary) / "source.tar.bz2"
            (downloader or download_source)(url, candidate)
            check_archive(candidate, digest)
            candidate.replace(archive)
    check_archive(archive, digest)  # A cache hit is not proof of integrity.
    if verify_only:
        return
    if destination.is_symlink():
        raise RuntimeError(f"Boost destination must not be a symlink: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="boost-extract-", dir=destination.parent) as temporary:
        extracted = pathlib.Path(temporary) / "source"
        safe_extract(archive, extracted)
        version = extracted / "boost/version.hpp"
        if not version.is_file() or version.is_symlink() or not re.search(
            rf"^\s*#\s*define\s+BOOST_VERSION\s+{EXPECTED_VERSION}\b",
            version.read_text(encoding="utf-8"), re.MULTILINE,
        ):
            raise RuntimeError("Boost source version does not match the pinned release")
        if destination.exists():
            if not destination.is_dir():
                raise RuntimeError(f"Boost destination must be a directory: {destination}")
            shutil.rmtree(destination)
        extracted.replace(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    parser.add_argument("--destination", type=pathlib.Path)
    parser.add_argument("--require-prefetched", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    if not args.verify_only and args.destination is None:
        parser.error("--destination is required unless --verify-only is set")
    try:
        materialize(
            args.archive, args.destination or ROOT / "3rdparty/boost",
            require_prefetched=args.require_prefetched, verify_only=args.verify_only,
        )
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Boost materialization failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
