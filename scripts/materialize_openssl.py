"""Verify pinned OpenSSL input, acquire its release asset, or stage two DLLs.

Release identity authenticates transport, not vendor signatures or current-source
qualification. Verification and materialization are offline.
"""
import argparse
import hashlib
import io
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
import time
import zipfile

from ci_actions_transport import TransportError, metadata
from pe_identity import architecture_from_pe
from prefetch_adb_helper_sources import download as download_source

MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
DOWNLOAD_BUDGET_SECONDS = 900
CHUNK_BYTES = 1024 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
RUNTIME = {
    "x86": ("openssl-1.1/x86/bin/libcrypto-1_1.dll", "openssl-1.1/x86/bin/libssl-1_1.dll"),
    "x64": ("openssl-1.1/x64/bin/libcrypto-1_1-x64.dll", "openssl-1.1/x64/bin/libssl-1_1-x64.dll"),
}
VERSION = {"vendor": "OpenSSL 1.1.1w", "version_txt": "OpenSSL_1_1_1w-0-ge04bd3433f\n",
           "number": "0x1010117fL", "text": "OpenSSL 1.1.1w  11 Sep 2023"}
LIMITS = {"entries": 261, "expanded_bytes": 64 * 1024 * 1024,
          "member_bytes": 16 * 1024 * 1024, "marker_bytes": 64 * 1024}
REPOSITORY = "ZEACENT/klogg"
RELEASE_TAG = "openssl-qt5-1.1.1w-input-v1"
TRANSPORT_KEYS = {"repository", "repository_id", "release_id", "tag", "asset_id", "asset_name", "url"}


class OpenSslError(RuntimeError):
    """Integrity, provenance, or ownership failure; never a retryable cache miss."""


def require(condition, message):
    if not condition:
        raise OpenSslError(message)


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate lock JSON key: " + key)
        result[key] = value
    return result


def _nonfinite(value):
    raise OpenSslError("non-finite lock JSON number: " + value)


def _object(value, keys, label):
    require(type(value) is dict and set(value) == set(keys), "invalid lock " + label + " schema")


def _positive(value, ceiling, label):
    require(type(value) is int and 0 < value <= ceiling, "invalid lock " + label)


def _regular_bytes(path, limit):
    """Read a bounded snapshot, refusing symlinks, reparse points and special files."""
    path = pathlib.Path(path)
    try:
        before = path.lstat()
        require(stat.S_ISREG(before.st_mode)
                and not getattr(before, "st_file_attributes", 0) & 0x400,
                "input is not a regular non-reparse file: " + str(path))
        require(before.st_size <= limit, "input exceeds byte limit: " + str(path))
        descriptor = os.open(str(path), os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
                             | getattr(os, "O_BINARY", 0))
        with os.fdopen(descriptor, "rb") as source:
            opened = os.fstat(source.fileno())
            require(stat.S_ISREG(opened.st_mode)
                    and not getattr(opened, "st_file_attributes", 0) & 0x400
                    and (opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino),
                    "input changed or is not a regular file")
            output = bytearray()
            for chunk in iter(lambda: source.read(min(CHUNK_BYTES, limit - len(output) + 1)), b""):
                output.extend(chunk)
                require(len(output) <= limit, "input exceeds byte limit")
            return bytes(output)
    except OSError as error:
        raise OpenSslError("cannot read input: " + str(path)) from error


def load_lock(path):
    try:
        lock = json.loads(_regular_bytes(path, 64 * 1024).decode("utf-8"),
                          object_pairs_hook=_pairs, parse_constant=_nonfinite)
    except (ValueError, UnicodeError) as error:
        raise OpenSslError("invalid lock JSON") from error
    _object(lock, ("schema_version", "archive", "version", "limits", "runtime", "transport"), "root")
    require(type(lock["schema_version"]) is int and lock["schema_version"] == 2,
            "invalid lock schema version")
    archive = lock["archive"]
    _object(archive, ("file_name", "size_bytes", "sha256"), "archive")
    require(archive["file_name"] == "openssl.zip", "invalid lock archive filename")
    _positive(archive["size_bytes"], MAX_ARCHIVE_BYTES, "archive size")
    require(type(archive["sha256"]) is str and SHA256.fullmatch(archive["sha256"]) is not None,
            "invalid lock archive digest")
    _object(lock["version"], VERSION, "version")
    require(lock["version"] == VERSION, "unsupported OpenSSL version markers")
    _object(lock["limits"], LIMITS, "limits")
    for key, maximum in LIMITS.items():
        _positive(lock["limits"][key], maximum, key + " limit")
    _object(lock["runtime"], RUNTIME, "runtime")
    for arch, names in RUNTIME.items():
        _object(lock["runtime"][arch], names, arch + " runtime")
        for identity in lock["runtime"][arch].values():
            _object(identity, ("size_bytes", "sha256"), "DLL identity")
            _positive(identity["size_bytes"], LIMITS["member_bytes"], "DLL size")
            require(type(identity["sha256"]) is str and SHA256.fullmatch(identity["sha256"])
                    is not None, "invalid lock DLL digest")
    origin = lock["transport"]
    _object(origin, TRANSPORT_KEYS, "transport")
    for key in ("repository_id", "release_id", "asset_id"):
        _positive(origin[key], 2 ** 63 - 1, key)
    require(origin["repository"] == REPOSITORY and origin["tag"] == RELEASE_TAG
            and origin["asset_name"] == archive["file_name"]
            and origin["url"] == "https://github.com/" + REPOSITORY + "/releases/download/"
            + RELEASE_TAG + "/" + archive["file_name"], "invalid lock transport identity")
    return lock


def _safe_name(name, root=None):
    require(type(name) is str and name and "\\" not in name and "\0" not in name
            and not name.startswith("/") and len(name) <= 1024, "unsafe ZIP path")
    parts = name.rstrip("/").split("/")
    require(all(part not in ("", ".", "..") and not part.endswith((".", " "))
                and not any(ord(char) < 32 or ord(char) > 126 or char in '<>:"|?*' for char in part)
                and not re.fullmatch(r"(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])",
                                     part.split(".")[0].rstrip(" .")) for part in parts),
            "unsafe Windows ZIP path")
    require(root is None or parts[0] == root, "unexpected ZIP root")
    return "/".join(parts).casefold()


def _inventory(archive, limits, root=None):
    infos = archive.infolist()
    require(0 < len(infos) <= limits["entries"], "ZIP entry count exceeds limit")
    seen = {}
    expanded = 0
    for info in infos:
        require(info.orig_filename == info.filename, "unsafe ZIP NUL path")
        canonical = _safe_name(info.filename, root)
        require(canonical not in seen, "duplicate or case-fold ZIP path")
        directory = info.is_dir()
        kind = stat.S_IFMT(info.external_attr >> 16)
        require(kind in ((0, stat.S_IFDIR) if directory else (0, stat.S_IFREG))
                and not (info.external_attr & 0x10 and not directory), "ZIP link or special entry")
        require(not info.flag_bits & 1, "encrypted ZIP entry")
        require(info.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED),
                "unsupported ZIP compression")
        require(0 <= info.file_size <= limits["member_bytes"]
                and (not directory or info.file_size == 0), "ZIP member exceeds limit")
        expanded += info.file_size
        require(expanded <= limits["expanded_bytes"], "ZIP expanded size exceeds limit")
        seen[canonical] = directory
    for name in seen:
        parents = name.split("/")[:-1]
        for index in range(1, len(parents) + 1):
            parent = "/".join(parents[:index])
            require(parent not in seen or seen[parent], "ZIP file/directory path collision")
    return infos


def _member_bytes(archive, info, ceiling):
    require(info.file_size <= ceiling, "ZIP marker or member exceeds limit")
    content = bytearray()
    with archive.open(info) as source:
        for chunk in iter(lambda: source.read(min(CHUNK_BYTES, ceiling - len(content) + 1)), b""):
            content.extend(chunk)
            require(len(content) <= ceiling, "streamed ZIP member exceeds limit")
    require(len(content) == info.file_size, "streamed ZIP member size mismatch")
    return bytes(content)


def _verify_bytes(raw, lock):
    require(len(raw) == lock["archive"]["size_bytes"], "OpenSSL archive size mismatch")
    require(hashlib.sha256(raw).hexdigest() == lock["archive"]["sha256"],
            "OpenSSL archive SHA-256 mismatch")
    runtime = {name: (arch, identity) for arch, files in lock["runtime"].items()
               for name, identity in files.items()}
    headers = {"openssl-1.1/" + arch + "/include/openssl/opensslv.h" for arch in RUNTIME}
    markers = headers | {"openssl-1.1/version.txt"}
    selected = {}
    observed_markers = {}
    try:
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            infos = _inventory(archive, lock["limits"], "openssl-1.1")
            expanded = 0
            for info in infos:
                ceiling = lock["limits"]["marker_bytes"] if info.filename in markers else lock["limits"]["member_bytes"]
                content = _member_bytes(archive, info, ceiling)
                expanded += len(content)
                require(expanded <= lock["limits"]["expanded_bytes"], "streamed ZIP expanded size exceeds limit")
                if info.filename in markers:
                    observed_markers[info.filename] = content
                if info.filename in runtime:
                    _, identity = runtime[info.filename]
                    require(len(content) == identity["size_bytes"]
                            and hashlib.sha256(content).hexdigest() == identity["sha256"],
                            "OpenSSL DLL size or SHA-256 mismatch: " + info.filename)
                    selected[info.filename] = content
        require(set(selected) == set(runtime) and set(observed_markers) == markers,
                "OpenSSL required runtime/version layout missing")
        require(observed_markers["openssl-1.1/version.txt"] == VERSION["version_txt"].encode("ascii"),
                "OpenSSL version.txt mismatch")
        for name in headers:
            header = observed_markers[name].decode("ascii")
            numbers = re.findall(r"^\s*#\s*define\s+OPENSSL_VERSION_NUMBER\s+(\S+)\s*$", header, re.MULTILINE)
            texts = re.findall(r'^\s*#\s*define\s+OPENSSL_VERSION_TEXT\s+"([^"\n]+)"\s*$', header, re.MULTILINE)
            require(numbers == [VERSION["number"]] and texts == [VERSION["text"]],
                    "OpenSSL header version mismatch")
        with tempfile.TemporaryDirectory(prefix="klogg-openssl-pe-") as scratch:
            for index, (name, content) in enumerate(selected.items()):
                path = pathlib.Path(scratch) / str(index)
                path.write_bytes(content)
                require(architecture_from_pe(path) == runtime[name][0], "OpenSSL DLL PE architecture mismatch")
    except (zipfile.BadZipFile, ValueError, UnicodeError, NotImplementedError, RuntimeError) as error:
        if isinstance(error, OpenSslError):
            raise
        raise OpenSslError("invalid OpenSSL ZIP, CRC, or PE identity: " + str(error)) from error
    return selected


def verify_archive(archive, lock_path):
    lock = load_lock(lock_path)
    return _verify_bytes(_regular_bytes(archive, lock["archive"]["size_bytes"]), lock)


def _equal(document, expected, label):
    require(type(document) is dict and all(type(document.get(key)) is type(value)
            and document[key] == value for key, value in expected.items()), "substituted " + label)


def authenticate_release(lock):
    """Bind the exact uploaded asset to the pinned repository and published release."""
    origin = lock["transport"]
    prefix = "repos/" + origin["repository"]
    _equal(metadata(prefix), {"id": origin["repository_id"], "full_name": origin["repository"]},
           "release repository")
    release = metadata(prefix + "/releases/" + str(origin["release_id"]))
    _equal(release, {"id": origin["release_id"], "tag_name": origin["tag"], "draft": False},
           "release identity")
    assets = release.get("assets")
    require(type(assets) is list
            and all(type(asset) is dict and type(asset.get("id")) is int and asset["id"] > 0
                    and type(asset.get("name")) is str and bool(asset["name"]) for asset in assets)
            and len({asset["id"] for asset in assets}) == len(assets)
            and len({asset["name"] for asset in assets}) == len(assets),
            "release asset list is malformed or ambiguous")
    matches = [asset for asset in assets if asset["id"] == origin["asset_id"]]
    require(len(matches) == 1, "pinned asset does not belong to release")
    expected = {"id": origin["asset_id"], "name": origin["asset_name"], "state": "uploaded",
                "size": lock["archive"]["size_bytes"], "digest": "sha256:" + lock["archive"]["sha256"],
                "browser_download_url": origin["url"]}
    _equal(matches[0], expected, "release asset association")
    asset = metadata(prefix + "/releases/assets/" + str(origin["asset_id"]))
    _equal(asset, expected, "release asset metadata")
    return asset


def _safe_parent(path):
    """Do not publish into an ancestor symlink or Windows reparse directory."""
    for parent in (path,) + tuple(path.parents):
        try:
            attributes = parent.lstat()
        except OSError as error:
            raise OpenSslError("output parent directory is unavailable") from error
        require(stat.S_ISDIR(attributes.st_mode)
                and not getattr(attributes, "st_file_attributes", 0) & 0x400,
                "output parent is not a non-reparse directory")


class _DownloadDeadline:
    """Cancellation predicate checked between reads, not a mid-read interruption."""

    def __init__(self):
        self.expires_at = time.monotonic() + DOWNLOAD_BUDGET_SECONDS

    def is_set(self):
        return time.monotonic() >= self.expires_at

    def check(self):
        require(not self.is_set(), "OpenSSL download exceeded deadline")


def acquire(archive, lock_path):
    archive = pathlib.Path(archive).absolute()
    lock = load_lock(lock_path)
    if os.path.lexists(str(archive)):
        _verify_bytes(_regular_bytes(archive, lock["archive"]["size_bytes"]), lock)
        return
    require(sys.platform.startswith("linux"), "cold acquisition requires the Ubuntu producer")
    require(bool(os.environ.get("GH_TOKEN")), "GH_TOKEN is required only for cold acquisition")
    _safe_parent(archive.parent)
    try:
        authenticate_release(lock)
        with tempfile.TemporaryDirectory(prefix=".klogg-openssl-acquire-", dir=str(archive.parent)) as scratch:
            staged = pathlib.Path(scratch) / "openssl.zip"
            deadline = _DownloadDeadline()
            try:
                download_source(lock["transport"]["url"], staged, max_bytes=lock["archive"]["size_bytes"],
                                attempts=1, socket_timeout=30, cancel_event=deadline)
            except RuntimeError:
                deadline.check()
                raise
            deadline.check()
            _verify_bytes(_regular_bytes(staged, lock["archive"]["size_bytes"]), lock)
            deadline.check()
            try:
                os.link(str(staged), str(archive))
            except FileExistsError as error:
                raise OpenSslError("archive destination already exists; refusing overwrite") from error
    except (TransportError, OSError, RuntimeError) as error:
        if isinstance(error, OpenSslError):
            raise
        raise OpenSslError("OpenSSL acquisition failed: " + str(error)) from error


def materialize(archive, lock_path, architecture, destination):
    require(architecture in RUNTIME, "unsupported runtime architecture")
    files = verify_archive(archive, lock_path)
    destination = pathlib.Path(destination).absolute()
    _safe_parent(destination.parent)
    owned = []
    def remember(path, directory, attributes):
        owned.append((path, directory, (attributes.st_dev, attributes.st_ino)))
    try:
        with tempfile.TemporaryDirectory(prefix=".klogg-openssl-runtime-", dir=str(destination.parent)) as scratch:
            staged = []
            for name in RUNTIME[architecture]:
                path = pathlib.Path(scratch) / pathlib.PurePosixPath(name).name
                with path.open("xb") as output:
                    output.write(files[name])
                require(_regular_bytes(path, len(files[name])) == files[name], "staged DLL content mismatch")
                staged.append((name, path))
            try:
                destination.mkdir()
            except FileExistsError as error:
                raise OpenSslError("runtime destination already exists; refusing overwrite") from error
            remember(destination, True, destination.lstat())
            runtime_dir = destination / architecture
            runtime_dir.mkdir()
            remember(runtime_dir, True, runtime_dir.lstat())
            binary_dir = runtime_dir / "bin"
            binary_dir.mkdir()
            remember(binary_dir, True, binary_dir.lstat())
            for name, source in staged:
                target = binary_dir / source.name
                with target.open("xb") as output:
                    remember(target, False, os.fstat(output.fileno()))
                    output.write(_regular_bytes(source, len(files[name])))
                require(hashlib.sha256(_regular_bytes(target, len(files[name]))).digest()
                        == hashlib.sha256(files[name]).digest(), "materialized DLL hash mismatch")
    except BaseException:
        # Cancellation has the same owned-only cleanup obligation as I/O failure.
        for path, directory, identity in reversed(owned):
            try:
                current = path.lstat()
                same_kind = stat.S_ISDIR(current.st_mode) if directory else stat.S_ISREG(current.st_mode)
                if ((current.st_dev, current.st_ino) == identity and same_kind
                        and not getattr(current, "st_file_attributes", 0) & 0x400):
                    path.rmdir() if directory else path.unlink()
            except OSError:
                pass  # Never remove an unexpected file or another actor's output.
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--archive", required=True, type=pathlib.Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--verify-only", action="store_true")
    mode.add_argument("--acquire", action="store_true")
    mode.add_argument("--materialize", action="store_true")
    parser.add_argument("--architecture", choices=tuple(RUNTIME))
    parser.add_argument("--destination", type=pathlib.Path)
    args = parser.parse_args(argv)
    if args.materialize:
        if args.architecture is None or args.destination is None:
            parser.error("--materialize requires --architecture and --destination")
    elif args.architecture is not None or args.destination is not None:
        parser.error("--architecture and --destination require --materialize")
    try:
        if args.verify_only:
            verify_archive(args.archive, args.lock)
        elif args.acquire:
            acquire(args.archive, args.lock)
        else:
            materialize(args.archive, args.lock, args.architecture, args.destination)
    except (OpenSslError, OSError) as error:
        print("OpenSSL input rejected: " + str(error), file=sys.stderr)
        return 1
    print("OpenSSL pinned input verified" + (" and runtime materialized" if args.materialize else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
