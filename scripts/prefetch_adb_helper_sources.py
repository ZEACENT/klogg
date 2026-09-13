#!/usr/bin/env python3
"""Prefetch and verify the immutable source closure for the bundled ADB helper."""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import gzip
import hashlib
import json
import os
import pathlib
import re
import shutil
import tarfile
import tempfile
import threading
import time
import urllib.request
from typing import Optional


RAW_ARCHIVE_IDENTITY = "raw-sha256"
CANONICAL_TAR_GZ_IDENTITY = "canonical-tar-gz-v1"

# Locked source archives occasionally answer transient 5xx; a bounded retry
# with linear backoff keeps the prefetch job resilient without masking real
# lock defects such as 404.
DOWNLOAD_ATTEMPTS = 4
DOWNLOAD_RETRY_BASE_SECONDS = 1.0
DOWNLOAD_SOCKET_TIMEOUT_SECONDS = 30
DOWNLOAD_CHUNK_SIZE = 64 * 1024
DEFAULT_DOWNLOAD_WORKERS = 4
PREFETCH_MANIFEST_NAME = "adb-helper-prefetch-manifest.json"
RETRYABLE_STATUS_CODES = frozenset({408, 429})
LOCK_RECORD_GROUPS = ("sources", "dependencies", "toolchain_packages")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")


def is_transient_http_status(code: int) -> bool:
    return code >= 500 or code in RETRYABLE_STATUS_CODES
SUPPORTED_ARCHIVE_IDENTITIES = {RAW_ARCHIVE_IDENTITY, CANONICAL_TAR_GZ_IDENTITY}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_component(value: str, label: str) -> str:
    if not value or value in (".", "..") or pathlib.PurePosixPath(value).name != value:
        raise RuntimeError(f"unsafe {label}: {value}")
    if "\\" in value or pathlib.PurePath(value).is_absolute():
        raise RuntimeError(f"unsafe {label}: {value}")
    return value


def normalized_archive_parts(value: str, label: str) -> tuple[str, ...]:
    if "\\" in value:
        raise RuntimeError(f"archive contains unsupported backslash in {label}: {value}")
    path = pathlib.PurePosixPath(value)
    if path.is_absolute():
        raise RuntimeError(f"archive {label} is absolute: {value}")
    parts: list[str] = []
    for part in path.parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise RuntimeError(f"archive {label} escapes extraction root: {value}")
            parts.pop()
        else:
            parts.append(part)
    if not parts:
        raise RuntimeError(f"archive {label} is empty: {value}")
    return tuple(parts)


def locked_symlink_exclusions(
    excluded_build_symlinks: object,
) -> dict[tuple[str, ...], dict[str, str]]:
    exclusions: dict[tuple[str, ...], dict[str, str]] = {}
    if excluded_build_symlinks is None:
        records = []
    elif isinstance(excluded_build_symlinks, list):
        records = excluded_build_symlinks
    else:
        raise RuntimeError("excluded build symlinks must be an array")
    for item in records:
        if not isinstance(item, dict):
            raise RuntimeError("invalid excluded build symlink record")
        path = item.get("path")
        target = item.get("target")
        reason = item.get("reason")
        if not all(isinstance(value, str) and value for value in (path, target, reason)):
            raise RuntimeError("invalid excluded build symlink record")
        parts = normalized_archive_parts(path, "excluded build symlink path")
        if pathlib.PurePosixPath(*parts).as_posix() != path or parts in exclusions:
            raise RuntimeError(f"invalid or duplicate excluded build symlink path: {path}")
        exclusions[parts] = {"path": path, "target": target, "reason": reason}
    return exclusions


def validated_archive_members(
    tar: tarfile.TarFile,
    excluded_build_symlinks: object = None,
    *,
    omit_excluded_symlinks: bool,
    validate_symlink_targets: bool,
) -> tuple[
    dict[tuple[str, ...], tarfile.TarInfo], list[dict[str, str]], bool
]:
    exclusions = locked_symlink_exclusions(excluded_build_symlinks)
    matched_exclusions: set[tuple[str, ...]] = set()
    seen: set[tuple[str, ...]] = set()
    by_name: dict[tuple[str, ...], tarfile.TarInfo] = {}
    root_directory_seen = False
    for member in tar.getmembers():
        if member.name.rstrip("/") == ".":
            if not member.isdir():
                raise RuntimeError(
                    f"archive contains normalized-empty non-directory member: {member.name}"
                )
            if root_directory_seen:
                raise RuntimeError("archive contains duplicate root directory member")
            root_directory_seen = True
            continue
        parts = normalized_archive_parts(member.name, "member")
        if parts in seen:
            raise RuntimeError(f"archive contains duplicate member path: {member.name}")
        seen.add(parts)
        if member.islnk() or not (member.isdir() or member.isfile() or member.issym()):
            raise RuntimeError(f"archive contains unsupported member type: {member.name}")
        if member.issym():
            exclusion = exclusions.get(parts)
            if exclusion is not None:
                if member.linkname != exclusion["target"]:
                    raise RuntimeError(
                        f"excluded build symlink target mismatch: {member.name}"
                    )
                matched_exclusions.add(parts)
                if omit_excluded_symlinks:
                    continue
            elif validate_symlink_targets:
                link_path = pathlib.PurePosixPath(*parts[:-1], member.linkname)
                normalized_archive_parts(link_path.as_posix(), "symlink target")
        by_name[parts] = member

    if not by_name:
        raise RuntimeError("archive contains no real members")

    missing_exclusions = exclusions.keys() - matched_exclusions
    if missing_exclusions:
        missing = ", ".join(exclusions[parts]["path"] for parts in missing_exclusions)
        raise RuntimeError(f"excluded build symlink is missing from archive: {missing}")
    return (
        by_name,
        [exclusions[parts] for parts in exclusions if parts in matched_exclusions],
        root_directory_seen,
    )


@contextlib.contextmanager
def deterministic_tar_gz(path: pathlib.Path):
    with path.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
                yield tar


def canonicalize_tar_gz(
    archive: pathlib.Path, excluded_build_symlinks: object = None
) -> None:
    """Encode a tar source tree independently of transport-time metadata.

    canonical-tar-gz-v1 binds normalized paths, member types, permission bits,
    regular-file bytes, and symlink targets. It deliberately excludes archive
    order, timestamps, owners, and provider PAX headers. Canonicalization never
    extracts symlinks; the stricter build extraction path still rejects links
    that escape its destination unless the lock excludes the exact link.
    """
    with tempfile.NamedTemporaryFile(dir=archive.parent, delete=False) as stream:
        temporary = pathlib.Path(stream.name)
    try:
        with tarfile.open(archive, "r:*") as source:
            members, _, _ = validated_archive_members(
                source,
                excluded_build_symlinks,
                omit_excluded_symlinks=False,
                validate_symlink_targets=False,
            )
            with deterministic_tar_gz(temporary) as output:
                for parts, member in sorted(members.items()):
                    info = tarfile.TarInfo(pathlib.PurePosixPath(*parts).as_posix())
                    info.mode = member.mode & 0o7777
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    if member.isdir():
                        info.type = tarfile.DIRTYPE
                        output.addfile(info)
                    elif member.issym():
                        info.type = tarfile.SYMTYPE
                        info.linkname = member.linkname
                        output.addfile(info)
                    else:
                        source_stream = source.extractfile(member)
                        if source_stream is None:
                            raise RuntimeError(
                                f"archive regular file has no content: {member.name}"
                            )
                        with source_stream:
                            info.size = member.size
                            output.addfile(info, source_stream)
        temporary.replace(archive)
    finally:
        temporary.unlink(missing_ok=True)


def platform_symlink_target(linkname: str, platform_name: str = os.name) -> str:
    if platform_name == "nt":
        return str(
            pathlib.PureWindowsPath(*pathlib.PurePosixPath(linkname).parts)
        )
    return linkname


def symlink_target_is_directory(
    parts: tuple[str, ...],
    member: tarfile.TarInfo,
    by_name: dict[tuple[str, ...], tarfile.TarInfo],
) -> bool:
    def target_parts(link_parts: tuple[str, ...], linkname: str) -> tuple[str, ...]:
        target = pathlib.PurePosixPath(*link_parts[:-1], linkname)
        return normalized_archive_parts(target.as_posix(), "symlink target")

    if member.linkname.endswith("/"):
        return True

    candidate = target_parts(parts, member.linkname)
    visited: set[tuple[str, ...]] = set()
    while True:
        if candidate in visited:
            raise RuntimeError(f"archive contains a symlink cycle: {member.name}")
        visited.add(candidate)
        target = by_name.get(candidate)
        if target is not None:
            if target.isdir():
                return True
            if target.isfile():
                return False
            if target.issym():
                candidate = target_parts(candidate, target.linkname)
                continue
        if any(
            len(path) > len(candidate) and path[: len(candidate)] == candidate
            for path in by_name
        ):
            return True
        top_level = {path[0] for path in by_name}
        if len(top_level) == 1 and candidate[0] not in top_level:
            raise RuntimeError(
                f"archive symlink escapes extraction root after layout normalization: {member.name}"
            )
        # Preserve the historical file-link default for intentionally broken
        # metadata links whose target is absent from the release archive.
        return False


def canonical_existing_ancestor(path: pathlib.Path) -> pathlib.Path:
    candidate = path
    while not candidate.exists():
        parent = candidate.parent
        if parent == candidate:
            raise RuntimeError(f"path has no existing ancestor: {path}")
        candidate = parent
    return candidate.resolve(strict=True)


def safe_extract(
    archive: pathlib.Path,
    destination: pathlib.Path,
    excluded_build_symlinks: object = None,
) -> list[dict[str, str]]:
    destination.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or any(destination.iterdir()):
        raise RuntimeError(f"archive extraction destination must be an empty directory: {destination}")

    with tarfile.open(archive, "r:*") as tar:
        by_name, matched_exclusions, explicit_root = validated_archive_members(
            tar,
            excluded_build_symlinks,
            omit_excluded_symlinks=True,
            validate_symlink_targets=True,
        )

        directories = sorted(
            ((parts, member) for parts, member in by_name.items() if member.isdir()),
            key=lambda item: len(item[0]),
        )
        regular_files = [
            (parts, member) for parts, member in by_name.items() if member.isfile()
        ]
        symlinks = [(parts, member) for parts, member in by_name.items() if member.issym()]

        for parts, member in directories:
            path = destination.joinpath(*parts)
            path.mkdir(parents=True, exist_ok=True)
            os.chmod(path, member.mode & 0o777)

        for parts, member in regular_files:
            path = destination.joinpath(*parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            source = tar.extractfile(member)
            if source is None:
                raise RuntimeError(f"archive regular file has no content: {member.name}")
            with source, path.open("xb") as output:
                shutil.copyfileobj(source, output)
            os.chmod(path, member.mode & 0o777)

        for parts, member in symlinks:
            path = destination.joinpath(*parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.exists() or path.is_symlink():
                raise RuntimeError(f"archive symlink collides with extracted path: {member.name}")
            os.symlink(
                platform_symlink_target(member.linkname),
                path,
                target_is_directory=symlink_target_is_directory(parts, member, by_name),
            )

    children = [path for path in destination.iterdir() if path.name != ".DS_Store"]
    if not explicit_root and len(children) == 1 and children[0].is_dir():
        top = children[0]
        temporary = destination.with_name(destination.name + ".flatten")
        if temporary.exists():
            shutil.rmtree(temporary)
        top.rename(temporary)
        destination.rmdir()
        temporary.rename(destination)

    extraction_root = destination.resolve()
    for path in destination.rglob("*"):
        if not path.is_symlink():
            continue
        try:
            resolved = path.resolve(strict=False)
            canonical_existing_ancestor( resolved ).relative_to( extraction_root )
        except (OSError, RuntimeError, ValueError) as error:
            raise RuntimeError(
                f"archive symlink escapes extraction root after layout normalization: {path}"
            ) from error

    return matched_exclusions


def download(
    url: str,
    destination: pathlib.Path,
    attempts: int = DOWNLOAD_ATTEMPTS,
    backoff_seconds: float = DOWNLOAD_RETRY_BASE_SECONDS,
    sleep=time.sleep,
    cancel_event: Optional[threading.Event] = None,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = pathlib.Path(stream.name)
    try:
        for attempt in range(attempts):
            if cancel_event is not None and cancel_event.is_set():
                raise RuntimeError("ADB source prefetch cancelled")
            try:
                request = urllib.request.Request(url, headers={"User-Agent": "klogg-adb-source-prefetch/1"})
                with urllib.request.urlopen(
                    request, timeout=DOWNLOAD_SOCKET_TIMEOUT_SECONDS
                ) as response, temporary.open("wb") as output:
                    read_chunk = getattr(response, "read1", response.read)
                    while True:
                        if cancel_event is not None and cancel_event.is_set():
                            raise RuntimeError("ADB source prefetch cancelled")
                        chunk = read_chunk(DOWNLOAD_CHUNK_SIZE)
                        if not chunk:
                            break
                        output.write(chunk)
                temporary.replace(destination)
                return
            except urllib.error.HTTPError as error:
                # Locked source hosts intermittently answer 502/503; a client
                # error is a lock or network defect that retrying cannot fix.
                retry = is_transient_http_status(error.code) and attempt + 1 < attempts
                error.close()
                if not retry:
                    raise
            except OSError:
                if attempt + 1 >= attempts:
                    raise
            if cancel_event is not None and cancel_event.is_set():
                raise RuntimeError("ADB source prefetch cancelled")
            sleep(backoff_seconds * (attempt + 1))
    finally:
        temporary.unlink(missing_ok=True)


def records(lock: dict) -> list[dict]:
    result: list[dict] = []
    for group in LOCK_RECORD_GROUPS:
        group_records = lock.get(group, [])
        if not isinstance(group_records, list):
            raise RuntimeError(f"invalid ADB lock record group: {group}")
        result.extend(group_records)
    return result


def required_string(record: dict, key: str, record_id: str) -> str:
    value = record.get(key)
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"invalid {key} for locked ADB source {record_id}")
    return value


def validated_records(
    lock: object, *, require_download_urls: bool = True
) -> list[dict]:
    """Validate every lock record before any download can begin."""
    if not isinstance(lock, dict):
        raise RuntimeError("ADB source lock must be a JSON object")

    validated: list[dict] = []
    seen_ids: set[str] = set()
    seen_archives: set[str] = set()
    for index, record in enumerate(records(lock)):
        if not isinstance(record, dict):
            raise RuntimeError(f"invalid locked ADB source record at index {index}")

        raw_id = required_string(record, "id", f"at index {index}")
        record_id = safe_component(raw_id, "ADB source id")
        archive_file = safe_component(
            required_string(record, "archive_file", record_id), "ADB archive file"
        )
        if record_id in seen_ids:
            raise RuntimeError(f"duplicate locked ADB source id: {record_id}")
        if archive_file in seen_archives:
            raise RuntimeError(f"duplicate locked ADB archive file: {archive_file}")
        seen_ids.add(record_id)
        seen_archives.add(archive_file)

        archive_identity = record.get("archive_identity", RAW_ARCHIVE_IDENTITY)
        if (
            not isinstance(archive_identity, str)
            or archive_identity not in SUPPORTED_ARCHIVE_IDENTITIES
        ):
            raise RuntimeError(
                f"unsupported ADB archive identity for {record_id}: {archive_identity}"
            )
        expected = required_string(record, "archive_sha256", record_id)
        if SHA256_PATTERN.fullmatch(expected) is None:
            raise RuntimeError(f"invalid archive_sha256 for locked ADB source {record_id}")

        url = record.get("download_url", record.get("archive_url"))
        if require_download_urls and (not isinstance(url, str) or not url):
            raise RuntimeError(f"invalid archive URL for locked ADB source {record_id}")
        if not isinstance(url, str):
            url = ""
        build_input = record.get("build_input", True)
        if not isinstance(build_input, bool):
            raise RuntimeError(f"invalid build_input for locked ADB source {record_id}")
        exclusions = record.get("excluded_build_symlinks", [])
        locked_symlink_exclusions(exclusions)

        validated.append(
            {
                "id": record_id,
                "archive_file": archive_file,
                "archive_identity": archive_identity,
                "archive_sha256": expected,
                "download_url": url,
                "build_input": build_input,
                "excluded_build_symlinks": exclusions,
            }
        )
    return validated


def validate_download_root_entries(
    download_root: pathlib.Path,
    locked_records: list[dict],
    maximum_size_bytes: Optional[int] = None,
) -> int:
    expected = {item["archive_file"] for item in locked_records}
    expected.add(PREFETCH_MANIFEST_NAME)
    actual_size_bytes = 0
    for path in download_root.iterdir():
        if path.name not in expected:
            raise RuntimeError(f"ADB source cache contains unlocked entry: {path.name}")
        if path.is_symlink() or not path.is_file():
            raise RuntimeError(f"ADB source cache entry is not a regular file: {path.name}")
        actual_size_bytes += path.stat().st_size
        if (
            maximum_size_bytes is not None
            and actual_size_bytes > maximum_size_bytes
        ):
            raise RuntimeError(
                "ADB source cache actual size exceeds the configured maximum: "
                f"{actual_size_bytes} > {maximum_size_bytes}"
            )
    return actual_size_bytes


def positive_byte_count(value: str) -> int:
    try:
        byte_count = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "maximum cache bytes must be a positive integer"
        ) from error
    if byte_count < 1:
        raise argparse.ArgumentTypeError(
            "maximum cache bytes must be a positive integer"
        )
    return byte_count


def positive_worker_count(value: str) -> int:
    try:
        workers = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("workers must be a positive integer") from error
    if workers < 1:
        raise argparse.ArgumentTypeError("workers must be a positive integer")
    return workers


def write_manifest_atomic(path: pathlib.Path, manifest: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Optional[pathlib.Path] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=path.parent, delete=False
        ) as stream:
            temporary = pathlib.Path(stream.name)
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--download-root", required=True, type=pathlib.Path)
    parser.add_argument("--extract-root", type=pathlib.Path)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--max-cache-bytes", type=positive_byte_count)
    parser.add_argument(
        "--workers", type=positive_worker_count, default=DEFAULT_DOWNLOAD_WORKERS
    )
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    locked_records = validated_records(lock, require_download_urls=not args.offline)
    args.download_root.mkdir(parents=True, exist_ok=True)

    missing = [
        item
        for item in locked_records
        if not (args.download_root / item["archive_file"]).is_file()
    ]
    if args.offline:
        if missing:
            archive = args.download_root / missing[0]["archive_file"]
            raise RuntimeError(f"prefetched ADB source archive is missing: {archive}")
    elif missing:
        cancel_event = threading.Event()
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
            downloads = [
                executor.submit(
                    download,
                    item["download_url"],
                    args.download_root / item["archive_file"],
                    cancel_event=cancel_event,
                )
                for item in missing
            ]
            try:
                for result in concurrent.futures.as_completed(downloads):
                    result.result()
            except BaseException:
                cancel_event.set()
                for pending in downloads:
                    pending.cancel()
                raise

    manifest = {"schema_version": 1, "lock": args.lock.name, "archives": []}
    for item in locked_records:
        archive = args.download_root / item["archive_file"]
        actual = sha256(archive)
        if (
            item["archive_identity"] == CANONICAL_TAR_GZ_IDENTITY
            and actual != item["archive_sha256"]
        ):
            canonicalize_tar_gz(archive, item["excluded_build_symlinks"])
            actual = sha256(archive)
        if actual != item["archive_sha256"]:
            archive.unlink(missing_ok=True)
            raise RuntimeError(
                f"ADB source archive sha256 mismatch for {item['id']}: "
                f"expected {item['archive_sha256']}, got {actual}"
            )
        archive_manifest = {
            "id": item["id"],
            "file": item["archive_file"],
            "sha256": actual,
            "archive_identity": item["archive_identity"],
        }

        if args.extract_root is not None and item["build_input"]:
            destination = args.extract_root / item["id"]
            if destination.exists():
                shutil.rmtree(destination)
            exclusions = safe_extract(
                archive,
                destination,
                item["excluded_build_symlinks"],
            )
            if exclusions:
                archive_manifest["excluded_build_symlinks"] = exclusions
        manifest["archives"].append(archive_manifest)

    manifest_path = args.download_root / PREFETCH_MANIFEST_NAME
    write_manifest_atomic(manifest_path, manifest)
    try:
        actual_size_bytes = validate_download_root_entries(
            args.download_root,
            locked_records,
            maximum_size_bytes=args.max_cache_bytes,
        )
    except BaseException:
        manifest_path.unlink(missing_ok=True)
        raise
    if args.max_cache_bytes is not None:
        print(
            "ADB source cache actual size: "
            f"{actual_size_bytes} / {args.max_cache_bytes} bytes"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
