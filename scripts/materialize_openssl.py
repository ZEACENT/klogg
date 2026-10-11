"""Verify pinned OpenSSL input, recover its temporary transport, or stage two DLLs.

Historical Actions provenance authenticates transport, not vendor signatures or
current-source qualification. Verification and materialization are offline.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
import zipfile

from ci_actions_transport import TransportError, download_artifact_zip, metadata
from pe_identity import architecture_from_pe

MAX_ARCHIVE_BYTES = 32 * 1024 * 1024
MAX_OUTER_BYTES = 32 * 1024 * 1024
DOWNLOAD_TIMEOUT = 900
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
TRANSPORT_KEYS = {
    "repository", "repository_id", "workflow", "event", "branch", "run_id", "attempt",
    "head_sha", "producer_id", "producer_name", "producer_started_at", "producer_completed_at",
    "upload_started_at", "upload_completed_at", "artifact_id", "artifact_name",
    "artifact_created_at", "artifact_expires_at", "artifact_metadata_size", "artifact_digest",
}


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


def _timestamp(value):
    require(type(value) is str and re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value)
            is not None, "invalid provenance timestamp")
    try:
        return datetime.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=datetime.timezone.utc)
    except ValueError as error:
        raise OpenSslError("invalid provenance timestamp") from error


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
    require(type(lock["schema_version"]) is int and lock["schema_version"] == 1,
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
    for key in ("repository_id", "run_id", "attempt", "producer_id", "artifact_id",
                "artifact_metadata_size"):
        _positive(origin[key], 2 ** 63 - 1, key)
    for key in TRANSPORT_KEYS - {"repository_id", "run_id", "attempt", "producer_id",
                                 "artifact_id", "artifact_metadata_size"}:
        require(type(origin[key]) is str and bool(origin[key]), "invalid lock transport " + key)
    require(origin["repository"] == "ZEACENT/klogg"
            and origin["workflow"] == ".github/workflows/ci-build.yml"
            and origin["event"] == "pull_request" and origin["attempt"] == 1
            and origin["producer_name"] == "Prefetch Windows Qt 5 OpenSSL"
            and origin["artifact_name"] == "openssl-archive"
            and re.fullmatch(r"[A-Za-z0-9._/-]+", origin["branch"]) is not None
            and re.fullmatch(r"[0-9a-f]{40}", origin["head_sha"]) is not None
            and re.fullmatch(r"sha256:[0-9a-f]{64}", origin["artifact_digest"]) is not None,
            "invalid lock transport identity")
    require(_timestamp(origin["producer_started_at"]) <= _timestamp(origin["upload_started_at"])
            <= _timestamp(origin["artifact_created_at"]) <= _timestamp(origin["upload_completed_at"])
            <= _timestamp(origin["producer_completed_at"]) < _timestamp(origin["artifact_expires_at"]),
            "invalid lock transport time window")
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


def authenticate_artifact(lock, now=None):
    """Authenticate the original successful producer, not the latest run conclusion."""
    origin = lock["transport"]
    now = now or datetime.datetime.now(datetime.timezone.utc)
    require(now.tzinfo is not None and now < _timestamp(origin["artifact_expires_at"]),
            "pinned OpenSSL transport has expired; verified offline bytes remain usable")
    prefix = "repos/" + origin["repository"] + "/actions/"
    run_path = prefix + "runs/" + str(origin["run_id"])
    artifact = metadata(prefix + "artifacts/" + str(origin["artifact_id"]))
    run = metadata(run_path)
    attempt = metadata(run_path + "/attempts/" + str(origin["attempt"]))
    listing = metadata(run_path + "/attempts/{}/jobs?per_page=100".format(origin["attempt"]))
    producer = metadata(prefix + "jobs/" + str(origin["producer_id"]))
    _equal(artifact, {"id": origin["artifact_id"], "name": origin["artifact_name"], "expired": False,
                      "size_in_bytes": origin["artifact_metadata_size"], "digest": origin["artifact_digest"],
                      "created_at": origin["artifact_created_at"], "expires_at": origin["artifact_expires_at"]},
           "artifact metadata")
    _equal(artifact.get("workflow_run"), {"id": origin["run_id"], "repository_id": origin["repository_id"],
           "head_repository_id": origin["repository_id"], "head_branch": origin["branch"],
           "head_sha": origin["head_sha"]}, "artifact run")
    run_identity = {"id": origin["run_id"], "head_sha": origin["head_sha"], "head_branch": origin["branch"],
                    "event": origin["event"], "path": origin["workflow"]}
    for label, response in (("run", run), ("explicit attempt", attempt)):
        _equal(response, run_identity, label)
        for field in ("repository", "head_repository"):
            _equal(response.get(field), {"id": origin["repository_id"], "full_name": origin["repository"]},
                   label + " " + field)
    require(type(run.get("run_attempt")) is int and run["run_attempt"] >= origin["attempt"],
            "substituted run attempt")
    _equal(attempt, {"run_attempt": origin["attempt"]}, "explicit attempt")
    require(type(listing) is dict and type(listing.get("jobs")) is list
            and type(listing.get("total_count")) is int
            and listing["total_count"] == len(listing["jobs"]) <= 100
            and all(type(job) is dict and type(job.get("id")) is int for job in listing["jobs"])
            and len({job["id"] for job in listing["jobs"]}) == len(listing["jobs"]),
            "producer job list is incomplete or malformed")
    matches = [job for job in listing["jobs"] if job.get("name") == origin["producer_name"]]
    require(len(matches) == 1, "original producer job is missing or ambiguous")
    job_identity = {"id": origin["producer_id"], "name": origin["producer_name"],
                    "run_id": origin["run_id"], "run_attempt": origin["attempt"],
                    "head_sha": origin["head_sha"], "status": "completed", "conclusion": "success",
                    "started_at": origin["producer_started_at"], "completed_at": origin["producer_completed_at"]}
    for job in (matches[0], producer):
        _equal(job, job_identity, "original producer job")
        steps = job.get("steps")
        require(type(steps) is list and all(type(step) is dict for step in steps), "producer upload steps missing")
        uploads = [step for step in steps if type(step.get("name")) is str
                   and re.fullmatch(r"Run actions/upload-artifact@[0-9a-f]{40}", step["name"]) is not None]
        require(len(uploads) == 1, "original producer upload is missing or ambiguous")
        _equal(uploads[0], {"status": "completed", "conclusion": "success",
               "started_at": origin["upload_started_at"], "completed_at": origin["upload_completed_at"]},
               "producer upload")
    require(_timestamp(attempt.get("run_started_at")) <= _timestamp(origin["producer_started_at"]),
            "producer is outside original attempt time window")
    return artifact


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
        authenticate_artifact(lock)
        with tempfile.TemporaryDirectory(prefix=".klogg-openssl-acquire-", dir=str(archive.parent)) as scratch:
            outer = pathlib.Path(scratch) / "actions.zip"
            download_artifact_zip(lock["transport"]["artifact_id"], outer,
                                  limit=MAX_OUTER_BYTES, timeout=DOWNLOAD_TIMEOUT)
            raw_outer = _regular_bytes(outer, MAX_OUTER_BYTES)
            require(hashlib.sha256(raw_outer).hexdigest() == lock["transport"]["artifact_digest"][7:],
                    "outer artifact SHA-256 mismatch")
            with zipfile.ZipFile(io.BytesIO(raw_outer)) as wrapper:
                infos = _inventory(wrapper, {"entries": 1, "member_bytes": lock["archive"]["size_bytes"],
                                             "expanded_bytes": lock["archive"]["size_bytes"]})
                require(len(infos) == 1 and infos[0].filename == "openssl.zip" and not infos[0].is_dir(),
                        "unexpected outer artifact inventory")
                raw = _member_bytes(wrapper, infos[0], lock["archive"]["size_bytes"])
            _verify_bytes(raw, lock)
            staged = pathlib.Path(scratch) / "openssl.zip"
            with staged.open("xb") as output:
                output.write(raw)
            try:
                os.link(str(staged), str(archive))
            except FileExistsError as error:
                raise OpenSslError("archive destination already exists; refusing overwrite") from error
    except (TransportError, OSError, zipfile.BadZipFile, RuntimeError) as error:
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
