#!/usr/bin/env python3
"""Prefetch and verify the immutable source closure for the bundled ADB helper."""

from __future__ import annotations

import argparse
import contextlib
import gzip
import hashlib
import json
import os
import pathlib
import shutil
import tarfile
import tempfile
import urllib.request


RAW_ARCHIVE_IDENTITY = "raw-sha256"
CANONICAL_TAR_GZ_IDENTITY = "canonical-tar-gz-v1"
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
    for item in excluded_build_symlinks or []:
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
) -> tuple[dict[tuple[str, ...], tarfile.TarInfo], list[dict[str, str]]]:
    exclusions = locked_symlink_exclusions(excluded_build_symlinks)
    matched_exclusions: set[tuple[str, ...]] = set()
    seen: set[tuple[str, ...]] = set()
    by_name: dict[tuple[str, ...], tarfile.TarInfo] = {}
    for member in tar.getmembers():
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

    missing_exclusions = exclusions.keys() - matched_exclusions
    if missing_exclusions:
        missing = ", ".join(exclusions[parts]["path"] for parts in missing_exclusions)
        raise RuntimeError(f"excluded build symlink is missing from archive: {missing}")
    return by_name, [exclusions[parts] for parts in exclusions if parts in matched_exclusions]


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
            members, _ = validated_archive_members(
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


def safe_extract(
    archive: pathlib.Path,
    destination: pathlib.Path,
    excluded_build_symlinks: object = None,
) -> list[dict[str, str]]:
    destination.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or any(destination.iterdir()):
        raise RuntimeError(f"archive extraction destination must be an empty directory: {destination}")

    with tarfile.open(archive, "r:*") as tar:
        by_name, matched_exclusions = validated_archive_members(
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
            os.symlink(member.linkname, path)

    children = [path for path in destination.iterdir() if path.name != ".DS_Store"]
    if len(children) == 1 and children[0].is_dir():
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
            resolved.relative_to(extraction_root)
        except (OSError, RuntimeError, ValueError) as error:
            raise RuntimeError(
                f"archive symlink escapes extraction root after layout normalization: {path}"
            ) from error

    return matched_exclusions


def download(url: str, destination: pathlib.Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = pathlib.Path(stream.name)
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "klogg-adb-source-prefetch/1"})
        with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def records(lock: dict) -> list[dict]:
    return [
        *lock.get("sources", []),
        *lock.get("dependencies", []),
        *lock.get("toolchain_packages", []),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--download-root", required=True, type=pathlib.Path)
    parser.add_argument("--extract-root", type=pathlib.Path)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    args.download_root.mkdir(parents=True, exist_ok=True)
    manifest = {"schema_version": 1, "lock": args.lock.name, "archives": []}

    seen_ids: set[str] = set()
    seen_archives: set[str] = set()
    for record in records(lock):
        record_id = safe_component(str(record["id"]), "ADB source id")
        archive_file = safe_component(str(record["archive_file"]), "ADB archive file")
        if record_id in seen_ids:
            raise RuntimeError(f"duplicate locked ADB source id: {record_id}")
        if archive_file in seen_archives:
            raise RuntimeError(f"duplicate locked ADB archive file: {archive_file}")
        seen_ids.add(record_id)
        seen_archives.add(archive_file)
        archive_identity = str(record.get("archive_identity", RAW_ARCHIVE_IDENTITY))
        if archive_identity not in SUPPORTED_ARCHIVE_IDENTITIES:
            raise RuntimeError(
                f"unsupported ADB archive identity for {record_id}: {archive_identity}"
            )
        archive = args.download_root / archive_file
        if not archive.is_file():
            if args.offline:
                raise RuntimeError(f"prefetched ADB source archive is missing: {archive}")
            download(record.get("download_url", record["archive_url"]), archive)
        expected = record["archive_sha256"]
        actual = sha256(archive)
        if archive_identity == CANONICAL_TAR_GZ_IDENTITY and actual != expected:
            canonicalize_tar_gz(archive, record.get("excluded_build_symlinks", []))
            actual = sha256(archive)
        if actual != expected:
            archive.unlink(missing_ok=True)
            raise RuntimeError(
                f"ADB source archive sha256 mismatch for {record['id']}: expected {expected}, got {actual}"
            )
        archive_manifest = {
            "id": record_id,
            "file": archive_file,
            "sha256": actual,
            "archive_identity": archive_identity,
        }

        if args.extract_root is not None and (record.get("build_input", True)):
            destination = args.extract_root / record_id
            if destination.exists():
                shutil.rmtree(destination)
            exclusions = safe_extract(
                archive,
                destination,
                record.get("excluded_build_symlinks", []),
            )
            if exclusions:
                archive_manifest["excluded_build_symlinks"] = exclusions
        manifest["archives"].append(archive_manifest)

    (args.download_root / "adb-helper-prefetch-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
