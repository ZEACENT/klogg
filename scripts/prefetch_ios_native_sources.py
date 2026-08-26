#!/usr/bin/env python3
"""Prefetch and SHA-256 verify the locked iOS native source archives."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import urllib.request


class PrefetchError(RuntimeError):
    pass


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=True)
    if args.output.is_symlink() or not args.output.is_dir():
        raise PrefetchError(f"invalid iOS source archive output directory: {args.output}")
    for source in lock["sources"]:
        archive_file = pathlib.PurePath(source["archive_file"])
        if archive_file.name != str(archive_file) or archive_file.is_absolute():
            raise PrefetchError(f"unsafe locked archive filename: {archive_file}")
        destination = args.output / archive_file.name
        if destination.is_symlink():
            raise PrefetchError(f"cached source archive must not be a symlink: {destination}")
        if destination.is_file() and sha256(destination) == source["archive_sha256"]:
            print(f"verified cached {source['id']}: {destination}")
            continue
        temporary = destination.with_suffix(destination.suffix + ".part")
        temporary.unlink(missing_ok=True)
        request = urllib.request.Request(
            source["archive_url"], headers={"User-Agent": "klogg-ios-native-prefetch/1"}
        )
        try:
            with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
        except Exception as error:
            temporary.unlink(missing_ok=True)
            raise PrefetchError(f"failed to download locked source {source['id']}: {error}") from error
        actual = sha256(temporary)
        if actual != source["archive_sha256"]:
            temporary.unlink()
            raise PrefetchError(
                f"downloaded source sha256 mismatch for {source['id']}: {actual}"
            )
        temporary.replace(destination)
        print(f"prefetched {source['id']}: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
