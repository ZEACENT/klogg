#!/usr/bin/env python3
"""Download and hash-lock native MINGW64 build tools; transport only their runtime closure."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import pathlib
import posixpath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LOCK = ROOT / "ci/tools/msys2-tools.json"
PREFIX = "msys64/"
MAX_ENTRY_SIZE = 128 * 1024 * 1024
MAX_SOURCE_SIZE = 512 * 1024 * 1024
DOWNLOAD_ATTEMPTS = 3


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_path(name: str) -> bool:
    return (bool(name) and not name.startswith(("/", "\\"))
            and "\\" not in name and ":" not in name
            and all(segment not in ("", ".", "..") for segment in name.split("/")))


def load_lock(path: pathlib.Path) -> dict:
    lock = json.loads(path.read_text(encoding="utf-8"))
    packages = lock.get("packages")
    if lock.get("schema_version") != 1 or not isinstance(packages, list) or not packages:
        raise RuntimeError("invalid MSYS2 tools lock")
    names = set()
    for package in packages:
        name = package["name"]
        version = package["version"]
        filename = f"mingw-w64-x86_64-{name}-{version}-any.pkg.tar.zst"
        escaped = filename.replace("++", "%2B%2B")
        url = "https://repo.msys2.org/mingw/mingw64/" + escaped
        files = package["runtime_files"]
        prefixes = package.get("runtime_prefixes", [])
        if (not re.fullmatch(r"[a-z0-9+\-]+", name)
                or not re.fullmatch(r"[0-9A-Za-z.~+-]+", version)
                or package["archive_file"] != filename or package["archive_url"] != url
                or not re.fullmatch(r"[0-9a-f]{64}", package["archive_sha256"])
                or name in names or not isinstance(package["license"], str)
                or not package["license"] or not isinstance(files, list)
                or not isinstance(prefixes, list)
                or any(not isinstance(file, str) or not safe_path(file)
                       or not file.startswith("mingw64/") for file in files)
                or any(not isinstance(prefix, str) or not safe_path(prefix.rstrip("/"))
                       or not prefix.startswith("mingw64/share/zoneinfo/")
                       or not prefix.endswith("/") for prefix in prefixes)):
            raise RuntimeError(f"invalid MSYS2 tool package lock: {name}")
        names.add(name)
    for package in packages:
        edges = package["depends"]
        if not isinstance(edges, list) or len(set(edges)) != len(edges) or any(
                dep not in names or dep == package["name"] for dep in edges):
            raise RuntimeError(f"incomplete MSYS2 runtime dependency closure: {package['name']}")
    sources = lock.get("corresponding_sources", [])
    if not isinstance(sources, list):
        raise RuntimeError("invalid MSYS2 corresponding-source lock")
    source_names = set()
    versions = {package["name"]: package["version"] for package in packages}
    for source in sources:
        name = source["name"]
        package = "cc-libs" if name == "gcc" else name
        if package not in versions or name in source_names:
            raise RuntimeError(f"invalid MSYS2 corresponding source: {name}")
        filename = f"mingw-w64-{name}-{versions[package]}.src.tar.zst"
        url = "https://repo.msys2.org/mingw/sources/" + filename
        if (source["archive_file"] != filename or source["archive_url"] != url
                or not re.fullmatch(r"[0-9a-f]{64}", source["archive_sha256"])
                or not isinstance(source["license_obligation"], str)
                or not source["license_obligation"]):
            raise RuntimeError(f"invalid MSYS2 corresponding-source lock: {name}")
        source_names.add(name)
    return lock


def check_archive(path: pathlib.Path, expected: str) -> None:
    if not path.is_file() or path.is_symlink():
        raise RuntimeError(f"MSYS2 archive missing or not a regular file: {path}")
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(chunk)
    if sha.hexdigest() != expected:
        raise RuntimeError(f"MSYS2 archive SHA-256 mismatch: {path}")


def download(url: str, target: pathlib.Path) -> None:
    with urllib.request.urlopen(url, timeout=90) as response, target.open("wb") as output:
        shutil.copyfileobj(response, output)


def verified_archive(cache: pathlib.Path, entry: dict, *, require_prefetched: bool,
                     downloader) -> pathlib.Path:
    archive = cache / entry["archive_file"]
    if not archive.exists() and not archive.is_symlink():
        if require_prefetched:
            raise RuntimeError(f"prefetched MSYS2 archive required: {archive}")
        cache.mkdir(parents=True, exist_ok=True)
        for attempt in range(DOWNLOAD_ATTEMPTS):
            with tempfile.TemporaryDirectory(prefix="msys2-download-", dir=cache) as temporary:
                candidate = pathlib.Path(temporary) / entry["archive_file"]
                try:
                    (downloader or download)(entry["archive_url"], candidate)
                except OSError as error:
                    if isinstance(error, urllib.error.HTTPError):
                        code = error.code
                        error.close()
                        if code not in (408, 429, 500, 502, 503, 504):
                            raise RuntimeError(f"MSYS2 download rejected: {archive}: HTTP {code}") from error
                    if attempt + 1 == DOWNLOAD_ATTEMPTS:
                        raise RuntimeError(f"MSYS2 download failed after {DOWNLOAD_ATTEMPTS} attempts: {archive}") from error
                    time.sleep(attempt + 1)
                    continue
                check_archive(candidate, entry["archive_sha256"])
                candidate.replace(archive)
                break
    check_archive(archive, entry["archive_sha256"])
    return archive


def decompress(path: pathlib.Path) -> io.BytesIO:
    if not shutil.which("zstd"):
        raise RuntimeError("native Windows runner requires zstd.exe for pinned MSYS2 .zst archives")
    result = subprocess.run(["zstd", "-d", "-c", str(path)], capture_output=True, check=False)
    if result.returncode:
        raise RuntimeError(f"zstd decompression failed: {result.stderr.decode(errors='replace')}")
    return io.BytesIO(result.stdout)


def package_contents(archive: pathlib.Path, package: dict, decompressor) -> dict[str, bytes]:
    selected = set(package["runtime_files"])
    prefixes = package.get("runtime_prefixes", [])
    files: dict[str, bytes] = {}
    links: dict[str, str] = {}
    metadata = None
    with decompressor(archive) as stream, tarfile.open(fileobj=stream, mode="r|") as tar:
        for member in tar:
            name = member.name.rstrip("/")
            if not safe_path(name):
                raise RuntimeError(f"unsafe MSYS2 archive path: {member.name}")
            if member.isdir():
                continue
            wanted = (name == ".PKGINFO" or name in selected or any(
                name.startswith(prefix) for prefix in prefixes)
                or name.startswith("mingw64/share/licenses/"))
            if member.islnk() and any(name.startswith(prefix) for prefix in prefixes):
                target = member.linkname
                if not safe_path(target) or not any(target.startswith(prefix) for prefix in prefixes):
                    raise RuntimeError(f"unsafe MSYS2 archive path: {target}")
                links[name] = target
                continue
            if member.issym() and any(name.startswith(prefix) for prefix in prefixes):
                target = posixpath.normpath(posixpath.join(posixpath.dirname(name), member.linkname))
                if not any(target.startswith(prefix) for prefix in prefixes):
                    raise RuntimeError(f"unsafe MSYS2 archive path: {member.linkname}")
                links[name] = target
                continue
            if not member.isfile():
                raise RuntimeError(f"unsafe MSYS2 archive entry: {name}")
            if wanted:
                if member.size > MAX_ENTRY_SIZE or name in files:
                    raise RuntimeError(f"invalid MSYS2 archive member: {name}")
                extracted = tar.extractfile(member)
                if extracted is None:
                    raise RuntimeError(f"missing MSYS2 archive member: {name}")
                data = extracted.read(MAX_ENTRY_SIZE + 1)
                if len(data) != member.size:
                    raise RuntimeError(f"invalid MSYS2 archive member size: {name}")
                if name == ".PKGINFO":
                    metadata = data.decode("utf-8")
                else:
                    files[name] = data
    for name in links:
        target = links[name]
        visited = {name}
        while target in links:
            if target in visited:
                raise RuntimeError(f"cyclic MSYS2 archive path: {name}")
            visited.add(target)
            target = links[target]
        if target not in files or name in files:
            raise RuntimeError(f"missing MSYS2 timezone target: {target}")
        files[name] = files[target]
    pkgname = f"mingw-w64-x86_64-{package['name']}"
    if (metadata is None
            or f"pkgname = {pkgname}" not in metadata.splitlines()
            or f"pkgver = {package['version']}" not in metadata.splitlines()):
        raise RuntimeError(f"MSYS2 package name/version mismatch: {pkgname}")
    if selected - files.keys() or any(not any(name.startswith(prefix) for name in files)
                                      for prefix in prefixes):
        raise RuntimeError(f"MSYS2 package runtime files missing: {pkgname}")
    return files


def materialize(cache: pathlib.Path, destination: pathlib.Path, lock_path: pathlib.Path = LOCK,
                *, require_prefetched: bool = False, downloader=None, decompressor=None) -> None:
    lock = load_lock(lock_path)
    cache, destination = pathlib.Path(cache), pathlib.Path(destination)
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f"MSYS2 tools artifact already exists: {destination}")
    files: dict[str, bytes] = {}
    for package in lock["packages"]:
        archive = verified_archive(cache, package, require_prefetched=require_prefetched,
                                   downloader=downloader)
        for name, data in package_contents(archive, package, decompressor or decompress).items():
            output_name = PREFIX + name
            if output_name in files and files[output_name] != data:
                raise RuntimeError(f"conflicting MSYS2 runtime file: {name}")
            files[output_name] = data
    sources = {}
    for source in lock.get("corresponding_sources", []):
        archive = verified_archive(cache, source, require_prefetched=require_prefetched,
                                   downloader=downloader)
        if archive.stat().st_size > MAX_SOURCE_SIZE:
            raise RuntimeError(f"MSYS2 source archive exceeds limit: {archive}")
        sources["corresponding-source/" + source["archive_file"]] = archive
    fingerprint = digest(json.dumps(lock, sort_keys=True).encode())
    envelope = {"schema_version": 1, "lock_sha256": fingerprint,
                "files": {name: digest(data) for name, data in sorted(files.items())}}
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, suffix=".zip", delete=False) as temporary:
        staged = pathlib.Path(temporary.name)
    try:
        with zipfile.ZipFile(staged, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for name, data in sorted(files.items()):
                output.writestr(name, data)
            for name, archive in sorted(sources.items()):
                output.write(archive, name, compress_type=zipfile.ZIP_STORED)
            output.writestr("msys2-tools-manifest.json", json.dumps(envelope, sort_keys=True))
        try:
            os.link(staged, destination)
        except FileExistsError as error:
            raise RuntimeError(f"MSYS2 tools artifact already exists: {destination}") from error
    finally:
        staged.unlink(missing_ok=True)


def extract_artifact(archive: pathlib.Path, destination: pathlib.Path,
                     lock_path: pathlib.Path = LOCK) -> None:
    lock = load_lock(lock_path)
    archive, destination = pathlib.Path(archive), pathlib.Path(destination)
    if archive.is_symlink() or not archive.is_file() or destination.is_symlink():
        raise RuntimeError("MSYS2 artifact missing or destination is a symlink")
    with zipfile.ZipFile(archive) as zip_file:
        names = zip_file.namelist()
        if (len(names) != len(set(names)) or len(names) != len({name.casefold() for name in names})
                or "msys2-tools-manifest.json" not in names
                or any(not safe_path(name) for name in names)):
            raise RuntimeError("unsafe MSYS2 artifact path or duplicate entry")
        if any(info.create_system == 3 and stat.S_IFMT(info.external_attr >> 16) == stat.S_IFLNK
               for info in zip_file.infolist()):
            raise RuntimeError("MSYS2 artifact symlink entries are forbidden")
        envelope = json.loads(zip_file.read("msys2-tools-manifest.json"))
        fingerprints = envelope.get("files")
        sources = {"corresponding-source/" + source["archive_file"]: source["archive_sha256"]
                   for source in lock.get("corresponding_sources", [])}
        if (envelope.get("schema_version") != 1
                or envelope.get("lock_sha256") != digest(json.dumps(lock, sort_keys=True).encode())
                or not isinstance(fingerprints, dict)
                or set(names) != set(fingerprints) | set(sources) | {"msys2-tools-manifest.json"}):
            raise RuntimeError("MSYS2 artifact lock or file closure mismatch")
        for name, expected in sources.items():
            if zip_file.getinfo(name).file_size > MAX_SOURCE_SIZE:
                raise RuntimeError(f"MSYS2 corresponding source exceeds limit: {name}")
            sha = hashlib.sha256()
            with zip_file.open(name) as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    sha.update(chunk)
            if sha.hexdigest() != expected:
                raise RuntimeError(f"MSYS2 corresponding source SHA-256 mismatch: {name}")
        required = {PREFIX + name for package in lock["packages"]
                    for name in package["runtime_files"]}
        prefixes = tuple(PREFIX + prefix for package in lock["packages"]
                         for prefix in package.get("runtime_prefixes", []))
        if not required <= set(fingerprints) or any(
                name not in required and not name.startswith(prefixes)
                and not name.startswith("msys64/mingw64/share/licenses/") for name in fingerprints):
            raise RuntimeError("MSYS2 artifact contains unexpected or missing runtime files")
        content = {}
        for name, expected in fingerprints.items():
            if (not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)
                    or not safe_path(name) or zip_file.getinfo(name).is_dir()
                    or zip_file.getinfo(name).file_size > MAX_ENTRY_SIZE):
                raise RuntimeError(f"invalid MSYS2 artifact file: {name}")
            data = zip_file.read(name)
            if digest(data) != expected:
                raise RuntimeError(f"MSYS2 artifact SHA-256 mismatch: {name}")
            content[name] = data
    if destination.exists():
        raise RuntimeError(f"MSYS2 destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="msys2-install-", dir=destination.parent) as temporary:
        stage = pathlib.Path(temporary) / "tools"
        for name, data in content.items():
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        stage.replace(destination)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=pathlib.Path, default=LOCK)
    parser.add_argument("--cache", type=pathlib.Path)
    parser.add_argument("--archive", type=pathlib.Path, required=True)
    parser.add_argument("--destination", type=pathlib.Path)
    parser.add_argument("--require-prefetched", action="store_true")
    parser.add_argument("--extract-artifact", action="store_true")
    args = parser.parse_args()
    if ((args.extract_artifact and not args.destination)
            or (not args.extract_artifact and not args.cache)):
        parser.error("--destination is required for extraction; --cache is required for materialization")
    try:
        if args.extract_artifact:
            extract_artifact(args.archive, args.destination, args.lock)
        else:
            materialize(args.cache, args.archive, args.lock,
                        require_prefetched=args.require_prefetched)
    except (OSError, ValueError, KeyError, RuntimeError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"MSYS2 tools materialization failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
