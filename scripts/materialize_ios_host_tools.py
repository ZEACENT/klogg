#!/usr/bin/env python3
"""Snapshot and independently verify an unreviewed iOS host tool tree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import posixpath
import shutil
import stat
import tempfile

from ci_dependency_core import CoreError, _publish_no_replace

MAX_FILES = 20000
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_DEPTH = 24
MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MANIFEST_NAME = "manifest.json"
ARCHITECTURES = ("x86_64", "arm64")
DIRECTORY_FLAGS = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)


class MaterializationError(ValueError):
    """An input or materialized tool tree is not an exact safe snapshot."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise MaterializationError(message)


def _require_posix_host() -> None:
    require(os.name == "posix" and hasattr(os, "geteuid")
            and os.scandir in getattr(os, "supports_fd", ())
            and os.open in getattr(os, "supports_dir_fd", ()),
            "iOS host snapshot requires POSIX descriptor-relative filesystem support")


def _canonical(document: dict) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def _same_file(before, after) -> bool:
    fields = ("st_dev", "st_ino", "st_mode", "st_nlink", "st_size", "st_mtime_ns", "st_ctime_ns")
    return all(getattr(before, field) == getattr(after, field) for field in fields)


def _regular_bytes(path, parent_fd=None, *, max_bytes=None) -> tuple[bytes, os.stat_result]:
    limit = MAX_FILE_BYTES if max_bytes is None else max_bytes
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    try:
        with os.fdopen(os.open(path, flags, dir_fd=parent_fd), "rb") as opened:
            before = os.fstat(opened.fileno())
            require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1,
                    "tool tree rejects hardlinks and special files")
            require(before.st_size <= limit, "tool input exceeds single-file budget")
            content = opened.read(limit + 1)
            after = os.fstat(opened.fileno())
    except OSError as error:
        raise MaterializationError("cannot safely open tool input") from error
    require(len(content) == before.st_size and _same_file(before, after),
            "tool input changed while reading")
    return content, before


def _open_root(path: pathlib.Path) -> int:
    """Hold the requested real directory through descriptor-relative traversal."""
    try:
        original = path.lstat()
        require(stat.S_ISDIR(original.st_mode), "tool root must be a real directory")
        resolved = path.resolve(strict=True)
        current = os.open(resolved.anchor, DIRECTORY_FLAGS)
        try:
            for component in resolved.parts[1:]:
                next_fd = os.open(component, DIRECTORY_FLAGS, dir_fd=current)
                os.close(current)
                current = next_fd
            require((os.fstat(current).st_dev, os.fstat(current).st_ino)
                    == (original.st_dev, original.st_ino),
                    "tool root changed while opening")
            return current
        except BaseException:
            os.close(current)
            raise
    except OSError as error:
        raise MaterializationError("cannot open real tool directory") from error


def _link_target(path: str, target: str, by_path: dict | None = None) -> str:
    require(isinstance(target, str) and target and not target.startswith("/")
            and not target.endswith("/"), "tool symlink must be relative to a file")
    parts = posixpath.dirname(path).split("/") if "/" in path else []
    segments = target.split("/")
    for index, component in enumerate(segments):
        if component == "..":
            require(parts, "tool symlink leaves the materialized root")
            parts.pop()
        elif component not in ("", "."):
            parts.append(component)
        if by_path is not None and index < len(segments) - 1 and parts:
            parent = by_path.get("/".join(parts))
            require(parent is not None and parent["type"] == "directory",
                    "tool symlink traverses a missing or non-directory component")
    resolved = "/".join(parts)
    require(resolved, "tool symlink leaves the materialized root")
    return resolved


def _validate_links(records: list[dict]) -> None:
    by_path = {record["path"]: record for record in records}
    endpoints = {}
    for record in records:
        if record["type"] != "symlink":
            continue
        chain = []
        seen = set()
        current = record["path"]
        while current not in endpoints:
            require(current not in seen, "cyclic tool symlink")
            seen.add(current)
            linked = by_path.get(current)
            require(linked is not None, "dangling tool symlink")
            if linked["type"] == "file":
                endpoints[current] = current
                break
            require(linked["type"] == "symlink", "tool symlink must resolve to a file")
            chain.append(current)
            current = _link_target(current, linked["target"], by_path)
        for alias in chain:
            endpoints[alias] = endpoints[current]


def _inventory_fd(root_fd: int, *, normalized: bool) -> list[dict]:
    records = []
    total = 0
    root_mode = stat.S_IMODE(os.fstat(root_fd).st_mode)
    require(stat.S_ISDIR(os.fstat(root_fd).st_mode), "tool root is not a directory")
    if normalized:
        require(root_mode == 0o700, "materialized tool root is not private")
    else:
        require(not root_mode & 0o7002, "unsafe source tool root permissions")

    def walk(directory_fd: int, prefix: str, depth: int) -> None:
        nonlocal total
        require(depth <= MAX_DEPTH, "tool tree exceeds directory depth")
        try:
            names = []
            with os.scandir(directory_fd) as iterator:
                for entry in iterator:
                    names.append(entry.name)
                    require(len(names) + len(records) <= MAX_FILES,
                            "tool tree exceeds file-count budget")
        except OSError as error:
            raise MaterializationError("cannot list tool directory") from error
        for name in sorted(names):
            require(name not in ("", ".", "..") and "/" not in name and "\x00" not in name,
                    "invalid tool path component")
            relative = prefix + name
            if relative == MANIFEST_NAME:
                require(normalized, "source tool tree contains a reserved manifest name")
                continue
            try:
                require(len(relative.encode("utf-8")) <= 1024, "tool path too long")
                info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            except (OSError, UnicodeError) as error:
                raise MaterializationError("tool entry disappeared or has invalid path") from error
            mode = stat.S_IMODE(info.st_mode)
            if stat.S_ISDIR(info.st_mode):
                require(not mode & 0o7002, "unsafe tool directory permissions")
                if normalized:
                    require(mode == 0o755, "materialized directory mode changed")
                records.append({"path": relative, "type": "directory", "mode": 0o755})
                try:
                    child = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
                except OSError as error:
                    raise MaterializationError("tool directory changed during traversal") from error
                try:
                    require((os.fstat(child).st_dev, os.fstat(child).st_ino)
                            == (info.st_dev, info.st_ino),
                            "tool directory changed during traversal")
                    walk(child, relative + "/", depth + 1)
                finally:
                    os.close(child)
            elif stat.S_ISLNK(info.st_mode):
                try:
                    target = os.readlink(name, dir_fd=directory_fd)
                    require((os.stat(name, dir_fd=directory_fd, follow_symlinks=False).st_dev,
                             os.stat(name, dir_fd=directory_fd, follow_symlinks=False).st_ino)
                            == (info.st_dev, info.st_ino),
                            "tool symlink changed during inventory")
                except OSError as error:
                    raise MaterializationError("tool symlink changed during inventory") from error
                _link_target(relative, target)
                records.append({"path": relative, "type": "symlink", "target": target})
            elif stat.S_ISREG(info.st_mode):
                require(not mode & 0o7002, "unsafe tool file permissions")
                content, opened = _regular_bytes(name, directory_fd)
                require(_same_file(info, opened),
                        "tool entry changed after inventory")
                total += len(content)
                require(total <= MAX_TOTAL_BYTES, "tool tree exceeds aggregate budget")
                executable = 0o755 if mode & 0o111 else 0o644
                if normalized:
                    require(mode == executable, "materialized file mode changed")
                records.append({"path": relative, "type": "file", "mode": executable,
                                "size": len(content), "sha256": hashlib.sha256(content).hexdigest()})
            else:
                raise MaterializationError("tool tree contains a special file")
            require(len(records) <= MAX_FILES, "tool tree exceeds file-count budget")

    walk(root_fd, "", 0)
    require(any(record["type"] == "file" for record in records),
            "tool tree contains no files")
    ordered = sorted(records, key=lambda row: row["path"])
    _validate_links(ordered)
    return ordered


def _inventory(root: pathlib.Path, *, normalized: bool = False) -> list[dict]:
    root_fd = _open_root(pathlib.Path(root))
    try:
        return _inventory_fd(root_fd, normalized=normalized)
    finally:
        os.close(root_fd)


def _read_relative(root_fd: int, path: str) -> bytes:
    pieces = path.split("/")
    opened = []
    directory_fd = root_fd
    try:
        for name in pieces[:-1]:
            directory_fd = os.open(name, DIRECTORY_FLAGS, dir_fd=directory_fd)
            opened.append(directory_fd)
        content, _ = _regular_bytes(pieces[-1], directory_fd)
        return content
    except OSError as error:
        raise MaterializationError("tool input changed before materialization") from error
    finally:
        for fd in reversed(opened):
            os.close(fd)


def _document(architecture: str, records: list[dict]) -> dict:
    require(architecture in ARCHITECTURES, "unsupported iOS host architecture")
    identity = hashlib.sha256(_canonical({"architecture": architecture,
                                          "entries": records})).hexdigest()
    return {"schema_version": 1, "kind": "unreviewed-ios-host-tool-tree",
            "architecture": architecture, "entries": records,
            "identity": identity, "complete_host_closure": False}


def verify_materialized(destination: pathlib.Path, manifest: pathlib.Path,
                        architecture: str) -> dict:
    _require_posix_host()
    require(architecture in ARCHITECTURES, "unsupported iOS host architecture")
    destination = pathlib.Path(destination)
    manifest = pathlib.Path(manifest)
    require(manifest == destination / MANIFEST_NAME,
            "tool manifest must be inside the materialized root")
    require(manifest.is_file() and not manifest.is_symlink(), "missing regular tool manifest")
    content, info = _regular_bytes(manifest, max_bytes=MAX_MANIFEST_BYTES)
    require(stat.S_IMODE(info.st_mode) == 0o600, "tool manifest is not private")
    try:
        document = json.loads(content)
    except (ValueError, UnicodeError) as error:
        raise MaterializationError("invalid tool manifest") from error
    require(isinstance(document, dict) and set(document) == {
        "schema_version", "kind", "architecture", "entries", "identity", "complete_host_closure"},
        "unsupported tool manifest schema")
    records = _inventory(destination, normalized=True)
    require(_canonical(_document(architecture, records)) == content,
            "materialized tree differs from exact canonical manifest")
    return document


def materialize(source: pathlib.Path, destination: pathlib.Path, manifest: pathlib.Path,
                architecture: str) -> dict:
    _require_posix_host()
    require(architecture in ARCHITECTURES, "unsupported iOS host architecture")
    source = pathlib.Path(source)
    destination = pathlib.Path(destination)
    manifest = pathlib.Path(manifest)
    require(manifest == destination / MANIFEST_NAME,
            "tool manifest must be inside the materialized root")
    require(not destination.exists() and not destination.is_symlink(), "tool output already exists")
    require(destination.parent.is_dir() and not destination.parent.is_symlink(),
            "tool output requires a real private parent")
    parent = destination.parent.resolve(strict=True)
    parent_mode = stat.S_IMODE(parent.stat().st_mode)
    require(parent.stat().st_uid == os.geteuid() and not parent_mode & 0o077,
            "tool output parent must be private")
    root_fd = _open_root(source)
    try:
        records = _inventory_fd(root_fd, normalized=False)
        stage = pathlib.Path(tempfile.mkdtemp(prefix=".ios-host-stage-", dir=parent))
        stage.chmod(0o700)
        try:
            for record in records:
                path = stage / record["path"]
                if record["type"] == "directory":
                    path.mkdir(mode=0o755)
                    path.chmod(0o755)
                elif record["type"] == "file":
                    content = _read_relative(root_fd, record["path"])
                    require(len(content) == record["size"]
                            and hashlib.sha256(content).hexdigest() == record["sha256"],
                            "tool input changed before materialization")
                    with path.open("xb") as staged:
                        staged.write(content)
                    path.chmod(record["mode"])
                else:
                    path.symlink_to(record["target"])
            copied = _inventory(stage, normalized=True)
            require(copied == records, "materialized bytes changed during staging")
            document = _document(architecture, copied)
            encoded = _canonical(document)
            require(len(encoded) <= MAX_MANIFEST_BYTES, "tool manifest exceeds size limit")
            stage_manifest = stage / MANIFEST_NAME
            try:
                with stage_manifest.open("xb") as output:
                    output.write(encoded)
                stage_manifest.chmod(0o600)
                verify_materialized(stage, stage_manifest, architecture)
                require(not destination.exists() and not destination.is_symlink(),
                        "tool output collision during publication")
                _publish_no_replace(stage, destination)
            except (OSError, CoreError) as error:
                raise MaterializationError("cannot publish tool snapshot") from error
            return document
        finally:
            if stage.exists() and not stage.is_symlink():
                shutil.rmtree(stage)
    finally:
        os.close(root_fd)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("materialize", "verify"))
    parser.add_argument("--destination", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--architecture", choices=ARCHITECTURES, required=True)
    parser.add_argument("--source", type=pathlib.Path)
    args = parser.parse_args(argv)
    if args.operation == "materialize":
        if args.source is None:
            parser.error("materialize requires --source")
        result = materialize(args.source, args.destination, args.manifest, args.architecture)
    else:
        if args.source is not None:
            parser.error("verify does not accept --source")
        result = verify_materialized(args.destination, args.manifest, args.architecture)
    print(result["identity"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
