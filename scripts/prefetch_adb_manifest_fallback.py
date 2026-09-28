#!/usr/bin/env python3
"""Acquire the locked AOSP manifest when Gitiles' archive endpoint returns 503."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import io
import json
import os
import pathlib
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

from prefetch_adb_helper_sources import (
    DOWNLOAD_ATTEMPTS,
    DOWNLOAD_RETRY_BASE_SECONDS,
    DOWNLOAD_SOCKET_TIMEOUT_SECONDS,
    canonicalize_tar_gz,
    deterministic_tar_gz,
    is_transient_http_status,
    sha256,
    validated_records,
)

HOST = "android.googlesource.com"
MAX_ENCODED_BYTES = 2 * 1024 * 1024
MAX_ARCHIVE_BYTES = 1024 * 1024
POLICY = {
    "commit": "5bc9a7ce1cd78dd53613bbfd0ebf506e1e4adb0f",
    "archive_file": "aosp-manifest-5bc9a7ce1cd78dd53613bbfd0ebf506e1e4adb0f.tar.gz",
    "sha256": "5ae266404d0a1c71cb6d91d15c24b1c7cb7c33abb7bdeb3a49f5e9f2b57b1fab",
    "files": {
        "GLOBAL-PREUPLOAD.cfg": {
            "size": 850,
            "git_blob": "b98a673b95bf16d9f34ddb17635c4debab899c27",
        },
        "default.xml": {
            "size": 111646,
            "git_blob": "f8c6b343b51c94693108dcefb6bc41f51ae24791",
        },
    },
}


def read_gitiles(url: str, *, attempts: int = DOWNLOAD_ATTEMPTS,
                 sleep=time.sleep) -> bytes:
    """Read a bounded commit-pinned TEXT blob; never trust a different host."""
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or parsed.hostname != HOST or parsed.query != "format=TEXT":
        raise RuntimeError("unreviewed Gitiles blob URL")
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(
                url, headers={"User-Agent": "klogg-adb-manifest-prefetch/1"}
            )
            with urllib.request.urlopen(request, timeout=30) as response:
                final = urllib.parse.urlsplit(response.geturl())
                if final.scheme != "https" or final.hostname != HOST:
                    raise RuntimeError("Gitiles blob redirected outside the reviewed host")
                encoded = response.read(MAX_ENCODED_BYTES + 1)
            if len(encoded) > MAX_ENCODED_BYTES:
                raise RuntimeError("oversized Gitiles blob response")
            try:
                return base64.b64decode(encoded.strip(), validate=True)
            except binascii.Error as error:
                raise RuntimeError("invalid Gitiles blob encoding") from error
        except urllib.error.HTTPError as error:
            retry = is_transient_http_status(error.code) and attempt + 1 < attempts
            error.close()
            if not retry:
                raise
        except OSError:
            if attempt + 1 >= attempts:
                raise
        sleep(attempt + 1)
    raise RuntimeError("unreachable Gitiles retry state")


def download_manifest(url: str, destination: pathlib.Path, *,
                      attempts: int = DOWNLOAD_ATTEMPTS, sleep=time.sleep) -> None:
    """Apply the shared retry policy without buffering an unbounded response."""
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(
                url, headers={"User-Agent": "klogg-adb-manifest-prefetch/1"}
            )
            with urllib.request.urlopen(request, timeout=DOWNLOAD_SOCKET_TIMEOUT_SECONDS) as response, \
                    destination.open("wb") as output:
                size = 0
                while True:
                    chunk = response.read(min(65536, MAX_ARCHIVE_BYTES - size + 1))
                    size += len(chunk)
                    if size > MAX_ARCHIVE_BYTES:
                        raise RuntimeError("oversized AOSP manifest archive")
                    if not chunk:
                        return
                    output.write(chunk)
        except urllib.error.HTTPError as error:
            retry = is_transient_http_status(error.code) and attempt + 1 < attempts
            error.close()
            if not retry:
                raise
        except OSError:
            if attempt + 1 >= attempts:
                raise
        sleep(DOWNLOAD_RETRY_BASE_SECONDS * (attempt + 1))
    raise RuntimeError("unreachable AOSP manifest retry state")


def prefetch_manifest(lock_path: pathlib.Path, download_root: pathlib.Path, *,
                      policy: dict = POLICY, downloader=download_manifest,
                      fetch=read_gitiles) -> pathlib.Path:
    """Publish only the exact canonical manifest archive named by the reviewed lock."""
    lock = json.loads(pathlib.Path(lock_path).read_text(encoding="utf-8"))
    records = validated_records(lock)
    matches = [record for record in records if record["id"] == "aosp-manifest"]
    commit = policy["commit"]
    archive_url = f"https://{HOST}/platform/manifest/+archive/{commit}.tar.gz"
    sources = [record for record in lock["sources"] if record.get("id") == "aosp-manifest"]
    if (len(matches) != 1 or len(sources) != 1
            or matches[0]["archive_file"] != policy["archive_file"]
            or matches[0]["archive_identity"] != "canonical-tar-gz-v1"
            or matches[0]["archive_sha256"] != policy["sha256"]
            or matches[0]["download_url"] != archive_url
            or sources[0].get("archive_url") != archive_url
            or sources[0].get("repository_url") != f"https://{HOST}/platform/manifest"
            or sources[0].get("commit") != commit
            or set(policy["files"]) != {"GLOBAL-PREUPLOAD.cfg", "default.xml"}):
        raise RuntimeError("unreviewed locked AOSP manifest source identity")

    download_root = pathlib.Path(download_root)
    download_root.mkdir(parents=True, exist_ok=True)
    if download_root.is_symlink():
        raise RuntimeError("AOSP manifest cache root must not be a symlink")
    destination = download_root / policy["archive_file"]
    if destination.exists() or destination.is_symlink():
        if (destination.is_symlink() or not destination.is_file()
                or destination.stat().st_size > MAX_ARCHIVE_BYTES
                or sha256(destination) != policy["sha256"]):
            raise RuntimeError("existing AOSP manifest archive differs from the lock")
        return destination

    with tempfile.NamedTemporaryFile(dir=download_root, delete=False) as output:
        temporary = pathlib.Path(output.name)
    try:
        try:
            downloader(archive_url, temporary)
        except urllib.error.HTTPError as error:
            if error.code != 503:
                raise
            blobs = {}
            for name, expected in sorted(policy["files"].items()):
                url = f"https://{HOST}/platform/manifest/+/{commit}/{name}?format=TEXT"
                data = fetch(url)
                if (len(data) != expected["size"]
                        or len(data) > MAX_ENCODED_BYTES
                        or hashlib.sha1(
                            b"blob " + str(len(data)).encode("ascii") + b"\0" + data
                        ).hexdigest() != expected["git_blob"]):
                    raise RuntimeError(f"Gitiles blob differs from locked manifest: {name}")
                blobs[name] = data
            with deterministic_tar_gz(temporary) as archive:
                for name, data in sorted(blobs.items()):
                    member = tarfile.TarInfo(name)
                    member.mode = 0o644
                    member.mtime = member.uid = member.gid = 0
                    member.uname = member.gname = "root"
                    member.size = len(data)
                    archive.addfile(member, io.BytesIO(data))
        else:
            if temporary.stat().st_size > MAX_ARCHIVE_BYTES:
                raise RuntimeError("oversized AOSP manifest archive")
            canonicalize_tar_gz(temporary)
        if (temporary.stat().st_size > MAX_ARCHIVE_BYTES
                or sha256(temporary) != policy["sha256"]):
            raise RuntimeError("AOSP manifest canonical SHA-256 differs from the lock")
        try:
            os.link(temporary, destination)
        except FileExistsError as error:
            raise RuntimeError("AOSP manifest cache destination appeared during fetch") from error
        return destination
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--download-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    result = prefetch_manifest(args.lock, args.download_root)
    print(f"verified locked AOSP manifest: {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
