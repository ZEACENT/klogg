#!/usr/bin/env python3
"""Deterministic, digest-locked ADB/iOS native core archives.

Existing build/legal verifiers remain mandatory qualification gates. This
format binds staged bytes to a caller-provided binary-core key; neither legacy
receipts nor the archive's own manifest authenticate a registry download.
The caller must obtain the expected archive SHA-256 and size from a separately
locked, verified OCI descriptor before accepting a downloaded archive.
"""

from __future__ import annotations

import ctypes
import gzip
import hashlib
import io
import json
import os
import pathlib
import posixpath
import re
import stat
import sys
import tarfile
import tempfile

from ci_dependency_artifact import MAX_CORE_BYTES, validate_archive_name, ArtifactError
from prefetch_adb_helper_sources import safe_extract

MANIFEST = ".ci-dependency-core.json"
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_MEMBERS = 256
CHUNK = 1024 * 1024
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
NAME = re.compile(r"[A-Za-z0-9_.+/-]+\Z")
IOS_LIB = re.compile(r"lib/lib[A-Za-z0-9_.+-]+\.dylib\Z")
# This closure is part of the locked ADB target plan. Keep this binary-only
# allowlist synchronized with the plans when a new target's core is introduced.
ADB_RUNTIME = {
    "linux-x86_64": ("libusb-1.0.so.0",),
    "linux-arm64": ("libusb-1.0.so.0",),
    "windows-x86_64": ("AdbWinApi.dll", "AdbWinUsbApi.dll", "libusb-1.0.dll"),
    "macos-x86_64": (),
    "macos-arm64": (),
}


def canonical_adb_mode(name: str, mode: int, target: str) -> int:
    """Normalize only the reviewed Windows helper and DLL NTFS/tar modes."""
    if target != "windows-x86_64":
        return mode
    if name == "helpers/adb.exe" and mode in (0o755, 0o777):
        return 0o755
    if name in {"helpers/" + item for item in ADB_RUNTIME[target]} and mode in (
            0o644, 0o666, 0o755, 0o777):
        return 0o644
    return mode


class CoreError(ValueError):
    """The core archive or staging tree violates its independent contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise CoreError(message)


def _target(component: str, target: str) -> None:
    try:
        validate_archive_name(f"{component}-{target}.tar.gz")
    except ArtifactError as error:
        raise CoreError("unsupported dependency core target") from error


def _digest(value: object, label: str) -> str:
    _require(isinstance(value, str) and DIGEST.fullmatch(value) is not None
             and value != "0" * 64, f"invalid {label}")
    return value


def _name(name: str) -> None:
    _require(isinstance(name, str) and len(name.encode("utf-8")) <= 100
             and NAME.fullmatch(name) is not None and "\\" not in name
             and not name.startswith("/")
             and all(part not in ("", ".", "..") for part in name.split("/")),
             f"unsafe core member name: {name}")


def _file_set(component: str, target: str, entries: dict) -> None:
    names = set(entries)
    if component == "adb-helper":
        _require(target in ADB_RUNTIME, "unsupported ADB closure target")
        helper = "helpers/adb.exe" if target.startswith("windows-") else "helpers/adb"
        expected = {helper} | {"helpers/" + name for name in ADB_RUNTIME[target]}
        _require(names == expected, "ADB core must contain exactly its binary closure")
        _require(all(entries[name]["type"] == "file" for name in names),
                 "ADB core may not contain links")
        _require(entries[helper]["mode"] == 0o755, "ADB helper is not executable")
    else:
        libs = {name for name in names if IOS_LIB.fullmatch(name)}
        _require(bool(libs) and names == libs, "iOS core must contain only dylibs")
        _require(any(entries[name]["type"] == "file" for name in libs),
                 "iOS core requires a real dylib")
    for name, entry in entries.items():
        if entry["type"] == "file":
            allowed = (0o644, 0o755) if name != "helpers/adb" and name != "helpers/adb.exe" else (0o755,)
            _require(entry["mode"] in allowed, f"unsafe file mode: {name}")
        else:
            _require(component == "ios-native" and name in libs
                     and entry["mode"] == 0o777, "unexpected core symlink")


def _links(entries: dict) -> None:
    for name, entry in entries.items():
        if entry["type"] != "symlink":
            continue
        link = entry["link"]
        _require(isinstance(link, str) and link and not link.startswith("/")
                 and "\\" not in link and "\x00" not in link,
                 f"unsafe core symlink: {name}")
        destination = posixpath.normpath(posixpath.join(posixpath.dirname(name), link))
        _require(destination.startswith("lib/") and destination in entries,
                 f"escaping or dangling core symlink: {name}")
        seen = {name}
        while entries[destination]["type"] == "symlink":
            _require(destination not in seen, f"cyclic core symlink: {name}")
            seen.add(destination)
            destination = posixpath.normpath(posixpath.join(
                posixpath.dirname(destination), entries[destination]["link"]))
            _require(destination.startswith("lib/") and destination in entries,
                     f"escaping or dangling core symlink: {name}")


def _hash_file(path: pathlib.Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        for data in iter(lambda: source.read(CHUNK), b""):
            size += len(data)
            _require(size <= MAX_CORE_BYTES, "core member exceeds size limit")
            digest.update(data)
    return digest.hexdigest(), size


def _staged(root: pathlib.Path, component: str, target: str) -> dict:
    _require(root.is_dir() and not root.is_symlink(), "missing or unsafe core root")
    entries = {}
    permitted_dirs = {"helpers"} if component == "adb-helper" else {"lib"}
    for directory, subdirs, files in os.walk(root, followlinks=False):
        for child in subdirs:
            child_path = pathlib.Path(directory) / child
            _require(not child_path.is_symlink()
                     and child_path.relative_to(root).as_posix() in permitted_dirs,
                     "unlisted or symlinked core directory")
        for child in files:
            path = pathlib.Path(directory) / child
            name = path.relative_to(root).as_posix()
            _name(name)
            _require(name != MANIFEST and name not in entries, "reserved or duplicate core path")
            info = path.lstat()
            if stat.S_ISREG(info.st_mode):
                sha, size = _hash_file(path)
                mode = stat.S_IMODE(info.st_mode)
                if component == "adb-helper":
                    mode = canonical_adb_mode(name, mode, target)
                entries[name] = {"type": "file", "mode": mode,
                                 "size": size, "sha256": sha}
            elif stat.S_ISLNK(info.st_mode):
                entries[name] = {"type": "symlink", "mode": 0o777,
                                 "link": os.readlink(path)}
            else:
                raise CoreError(f"unsupported core file type: {name}")
            _require(len(entries) < MAX_MEMBERS, "too many core members")
    _file_set(component, target, entries)
    _links(entries)
    _require(sum(item.get("size", 0) for item in entries.values()) <= MAX_CORE_BYTES,
             "core exceeds total size limit")
    return entries


def _json_bytes(document: dict) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=True, allow_nan=False) + "\n").encode("ascii")


def _info(name: str, entry: dict) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.uid = info.gid = info.mtime = 0
    info.uname = info.gname = ""
    info.mode = entry["mode"]
    if entry["type"] == "symlink":
        info.type = tarfile.SYMTYPE
        info.linkname = entry["link"]
    else:
        info.size = entry["size"]
    return info


class _CheckedReader:
    def __init__(self, path: pathlib.Path):
        self.source = path.open("rb")
        self.digest = hashlib.sha256()

    def read(self, size: int) -> bytes:
        data = self.source.read(size)
        self.digest.update(data)
        return data

    def close(self) -> None:
        self.source.close()


def package_core(root: pathlib.Path, archive: pathlib.Path, *, component: str,
                 target: str, core_identity: str) -> dict[str, object]:
    """Package a dedicated core-only stage; return its external SHA-256 and size.

    Receipts, legal assets, source archives and release overlays are external
    qualification/publication material. Run existing binary/legal verifiers before
    packaging; verifier-policy changes must not alter these binary-core bytes.
    """
    _target(component, target)
    _digest(core_identity, "binary-core identity")
    root, archive = pathlib.Path(root), pathlib.Path(archive)
    _require(not archive.exists() and not archive.is_symlink(), "core archive already exists")
    _require(not archive.parent.is_symlink() and archive.parent.is_dir(), "unsafe archive parent")
    try:
        archive.resolve().relative_to(root.resolve())
    except ValueError:
        pass
    else:
        raise CoreError("archive must be outside core root")
    entries = _staged(root, component, target)
    manifest = _json_bytes({"schema_version": 1, "component": component, "target": target,
                            "core_identity": core_identity, "files": entries})
    _require(len(manifest) <= MAX_MANIFEST_BYTES, "oversized core manifest")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=archive.parent, delete=False) as output:
            temporary = pathlib.Path(output.name)
            with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as zipped:
                with tarfile.open(fileobj=zipped, mode="w", format=tarfile.USTAR_FORMAT) as tar:
                    tar.addfile(_info(MANIFEST, {"mode": 0o644, "size": len(manifest),
                                                 "type": "file"}), io.BytesIO(manifest))
                    for name, entry in sorted(entries.items()):
                        info = _info(name, entry)
                        if entry["type"] == "symlink":
                            tar.addfile(info)
                        else:
                            reader = _CheckedReader(root / name)
                            try:
                                tar.addfile(info, reader)
                                _require(reader.digest.hexdigest() == entry["sha256"],
                                         f"core input changed while packaging: {name}")
                            finally:
                                reader.close()
        sha, size = _hash_file(temporary)
        _require(0 < size <= MAX_CORE_BYTES, "oversized core archive")
        verify_core(temporary, component=component, target=target,
                    core_identity=core_identity, expected_sha256=sha, expected_size=size)
        try:
            os.link(temporary, archive)
        except FileExistsError as error:
            raise CoreError("core archive already exists") from error
        return {"sha256": sha, "size": size}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _read_member(tar: tarfile.TarFile, member: tarfile.TarInfo, limit: int) -> bytes:
    _require(member.isfile() and member.size <= limit, "invalid core manifest")
    source = tar.extractfile(member)
    _require(source is not None, "unreadable core manifest")
    with source:
        data = source.read(limit + 1)
    _require(len(data) == member.size, "truncated core manifest")
    return data


def verify_core(archive: pathlib.Path, *, component: str, target: str,
                core_identity: str, expected_sha256: str,
                expected_size: int) -> dict[str, object]:
    """Verify independently locked compressed bytes, then exact tar and member semantics."""
    _target(component, target)
    _digest(core_identity, "binary-core identity")
    _digest(expected_sha256, "external archive SHA-256")
    _require(type(expected_size) is int and 0 < expected_size <= MAX_CORE_BYTES,
             "invalid external archive size")
    archive = pathlib.Path(archive)
    _require(not archive.is_symlink() and archive.is_file()
             and archive.stat().st_size == expected_size, "core archive size mismatch")
    sha, size = _hash_file(archive)
    _require(sha == expected_sha256 and size == expected_size,
             "external core archive digest mismatch")
    try:
        with tarfile.open(archive, mode="r:gz") as tar:
            members = tar.getmembers()
            _require(1 < len(members) <= MAX_MEMBERS, "invalid core member count")
            manifest_member = members[0]
            _require(manifest_member.name == MANIFEST, "missing first core manifest")
            raw = _read_member(tar, manifest_member, MAX_MANIFEST_BYTES)
            try:
                document = json.loads(raw)
            except (ValueError, UnicodeError) as error:
                raise CoreError("invalid core manifest JSON") from error
            _require(isinstance(document, dict) and set(document) == {
                "schema_version", "component", "target", "core_identity", "files"}
                     and type(document["schema_version"]) is int
                     and document["schema_version"] == 1
                     and document["component"] == component
                     and document["target"] == target
                     and document["core_identity"] == core_identity
                     and isinstance(document["files"], dict)
                     and raw == _json_bytes(document), "noncanonical or substituted core manifest")
            entries = document["files"]
            _require(len(entries) == len(members) - 1, "unexpected core archive members")
            for name, entry in entries.items():
                _name(name)
                _require(isinstance(entry, dict) and type(entry.get("mode")) is int
                         and ((set(entry) == {"type", "mode", "size", "sha256"}
                               and entry.get("type") == "file"
                               and type(entry.get("size")) is int
                               and 0 <= entry["size"] <= MAX_CORE_BYTES
                               and isinstance(entry.get("sha256"), str)
                               and DIGEST.fullmatch(entry["sha256"]) is not None)
                              or (set(entry) == {"type", "mode", "link"}
                                  and entry.get("type") == "symlink"
                                  and isinstance(entry.get("link"), str))),
                         f"invalid core file record: {name}")
            _file_set(component, target, entries)
            _links(entries)
            _require(sum(item.get("size", 0) for item in entries.values()) <= MAX_CORE_BYTES,
                     "oversized core contents")
            expected_order = [MANIFEST, *sorted(entries)]
            _require([member.name for member in members] == expected_order,
                     "unexpected, duplicate or reordered core member")
            for member in members:
                entry = ({"type": "file", "mode": 0o644, "size": len(raw)}
                         if member.name == MANIFEST else entries[member.name])
                _require(member.uid == 0 and member.gid == 0 and member.mtime == 0
                         and member.uname == member.gname == ""
                         and member.mode == entry["mode"] and not member.pax_headers
                         and member.linkname == entry.get("link", "")
                         and member.size == entry.get("size", 0)
                         and ((member.isfile() and entry["type"] == "file")
                              or (member.issym() and entry["type"] == "symlink")),
                         f"invalid core tar metadata: {member.name}")
                if member.name == MANIFEST or entry["type"] == "symlink":
                    continue
                source = tar.extractfile(member)
                _require(source is not None, "unreadable core member")
                digest = hashlib.sha256()
                with source:
                    for chunk in iter(lambda: source.read(CHUNK), b""):
                        digest.update(chunk)
                _require(digest.hexdigest() == entry["sha256"],
                         f"core member checksum mismatch: {member.name}")
    except (tarfile.TarError, EOFError, OSError, UnicodeError, OverflowError) as error:
        raise CoreError("invalid core tar/gzip archive") from error
    return {"component": component, "target": target, "core_identity": core_identity,
            "sha256": sha, "size": size}


def _require_private_parent(parent: pathlib.Path, *, platform: str = None) -> None:
    platform = os.name if platform is None else platform
    _require(parent.is_dir() and not parent.is_symlink(),
             "core destination requires an existing private parent")
    # Python 3.8 does not establish a private Windows ACL from mkdir(mode=0700).
    _require(platform == "posix", "Windows core staging requires audited private ACL support")
    info = parent.stat()
    _require(info.st_uid == os.geteuid() and stat.S_IMODE(info.st_mode) & 0o077 == 0,
             "core destination parent must belong to this user and be private")


def _publish_no_replace(source: pathlib.Path, destination: pathlib.Path) -> None:
    """Publish a directory atomically without replacing a racing destination."""
    if os.name == "nt":
        os.rename(source, destination)
        return
    library = ctypes.CDLL(None, use_errno=True)
    try:
        if sys.platform == "darwin":
            publish = library.renamex_np
            publish.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
            arguments = (os.fsencode(source), os.fsencode(destination), 0x00000004)
        elif sys.platform.startswith("linux"):
            publish = library.renameat2
            publish.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                                ctypes.c_char_p, ctypes.c_uint)
            arguments = (-100, os.fsencode(source), -100, os.fsencode(destination), 1)
        else:
            raise CoreError("no atomic no-replace directory publish for this platform")
    except AttributeError as error:
        raise CoreError("atomic no-replace directory publish unavailable") from error
    publish.restype = ctypes.c_int
    if publish(*arguments) != 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(destination))


def stage_core(archive: pathlib.Path, destination: pathlib.Path, *, component: str,
               target: str, core_identity: str, expected_sha256: str,
               expected_size: int) -> dict[str, object]:
    """Stage only core binaries under a private parent after external lock authentication.

    The caller must authenticate the digest and core identity from signed
    production evidence first. This does not qualify a candidate or assemble
    versioned legal assets; no product workflow calls it without that boundary.
    """
    archive, destination = pathlib.Path(archive), pathlib.Path(destination)
    parent = destination.parent
    _require_private_parent(parent)
    _require(not destination.exists() and not destination.is_symlink(),
             "core destination already exists")
    _require(archive.is_file() and not archive.is_symlink(), "missing safe core archive")
    _require(type(expected_size) is int and 0 < expected_size <= MAX_CORE_BYTES,
             "invalid expected core archive size")
    try:
        with tempfile.TemporaryDirectory(prefix=".native-core-", dir=parent) as temporary:
            workspace = pathlib.Path(temporary)
            snapshot = workspace / "core.tar.gz"
            copied = 0
            with archive.open("rb") as source, snapshot.open("xb") as output:
                for chunk in iter(lambda: source.read(CHUNK), b""):
                    copied += len(chunk)
                    _require(copied <= expected_size, "core archive exceeds locked size")
                    output.write(chunk)
            _require(copied == expected_size, "core archive differs from locked size")
            verified = verify_core(snapshot, component=component, target=target,
                                   core_identity=core_identity,
                                   expected_sha256=expected_sha256,
                                   expected_size=expected_size)
            with tarfile.open(snapshot, "r:gz") as tar:
                raw = _read_member(tar, tar.getmembers()[0], MAX_MANIFEST_BYTES)
                entries = json.loads(raw)["files"]
            stage = workspace / "stage"
            safe_extract(snapshot, stage)
            (stage / MANIFEST).unlink()
            _require(_staged(stage, component, target) == entries,
                     "staged core binary closure differs from verified archive")
            sha, size = _hash_file(snapshot)
            _require(sha == expected_sha256 and size == expected_size,
                     "core archive changed during staging")
            _require(not destination.exists() and not destination.is_symlink(),
                     "core destination appeared during staging")
            _publish_no_replace(stage, destination)
            return verified
    except (OSError, RuntimeError, ValueError, KeyError, IndexError,
            OverflowError, tarfile.TarError) as error:
        raise CoreError("cannot safely stage verified native core") from error
