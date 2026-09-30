#!/usr/bin/env python3
"""Reproduce the locked AOSP libusb archive only after Gitiles archive HTTP 503."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import pathlib
import posixpath
import re
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
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

REPOSITORY = "https://android.googlesource.com/platform/external/libusb"
COMMIT = "70460fc2b43c3948f9caae1fd4eacd2d666a872b"
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024
MAX_DOWNLOAD_SECONDS = 180
MAX_EXPANDED_ARCHIVE_BYTES = 16 * 1024 * 1024
MAX_TREE_BYTES = 8 * 1024 * 1024
MAX_OBJECT_BYTES = 16 * 1024 * 1024
MAX_TREE_ENTRIES = 256
GIT_DEADLINE_SECONDS = 300
POLICY = {
    "commit": COMMIT,
    "repository_url": REPOSITORY,
    "archive_file": "aosp-libusb-" + COMMIT + ".tar.gz",
    "sha256": "3cefc015ee99db245dbbb5e0401bf1a6531ab3505b3810968665c862766dadde",
    "entries": 170,
    "directories": 21,
    "symlinks": 5,
}


def download_archive(url: str, destination: pathlib.Path, *,
                     attempts: int = DOWNLOAD_ATTEMPTS, sleep=time.sleep) -> None:
    """Bound the direct archive bytes and preserve the existing retry policy."""
    deadline = time.monotonic() + MAX_DOWNLOAD_SECONDS
    for attempt in range(attempts):
        try:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("locked libusb archive download exceeded overall deadline")
            request = urllib.request.Request(
                url, headers={"User-Agent": "klogg-adb-libusb-prefetch/1"}
            )
            with urllib.request.urlopen(request, timeout=min(DOWNLOAD_SOCKET_TIMEOUT_SECONDS, remaining)) as response, \
                    destination.open("wb") as output:
                size = 0
                while True:
                    chunk = response.read1(min(65536, MAX_ARCHIVE_BYTES - size + 1))
                    if time.monotonic() >= deadline:
                        raise RuntimeError("locked libusb archive download exceeded overall deadline")
                    size += len(chunk)
                    if size > MAX_ARCHIVE_BYTES:
                        raise RuntimeError("oversized locked AOSP libusb archive")
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
    raise RuntimeError("unreachable locked libusb retry state")


def _bounded_archive_expansion(archive: pathlib.Path) -> None:
    expanded = 0
    with gzip.open(archive, "rb") as source:
        while chunk := source.read(min(65536, MAX_EXPANDED_ARCHIVE_BYTES - expanded + 1)):
            expanded += len(chunk)
            if expanded > MAX_EXPANDED_ARCHIVE_BYTES:
                raise RuntimeError("oversized expanded locked libusb archive")


def _git(repository: pathlib.Path, arguments: list[str], *, timeout: int = 90) -> bytes:
    environment = {**os.environ, "GIT_TERMINAL_PROMPT": "0",
                   "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull}
    try:
        result = subprocess.run(
            ["git", "-c", "http.followRedirects=false", "-c", "credential.helper=",
             "-C", str(repository), *arguments],
            check=True, capture_output=True, timeout=timeout, env=environment,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise RuntimeError("official locked libusb Git operation failed: " + arguments[0]) from error
    if len(result.stdout) > MAX_TREE_BYTES:
        raise RuntimeError("oversized locked libusb Git response")
    return result.stdout


def fetch_git_tree(policy: dict) -> dict[str, tuple[str, bytes]]:
    """Fetch one reviewed commit, then verify each tracked Git blob locally."""
    with tempfile.TemporaryDirectory(prefix="klogg-libusb-commit-") as directory:
        repository = pathlib.Path(directory)
        deadline = time.monotonic() + GIT_DEADLINE_SECONDS

        def bounded_git(arguments: list[str]) -> bytes:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("locked libusb Git fetch exceeded overall deadline")
            return _git(repository, arguments, timeout=max(1, min(90, int(remaining))))

        bounded_git(["init", "-q"])
        for attempt in range(2):
            try:
                bounded_git(["fetch", "-q", "--no-tags", "--depth=1",
                             policy["repository_url"], policy["commit"]])
                break
            except RuntimeError as error:
                if attempt == 1 or time.monotonic() + 10 >= deadline:
                    raise RuntimeError(
                        "official locked libusb Git fetch failed: " + policy["repository_url"] + ": " + str(error)
                    ) from error
                time.sleep(10)
        objects = repository / ".git" / "objects"
        if sum(path.stat().st_size for path in objects.rglob("*") if path.is_file()) > MAX_OBJECT_BYTES:
            raise RuntimeError("oversized locked libusb Git object store")
        commit = bounded_git(["rev-parse", "FETCH_HEAD"]).strip().decode("ascii")
        if commit != policy["commit"]:
            raise RuntimeError("locked libusb Git commit differs from the reviewed source")
        entries = bounded_git(["ls-tree", "--full-tree", "-rz", commit]).split(b"\0")
        if entries[-1] != b"" or len(entries) - 1 != policy["entries"]:
            raise RuntimeError("locked libusb Git tree entry count differs from the reviewed source")
        files = {}
        size = 0
        for record in entries[:-1]:
            metadata, separator, raw_name = record.partition(b"\t")
            fields = metadata.split(b" ")
            if not separator or len(fields) != 3 or fields[1] != b"blob":
                raise RuntimeError("invalid locked libusb Git tree entry")
            mode, oid = fields[0].decode("ascii"), fields[2].decode("ascii")
            if mode not in ("100644", "100755", "120000") or not re.fullmatch(r"[0-9a-f]{40}", oid):
                raise RuntimeError("unsupported locked libusb Git mode or blob identity")
            name = raw_name.decode("utf-8")
            if name in files:
                raise RuntimeError("duplicate locked libusb Git tree path")
            data = bounded_git(["cat-file", "blob", oid])
            size += len(data)
            if (size > MAX_TREE_BYTES or
                    hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest() != oid):
                raise RuntimeError("locked libusb Git blob exceeds limit or differs from tree identity")
            files[name] = (mode, data)
        return files


def _safe_path(name: str) -> None:
    if (not name or name.startswith("/") or "\\" in name or "\n" in name or "\r" in name
            or posixpath.normpath(name) != name or any(part in ("", ".", "..") for part in name.split("/"))):
        raise RuntimeError("unsafe locked libusb archive path: " + name)


def _write_tree_archive(destination: pathlib.Path, files: dict[str, tuple[str, bytes]],
                        policy: dict) -> None:
    if len(files) != policy["entries"] or len(files) > MAX_TREE_ENTRIES:
        raise RuntimeError("locked libusb tree entry count differs from reviewed source")
    directories = set()
    for name in files:
        _safe_path(name)
        parent = posixpath.dirname(name)
        while parent:
            directories.add(parent)
            parent = posixpath.dirname(parent)
    if len(directories) != policy["directories"] or directories & files.keys():
        raise RuntimeError("locked libusb tree directory layout differs from reviewed source")
    total = 0
    symlinks = 0
    with deterministic_tar_gz(destination) as archive:
        for name in sorted(directories | files.keys()):
            member = tarfile.TarInfo(name)
            member.uid = member.gid = member.mtime = 0
            member.uname = member.gname = "root"
            if name in directories:
                member.type = tarfile.DIRTYPE
                member.mode = 0o755
                archive.addfile(member)
                continue
            mode, data = files[name]
            if mode not in ("100644", "100755", "120000"):
                raise RuntimeError("unsupported locked libusb Git mode: " + name)
            member.mode = 0o755 if mode == "100755" else 0o644
            if mode == "120000":
                symlinks += 1
                target = data.decode("utf-8")
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
                if (not target or target.startswith("/") or "\\" in target or "\n" in target
                        or resolved in (".", "..") or resolved.startswith("../")
                        or resolved not in files and resolved not in directories):
                    raise RuntimeError("unsafe locked libusb symlink: " + name)
                member.type = tarfile.SYMTYPE
                member.linkname = target
                archive.addfile(member)
                continue
            total += len(data)
            if total > MAX_TREE_BYTES:
                raise RuntimeError("oversized locked libusb source tree")
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    if symlinks != policy["symlinks"]:
        raise RuntimeError("locked libusb symlink count differs from reviewed source")
    canonicalize_tar_gz(destination)


def prefetch_libusb(lock_path: pathlib.Path, download_root: pathlib.Path, *,
                    policy: dict = POLICY, downloader=download_archive,
                    fetch=fetch_git_tree) -> pathlib.Path:
    """Publish only the original lock's canonical archive from its reviewed commit."""
    lock = json.loads(pathlib.Path(lock_path).read_text(encoding="utf-8"))
    records = validated_records(lock)
    matches = [record for record in records if record["id"] == "aosp-libusb"]
    sources = [record for record in lock["sources"] if record.get("id") == "aosp-libusb"]
    url = policy["repository_url"] + "/+archive/" + policy["commit"] + ".tar.gz"
    if (len(matches) != 1 or len(sources) != 1
            or matches[0]["archive_file"] != policy["archive_file"]
            or matches[0]["archive_identity"] != "canonical-tar-gz-v1"
            or matches[0]["archive_sha256"] != policy["sha256"]
            or matches[0]["download_url"] != url
            or sources[0].get("archive_url") != url
            or sources[0].get("repository_url") != policy["repository_url"]
            or sources[0].get("commit") != policy["commit"]
            or sources[0].get("build_input") is not False):
        raise RuntimeError("unreviewed locked AOSP libusb source identity")
    download_root = pathlib.Path(download_root)
    download_root.mkdir(parents=True, exist_ok=True)
    if download_root.is_symlink():
        raise RuntimeError("locked libusb cache root must not be a symlink")
    destination = download_root / policy["archive_file"]
    if destination.exists() or destination.is_symlink():
        if (destination.is_symlink() or not destination.is_file()
                or destination.stat().st_size > MAX_ARCHIVE_BYTES
                or sha256(destination) != policy["sha256"]):
            raise RuntimeError("existing AOSP libusb archive differs from the lock")
        return destination
    with tempfile.NamedTemporaryFile(dir=download_root, delete=False) as stream:
        temporary = pathlib.Path(stream.name)
    try:
        try:
            downloader(url, temporary)
        except urllib.error.HTTPError as error:
            if error.code != 503:
                raise
            files = fetch(policy)
            _write_tree_archive(temporary, files, policy)
        else:
            if temporary.stat().st_size > MAX_ARCHIVE_BYTES:
                raise RuntimeError("oversized locked libusb download")
            _bounded_archive_expansion(temporary)
            canonicalize_tar_gz(temporary)
        if (temporary.stat().st_size > MAX_ARCHIVE_BYTES or sha256(temporary) != policy["sha256"]):
            raise RuntimeError("locked libusb canonical SHA-256 differs from the lock")
        try:
            os.link(temporary, destination)
        except FileExistsError as error:
            raise RuntimeError("locked libusb cache destination appeared during fetch") from error
        return destination
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--download-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    result = prefetch_libusb(args.lock, args.download_root)
    print(f"verified locked AOSP libusb: {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
