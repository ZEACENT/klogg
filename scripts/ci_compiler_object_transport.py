#!/usr/bin/env python3
"""Shared trusted-code compiler objects; never native qualification or source proof.

Canonical same-repository PRs and master explicitly share compiler-cache data.
The compiler cache still checks every compile's source, headers and flags. A
successful producing lane and its reviewed steps are required, not success of
unrelated lanes. All consuming builds, links, tests and package checks still run.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from urllib.parse import quote
import zipfile

# Keep -I execution isolated from CWD while admitting this script's own trusted
# sibling modules. Runtime input paths never participate in module resolution.
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import ci_actions_transport as actions
from ci_environment import _parse_json

REPOSITORY = actions.REPOSITORY
WORKFLOW = ".github/workflows/ci-build.yml"
LOCAL_REUSABLE_WORKFLOWS = (
    ".github/workflows/ci-ios-host-evidence.yml",
    ".github/workflows/ci-environments.yml",
    ".github/workflows/ci-dependencies.yml",
)
POLICY_FILES = (
    WORKFLOW,
    "scripts/ci_compiler_object_transport.py",
    "scripts/ci_actions_transport.py",
    "scripts/ci_environment.py",
    ".github/actions/restore-compiler-object-artifact/action.yml",
    ".github/actions/export-compiler-object-artifact/action.yml",
    ".github/actions/save-verified-compiler-cache/action.yml",
)
# Match the largest existing Windows backend allowance, not a new cache budget.
MAX_BYTES = 2 * 1024 * 1024 * 1024
MAX_MEMBERS = 100_000
MAX_MANIFEST = 1_048_576
BACKEND_MARKER = ".klogg-compiler-backend.json"
MAX_BACKEND_MARKER = 4096
MAX_PAGES = 3
MAX_CANDIDATES = 12
MAX_NEWER_ATTEMPTS = 3
MAX_SECONDS = 300
RETENTION_DAYS = 2
REFRESH_SECONDS = 86400
TERMINAL_CONCLUSIONS = ("success", "failure", "cancelled")
ARTIFACT_IDENTITY_FIELDS = ("id", "name", "expired", "created_at", "workflow_run", "digest", "size_in_bytes")
SHA = re.compile(r"[0-9a-f]{40}\Z")
SAFE = re.compile(r"[A-Za-z0-9_.-]+\Z")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def fingerprint(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def artifact_identity(metadata):
    # Compare every admission field, preserving missing/null and numeric types.
    # updated_at and response URLs are not provenance or download inputs.
    return canonical({key: metadata[key] for key in ARTIFACT_IDENTITY_FIELDS if key in metadata})


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def timestamp(value):
    require(isinstance(value, str) and value.endswith("Z"), "missing UTC Actions timestamp")
    try:
        return datetime.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
    except ValueError as error:
        raise ValueError("invalid Actions timestamp") from error


def document(path):
    path = Path(path)
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= MAX_MANIFEST,
            "missing or oversized compiler cache descriptor")
    result = _parse_json(path.read_bytes(), "compiler cache document")
    require(isinstance(result, dict), "compiler cache document must be an object")
    return result


def safe_name(name):
    require(isinstance(name, str) and 0 < len(name) <= 240, "unsafe object member name")
    parts = name.split("/")
    require(all(SAFE.fullmatch(part) and part not in (".", "..")
                and not part.endswith((".", " "))
                and part.split(".", 1)[0].upper() not in
                {"CON", "PRN", "AUX", "NUL", *(f"COM{x}" for x in range(1, 10)),
                 *(f"LPT{x}" for x in range(1, 10))}
                for part in parts), "unsafe object member path")
    require(all(part.lower() not in ("ccache.conf", BACKEND_MARKER) for part in parts),
            "cache configuration or backend hint is not object data")
    return parts


def policy_bytes(data):
    """Only UTF-8 CRLF checkout conversion is equivalent to Git's LF policy.

    Never invoke Git clean/smudge filters or normalize other source content.
    Both the policy digest and authenticated Git blob proof use these bytes;
    any non-newline runtime-code drift remains a different policy identity.
    """
    try:
        text = data.decode("utf-8").replace("\r\n", "\n")
    except UnicodeError as error:
        raise ValueError("compiler cache policy is not UTF-8 text") from error
    require("\r" not in text and "\x00" not in text, "unsupported compiler cache policy text encoding")
    return text.encode("utf-8")


def local_workflow_calls(data):
    """Recognize reviewed literal job-level local calls, never shell text/URLs.

    GitHub resolves these calls at the caller's commit. Their authenticated
    attempt metadata can therefore identify a PR merge checkout independently
    of the artifact manifest and the API's different PR-head head_sha field.
    """
    found, in_jobs = set(), False
    for line in data.decode("utf-8").splitlines():
        if line == "jobs:":
            in_jobs = True
            continue
        if in_jobs and line and not line[0].isspace() and not line.startswith("#"):
            break
        if in_jobs:
            match = re.fullmatch(r"    uses:\s+(['\"]?)(\./[A-Za-z0-9_./-]+)\1(?:\s+#.*)?\s*", line)
            if match and match[2][2:] in LOCAL_REUSABLE_WORKFLOWS:
                found.add(match[2][2:])
    return sorted(found)


def context(descriptor, repo_root):
    fields = {"schema_version", "kind", "label", "compatibility", "policy_files",
              "producer_job", "required_steps", "export_step", "budget_bytes"}
    require(isinstance(descriptor, dict) and set(descriptor) == fields
            and type(descriptor["schema_version"]) is int and descriptor["schema_version"] == 1,
            "invalid compiler object descriptor")
    require(descriptor["kind"] in ("ccache", "sccache"), "unsupported compiler cache kind")
    require(isinstance(descriptor["label"], str) and re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,63}", descriptor["label"]),
            "invalid compiler cache label")
    require(isinstance(descriptor["compatibility"], dict) and bool(descriptor["compatibility"]),
            "missing compiler compatibility inputs")
    require(type(descriptor["budget_bytes"]) is int and 0 < descriptor["budget_bytes"] <= MAX_BYTES,
            "invalid compiler cache budget")
    for key in ("producer_job", "export_step"):
        require(isinstance(descriptor[key], str) and 0 < len(descriptor[key]) <= 200
                and not any(ord(char) < 32 for char in descriptor[key]), "invalid producer step identity")
    steps = descriptor["required_steps"]
    require(isinstance(steps, list) and 0 < len(steps) <= 20
            and all(isinstance(step, str) and 0 < len(step) <= 200 for step in steps)
            and len(set(steps)) == len(steps), "invalid required producer steps")
    extra = descriptor["policy_files"]
    require(isinstance(extra, list) and len(extra) <= 20
            and all(isinstance(path, str) for path in extra), "invalid cache policy files")
    root = Path(repo_root).resolve(strict=True)
    files, hashes, local_calls = {}, {}, []
    for name in sorted(set(POLICY_FILES) | set(extra)):
        safe_name(name)
        path = root / name
        require(path.is_file() and not path.is_symlink() and path.stat().st_size <= MAX_MANIFEST,
                "missing or unsafe compiler cache policy file")
        try:
            path.resolve(strict=True).relative_to(root)
        except ValueError as error:
            raise ValueError("cache policy escapes repository") from error
        data = policy_bytes(path.read_bytes())
        if name == WORKFLOW:
            local_calls = local_workflow_calls(data)
        hashes[name] = hashlib.sha256(data).hexdigest()
        files[name] = hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()
    policy = {key: descriptor[key] for key in ("producer_job", "required_steps", "export_step")}
    policy.update(files=hashes, local_workflows=local_calls)
    result = {key: descriptor[key] for key in fields - {"policy_files"}}
    result.update(policy_files=files, local_workflows=local_calls, policy_digest=fingerprint(policy),
                  compatibility_digest=fingerprint(descriptor["compatibility"]))
    result["key"] = fingerprint({key: result[key] for key in
                                 ("kind", "label", "policy_digest", "compatibility_digest", "budget_bytes")})
    return result


def artifact_name(ctx, attempt):
    require(type(attempt) is int and 0 < attempt <= 999999, "invalid artifact run attempt")
    return f"compiler-objects-v1-{ctx['label']}-{ctx['key']}-attempt-{attempt}"


def _git(root, *arguments):
    return subprocess.run(["git", *arguments], cwd=root, check=True, capture_output=True,
                          text=True, timeout=30).stdout.strip()


def identity(environ, repo_root, git=_git):
    require(environ.get("GITHUB_REPOSITORY") == REPOSITORY, "canonical repository required")
    payload = document(environ["GITHUB_EVENT_PATH"])
    repository = payload.get("repository")
    require(isinstance(repository, dict) and repository.get("full_name") == REPOSITORY
            and type(repository.get("id")) is int and repository["id"] > 0
            and environ.get("GITHUB_REPOSITORY_ID") == str(repository["id"]),
            "canonical repository numeric identity required")
    event, sha = environ.get("GITHUB_EVENT_NAME"), environ.get("GITHUB_SHA", "")
    require(SHA.fullmatch(sha) and sha != "0" * 40, "invalid checkout identity")
    if event == "pull_request":
        pull = payload.get("pull_request")
        require(isinstance(pull, dict) and isinstance(pull.get("head"), dict)
                and isinstance(pull["head"].get("repo"), dict), "malformed PR event")
        number, head = payload.get("number"), pull["head"]
        require(type(number) is int and number > 0 and pull.get("number") == number
                and environ.get("GITHUB_REF") == f"refs/pull/{number}/merge"
                and head["repo"].get("full_name") == REPOSITORY
                and head["repo"].get("id") == repository["id"], "forks cannot share compiler artifacts")
        head_sha = head.get("sha", "")
        require(isinstance(head_sha, str) and SHA.fullmatch(head_sha), "invalid PR head SHA")
    else:
        require(event == "push" and environ.get("GITHUB_REF") == "refs/heads/master"
                and payload.get("ref") == "refs/heads/master" and payload.get("after") == sha,
                "compiler artifacts require an ordinary same-repository PR or master push")
        head_sha = sha
    workflow = environ.get("GITHUB_WORKFLOW_REF", "")
    require(workflow.startswith(REPOSITORY + "/" + WORKFLOW + "@"), "unexpected producer workflow")
    require(git(repo_root, "rev-parse", "HEAD") == sha, "checkout differs from Actions revision")
    tree = git(repo_root, "rev-parse", "HEAD^{tree}")
    require(SHA.fullmatch(tree), "invalid compiled source tree")
    ids = [environ.get(key, "") for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT")]
    require(all(re.fullmatch(r"[1-9][0-9]{0,19}", value) for value in ids), "invalid Actions run identity")
    return {"repository_id": repository["id"], "run_id": int(ids[0]), "run_attempt": int(ids[1]),
            "event": event, "head_sha": head_sha, "checkout_sha": sha, "tree": tree}


def source_sensitive(ctx):
    return ctx["compatibility"].get("component") == "application"


def source_covered(ctx, producer_tree, current_tree):
    # Source trees guide acquisition/export freshness for mixed application
    # caches, never compatibility of dependency-only native compiler objects.
    return not source_sensitive(ctx) or (current_tree is not None and producer_tree == current_tree)


def mark_backend(cache, ctx, origin):
    """Write an acquisition hint at the caller's validated cache-save boundary.

    This is not an attestation or a claim about every cache entry. The backend
    has the existing same-trusted-code cache policy and each compile still
    validates its own inputs. Shared artifact envelopes never carry this hint.
    """
    cache = Path(cache)
    require(cache.is_dir() and not cache.is_symlink(), "backend cache directory is unsafe")
    require(all(type(origin.get(key)) is int and origin[key] > 0
                for key in ("repository_id", "run_id", "run_attempt"))
            and isinstance(origin.get("tree"), str) and SHA.fullmatch(origin["tree"]),
            "invalid backend hint origin")
    marker = cache / BACKEND_MARKER
    require(not marker.is_symlink() and (not marker.exists() or marker.is_file()), "unsafe backend hint destination")
    value = {"schema_version": 1, "kind": "ordinary-compiler-backend-hint", "repository": REPOSITORY,
             "repository_id": origin["repository_id"], "context_key": ctx["key"], "source_tree": origin["tree"],
             "run_id": origin["run_id"], "run_attempt": origin["run_attempt"]}
    raw = canonical(value)
    require(len(raw) <= MAX_BACKEND_MARKER, "backend hint exceeds metadata budget")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".compiler-context-", dir=cache, delete=False) as output:
            temporary = Path(output.name)
            output.write(raw)
        temporary.replace(marker)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return marker


def backend_matches(cache, ctx, repository_id, current_tree):
    if cache is None:
        return False
    cache = Path(cache)
    marker = cache / BACKEND_MARKER
    try:
        if (not cache.is_dir() or cache.is_symlink() or not marker.is_file() or marker.is_symlink()
                or not 0 < marker.stat().st_size <= MAX_BACKEND_MARKER):
            return False
        value = document(marker)
    except (OSError, ValueError):
        return False
    fields = {"schema_version", "kind", "repository", "repository_id", "context_key", "source_tree", "run_id", "run_attempt"}
    return (set(value) == fields and type(value["schema_version"]) is int and value["schema_version"] == 1
            and value["kind"] == "ordinary-compiler-backend-hint" and value["repository"] == REPOSITORY
            and type(value["repository_id"]) is int and value["repository_id"] == repository_id
            and value["context_key"] == ctx["key"] and isinstance(value["source_tree"], str)
            and SHA.fullmatch(value["source_tree"]) is not None
            and all(type(value[key]) is int and value[key] > 0 for key in ("run_id", "run_attempt"))
            and source_covered(ctx, value["source_tree"], current_tree))


class ExportCapacityExhausted(ValueError):
    """A validated private ccache snapshot still cannot meet the unchanged cap."""

    def __init__(self, report):
        super().__init__("private compiler cache cannot satisfy bounded nonempty export after cleanup")
        self.report = dict(report)


def cache_inventory(cache_root, kind):
    cache_root = Path(cache_root)
    require(cache_root.is_dir() and not cache_root.is_symlink(), "compiler cache is not an ordinary directory")
    files, seen, paths = [], set(), {}
    report = {"source_bytes": 0, "source_file_count": 0, "persistent_bytes": 0,
              "excluded_transient_bytes": 0, "excluded_metadata_bytes": 0}
    def scan_error(error):
        raise error
    for directory, dirs, names in os.walk(cache_root, followlinks=False, onerror=scan_error):
        for name in dirs + names:
            require(not (Path(directory) / name).is_symlink(), "compiler cache contains a symlink")
        for name in dirs:
            safe_name((Path(directory) / name).relative_to(cache_root).as_posix())
        for name in sorted(names):
            path = Path(directory) / name
            observed = path.lstat()
            require(stat.S_ISREG(observed.st_mode), "special compiler object")
            report["source_file_count"] += 1
            require(report["source_file_count"] <= MAX_MEMBERS, "compiler cache exceeds inventory member limit")
            report["source_bytes"] += observed.st_size
            relative = path.relative_to(cache_root).as_posix()
            if name.lower() in ("ccache.conf", BACKEND_MARKER):
                if path.parent != cache_root:
                    safe_name(path.parent.relative_to(cache_root).as_posix())
                report["excluded_metadata_bytes"] += observed.st_size
                continue
            parts = track_name(relative, seen, paths)
            # ccache's documented default temporary_dir is cache_dir/tmp. Its
            # inode hash accelerator also lives there (ccache 4.8/4.13 InodeCache).
            # It is not persistent compiler output. Validate it before exclusion;
            # do not guess result suffixes (new ccache also uses suffixless keys).
            if kind == "ccache" and parts[0] == "tmp":
                require(len(parts) > 1, "ccache temporary namespace is not a directory")
                report["excluded_transient_bytes"] += observed.st_size
                continue
            report["persistent_bytes"] += observed.st_size
            files.append((relative, path, observed))
    return files, report


def same_inventory_file(observed, expected):
    fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
    return stat.S_ISREG(observed.st_mode) and all(getattr(observed, key) == getattr(expected, key) for key in fields)


def copy_cache_inventory(files, snapshot):
    snapshot.mkdir()
    for relative, source, expected in files:
        destination = snapshot.joinpath(*relative.split("/"))
        destination.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(source, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(descriptor, "rb") as stream, destination.open("xb") as target:
            before = os.fstat(stream.fileno())
            require(same_inventory_file(before, expected), "compiler cache changed during snapshot copy")
            remaining = expected.st_size
            while remaining:
                chunk = stream.read(min(remaining, 1024 * 1024))
                require(chunk, "compiler cache truncated during snapshot copy")
                target.write(chunk)
                remaining -= len(chunk)
            after = os.fstat(stream.fileno())
            require(not stream.read(1) and same_inventory_file(after, expected)
                    and same_inventory_file(source.lstat(), expected), "compiler cache changed during snapshot copy")
        destination.chmod(0o600)
        os.utime(destination, ns=(before.st_atime_ns, expected.st_mtime_ns))


def cleanup_ccache_snapshot(snapshot, target_bytes, ctx):
    """Use native LRU only on a real private copy, never replayed configuration."""
    require(0 < target_bytes < ctx["budget_bytes"], "invalid private cleanup headroom")
    size = f"{target_bytes // 1000}.{target_bytes % 1000:03d}k"
    private = snapshot.parent.resolve(strict=True)
    config = private / "trusted-ccache.conf"
    config.write_text("max_size = " + size + "\n", encoding="utf-8")
    (private / "runtime").mkdir()
    environment = {key: value for key, value in os.environ.items() if not key.startswith("CCACHE_")}
    compatibility = ctx.get("compatibility", {})
    if compatibility.get("platform") == "Linux":
        image = compatibility.get("toolchain", {}).get("image")
        require(isinstance(image, str) and re.fullmatch(r"(?:[A-Za-z0-9./:_-]+@)?sha256:[0-9a-f]{64}", image),
                "private cleanup requires the verified immutable Linux image")
        require("," not in str(private), "unsafe private cache mount path")
        command = ["docker", "run", "--pull=never", "--rm", "--network=none",
                   "--user", f"{os.getuid()}:{os.getgid()}",
                   "--mount", f"type=bind,source={private},target=/compiler-export",
                   "--entrypoint", "/usr/bin/env", image, "-i",
                   "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                   "HOME=/compiler-export", "CCACHE_DIR=/compiler-export/cache",
                   "CCACHE_CONFIGPATH=/compiler-export/trusted-ccache.conf",
                   "CCACHE_TEMPDIR=/compiler-export/runtime", "ccache", "--cleanup"]
    else:
        executable = shutil.which("ccache")
        require(executable is not None, "native ccache cleanup executable unavailable")
        environment.update(CCACHE_DIR=str(snapshot.resolve(strict=True)), CCACHE_CONFIGPATH=str(config),
                           CCACHE_TEMPDIR=str(private / "runtime"))
        command = [executable, "--cleanup"]
    subprocess.run(command, env=environment, check=True, capture_output=True, text=True, timeout=60)


def pack(cache_root, output, ctx, origin, *, report=None):
    cache_root, output = Path(cache_root), Path(output)
    require(not output.exists() and not output.is_symlink(), "export destination already exists")
    require(cache_root.resolve() not in output.resolve().parents, "export destination overlaps compiler cache")
    files, inventory = cache_inventory(cache_root, ctx["kind"])
    require(files and inventory["persistent_bytes"] > 0, "compiler cache has no object data")
    report = {} if report is None else report
    report.update(inventory, budget_bytes=ctx["budget_bytes"], status="packed", copy_wall_ms=0, cleanup_wall_ms=0)
    size = inventory["persistent_bytes"]
    overflow = size > ctx["budget_bytes"]
    if overflow:
        require(ctx["kind"] == "ccache", "compiler cache exceeds export budget")
        allowance = max(1_000_000, ctx["budget_bytes"] // 100)
        require(size <= ctx["budget_bytes"] + allowance, "compiler cache exceeds validated backend allowance")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".compiler-export-", dir=output.parent) as temporary:
        if overflow:
            private = Path(temporary) / "private"
            private.mkdir()
            snapshot = private / "cache"
            started = time.monotonic()
            copy_cache_inventory(files, snapshot)
            report["copy_wall_ms"] = round((time.monotonic() - started) * 1000)
            headroom = min(allowance, max(1, ctx["budget_bytes"] // 2))
            target = ctx["budget_bytes"] - headroom
            report["scratch_target_bytes"] = target
            started = time.monotonic()
            cleanup_ccache_snapshot(snapshot, target, ctx)
            report["cleanup_wall_ms"] = round((time.monotonic() - started) * 1000)
            files, cleaned = cache_inventory(snapshot, "ccache")
            size = cleaned["persistent_bytes"]
            report.update(status="compacted", retained_bytes=size, retained_file_count=len(files))
            if not files or size == 0 or size > ctx["budget_bytes"]:
                report.update(status="capacity-exhausted",
                              reason="empty-after-cleanup" if size == 0 else "oversized-after-cleanup")
                raise ExportCapacityExhausted(report)
        staging = Path(temporary) / "bundle"
        staging.mkdir()
        archive_path = staging / "objects.tar"
        with tarfile.open(archive_path, "w", format=tarfile.USTAR_FORMAT) as archive:
            for relative, path, expected in sorted(files):
                info = tarfile.TarInfo(relative)
                info.size, info.mode, info.mtime = expected.st_size, 0o600, 0
                if ctx["kind"] == "sccache":
                    # Preserve the Windows path: CPython 3.12 lstat/fstat ctime
                    # mean CreationTime/ChangeTime respectively, not one identity.
                    with path.open("rb") as stream:
                        archive.addfile(info, stream)
                else:
                    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
                    with os.fdopen(descriptor, "rb") as stream:
                        require(same_inventory_file(os.fstat(stream.fileno()), expected), "compiler cache changed during packing")
                        archive.addfile(info, stream)
                        require(not stream.read(1) and same_inventory_file(os.fstat(stream.fileno()), expected)
                                and same_inventory_file(path.lstat(), expected), "compiler cache changed during packing")
        require(archive_path.stat().st_size <= archive_limit(ctx), "object envelope exceeds export budget")
        manifest = {"schema_version": 1, "kind": "ordinary-compiler-objects", "context": ctx,
                    "origin": origin, "archive_sha256": digest(archive_path),
                    "object_count": len(files), "object_bytes": size}
        data = canonical(manifest)
        require(len(data) <= MAX_MANIFEST, "compiler object manifest is oversized")
        (staging / "manifest.json").write_bytes(data)
        report.update(retained_bytes=size, retained_file_count=len(files), archive_bytes=archive_path.stat().st_size)
        staging.rename(output)
    return manifest


def archive_limit(ctx):
    return ctx["budget_bytes"] + MAX_MEMBERS * 1024 + MAX_MANIFEST


def track_name(name, files, paths):
    parts = safe_name(name)
    require(name.casefold() not in paths, "duplicate compiler object or directory alias")
    for index in range(1, len(parts) + 1):
        component = "/".join(parts[:index])
        folded = component.casefold()
        require(folded not in files and paths.get(folded, component) == component,
                "compiler object parent is a file or case alias")
        paths[folded] = component
    files.add(name.casefold())
    return parts


def extract_objects(archive_path, output, ctx):
    output = Path(output)
    require(not output.exists() and not output.is_symlink(), "object destination already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".compiler-objects-", dir=output.parent) as temporary:
        staging = Path(temporary) / "objects"
        staging.mkdir()
        count, total, seen, paths = 0, 0, set(), {}
        with tarfile.open(archive_path, "r:") as archive:
            for member in archive:
                parts = track_name(member.name, seen, paths)
                require(member.isfile() and not member.pax_headers and member.mode == 0o600
                        and member.size >= 0,
                        "unsafe or duplicate compiler object member")
                count += 1
                total += member.size
                require(count <= MAX_MEMBERS and total <= ctx["budget_bytes"], "object extraction exceeds budget")
                path = staging.joinpath(*parts)
                path.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                require(source is not None, "missing compiler object stream")
                with source, path.open("xb") as target:
                    remaining = member.size
                    while remaining:
                        chunk = source.read(min(1024 * 1024, remaining))
                        require(chunk, "truncated compiler object")
                        target.write(chunk)
                        remaining -= len(chunk)
                path.chmod(0o600)
        require(count > 0, "empty compiler object archive")
        staging.rename(output)
    return count, total


def _attempt_job(prefix, attempt_number, ctx, get):
    listing = get(prefix + f"/attempts/{attempt_number}/jobs?per_page=100")
    jobs = listing.get("jobs")
    require(isinstance(jobs, list) and type(listing.get("total_count")) is int
            and listing["total_count"] == len(jobs) <= 100, "incomplete producer job listing")
    matching = [job for job in jobs if isinstance(job, dict) and job.get("name") == ctx["producer_job"]]
    require(len(matching) <= 1, "ambiguous compiler artifact producer job")
    return matching[0] if matching else None


def _successful_job(job, run_id, attempt_number, sha, ctx):
    require(type(job.get("id")) is int and job["id"] > 0 and job.get("run_id") == run_id
            and job.get("run_attempt") == attempt_number and job.get("head_sha") == sha,
            "compiler artifact job provenance mismatch")
    if job.get("status") != "completed" or job.get("conclusion") != "success":
        return False
    steps = job.get("steps")
    require(isinstance(steps, list), "missing producer gated steps")
    for name in set(ctx["required_steps"] + [ctx["export_step"]]):
        matches = [step for step in steps if isinstance(step, dict) and step.get("name") == name]
        require(len(matches) <= 1, "ambiguous producer gated step")
        if not matches or matches[0].get("status") != "completed" or matches[0].get("conclusion") != "success":
            return False
    return True


def _checkout_proof(run, attempt, ctx, get):
    head = run["head_sha"]
    if run["event"] == "push":
        checkout, kind = head, "actions-push-head"
    else:
        references = attempt.get("referenced_workflows")
        # A latest-run response cannot prove a different, older attempt. The
        # same-attempt response is an equivalent authenticated fallback only.
        if references is None and run["run_attempt"] == attempt["run_attempt"]:
            references = run.get("referenced_workflows")
        if not isinstance(references, list):
            return None
        known = {REPOSITORY + "/" + name for name in ctx["local_workflows"]}
        observed = set()
        for item in references:
            if not isinstance(item, dict) or not isinstance(item.get("path"), str):
                continue
            path, separator, revision = item["path"].partition("@")
            if path not in known:
                continue
            sha, ref = item.get("sha"), item.get("ref")
            if (not separator or not isinstance(sha, str) or not SHA.fullmatch(sha)
                    or sha == "0" * 40 or revision != sha or not isinstance(ref, str)
                    or not re.fullmatch(r"refs/pull/[1-9][0-9]*/merge", ref)):
                return None
            observed.add((sha, ref))
        if not observed:
            return None
        require(len(observed) == 1, "ambiguous authenticated checkout provenance")
        checkout, _ = next(iter(observed))
        kind = "actions-local-reusable-workflow"
    commit = get(f"repos/{REPOSITORY}/git/commits/{checkout}")
    tree, parents = commit.get("tree"), commit.get("parents")
    require(commit.get("sha") == checkout and isinstance(tree, dict)
            and isinstance(tree.get("sha"), str) and SHA.fullmatch(tree["sha"])
            and tree["sha"] != "0" * 40 and isinstance(parents, list),
            "invalid authenticated checkout commit/tree")
    if run["event"] == "pull_request":
        require(len(parents) == 2 and all(isinstance(parent, dict)
                and isinstance(parent.get("sha"), str) and SHA.fullmatch(parent["sha"])
                for parent in parents) and parents[1]["sha"] == head
                and parents[0]["sha"] != head and checkout != head,
                "authenticated merge checkout does not contain the producer PR head")
    return {"checkout_sha": checkout, "tree": tree["sha"]}, kind


def _producer(artifact, attempt_number, ctx, repository_id, get):
    origin = artifact["workflow_run"]
    run_id, sha = origin["id"], origin["head_sha"]
    prefix = f"repos/{REPOSITORY}/actions/runs/{run_id}"
    run = get(prefix)
    require(type(run.get("id")) is int and run["id"] == run_id
            and run.get("head_sha") == sha and run.get("head_branch") == origin.get("head_branch")
            and run.get("path") == WORKFLOW and type(run.get("run_attempt")) is int
            and run["run_attempt"] >= attempt_number, "producer run provenance mismatch")
    for key in ("repository", "head_repository"):
        repository = run.get(key)
        require(isinstance(repository, dict) and repository.get("id") == repository_id
                and repository.get("full_name") == REPOSITORY, "foreign compiler artifact producer")
    event = run.get("event")
    if event not in ("pull_request", "push") or (event == "push" and run.get("head_branch") != "master"):
        return None
    if run.get("status") != "completed" or run.get("conclusion") not in TERMINAL_CONCLUSIONS:
        return None
    attempt = get(prefix + f"/attempts/{attempt_number}")
    require(attempt.get("id") == run_id and attempt.get("run_attempt") == attempt_number
            and attempt.get("head_sha") == sha, "producer attempt provenance mismatch")
    if attempt.get("status") != "completed" or attempt.get("conclusion") not in TERMINAL_CONCLUSIONS:
        return None
    job = _attempt_job(prefix, attempt_number, ctx, get)
    if job is None or not _successful_job(job, run_id, attempt_number, sha, ctx):
        return None
    # Retain completed lane objects when only unrelated jobs were rerun, but do
    # not hide a newer failed producer behind an older green snapshot. Search
    # newest-first for this producer, not merely the latest aggregate attempt.
    if run["run_attempt"] - attempt_number > MAX_NEWER_ATTEMPTS:
        return None
    for latest_number in range(run["run_attempt"], attempt_number, -1):
        latest = get(prefix + f"/attempts/{latest_number}")
        require(latest.get("id") == run_id and latest.get("run_attempt") == latest_number
                and latest.get("head_sha") == sha, "latest producer attempt provenance mismatch")
        if latest.get("status") != "completed" or latest.get("conclusion") not in TERMINAL_CONCLUSIONS:
            return None
        latest_job = _attempt_job(prefix, latest_number, ctx, get)
        if latest_job is not None:
            if not _successful_job(latest_job, run_id, latest_number, sha, ctx):
                return None
            break
    require(timestamp(attempt["run_started_at"]) <= timestamp(job["started_at"])
            <= timestamp(artifact["created_at"]) <= timestamp(job["completed_at"]),
            "artifact is outside successful producer job execution")
    proof = _checkout_proof(run, attempt, ctx, get)
    if proof is None:
        print(f"compiler artifact run {run_id} attempt {attempt_number} lacks authenticated checkout proof", file=sys.stderr)
        return None
    checkout, proof_kind = proof
    for name, blob in ctx["policy_files"].items():
        metadata = get(f"repos/{REPOSITORY}/contents/{quote(name, safe='/')}?ref={checkout['checkout_sha']}")
        require(metadata.get("type") == "file" and metadata.get("path") == name
                and metadata.get("sha") == blob, "producer policy differs from reviewed Git bytes")
    source = {"repository_id": repository_id, "run_id": run_id, "run_attempt": attempt_number,
              "event": event, "head_sha": sha, **checkout}
    terminal = {"aggregate_conclusion": run["conclusion"], "aggregate_run_attempt": run["run_attempt"],
                "producer_attempt_conclusion": attempt["conclusion"], "producer_job_conclusion": "success",
                "checkout_proof": proof_kind}
    return source, terminal


def _unpack(archive_path, destination, artifact, ctx, origin):
    require(archive_path.is_file() and not archive_path.is_symlink()
            and archive_path.stat().st_size == artifact["size_in_bytes"], "artifact ZIP size mismatch")
    require(digest(archive_path) == artifact["digest"][7:], "artifact ZIP digest mismatch")
    with tempfile.TemporaryDirectory(prefix=".compiler-envelope-", dir=destination.parent) as directory:
        temporary = Path(directory)
        with zipfile.ZipFile(archive_path) as archive:
            members = archive.infolist()
            require(len(members) == 2 and {item.filename for item in members} == {"manifest.json", "objects.tar"},
                    "unexpected compiler artifact envelope")
            for member in members:
                require(stat.S_IFMT(member.external_attr >> 16) in (0, stat.S_IFREG)
                        and not member.flag_bits & 1 and not member.is_dir()
                        and member.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                        and 0 < member.file_size <= (MAX_MANIFEST if member.filename == "manifest.json" else archive_limit(ctx)),
                        "unsafe or oversized compiler artifact envelope")
                with archive.open(member) as source, (temporary / member.filename).open("xb") as target:
                    copied = 0
                    while chunk := source.read(1024 * 1024):
                        copied += len(chunk)
                        require(copied <= member.file_size, "expanded compiler ZIP exceeds size")
                        target.write(chunk)
                    require(copied == member.file_size, "truncated compiler ZIP member")
        manifest = document(temporary / "manifest.json")
        require(set(manifest) == {"schema_version", "kind", "context", "origin", "archive_sha256", "object_count", "object_bytes"}
                and manifest["schema_version"] == 1 and manifest["kind"] == "ordinary-compiler-objects"
                and manifest["context"] == ctx, "compiler object manifest compatibility mismatch")
        claimed = manifest["origin"]
        require(isinstance(claimed, dict) and set(claimed) == set(origin) | {"checkout_sha", "tree"}
                and all(claimed.get(key) == value for key, value in origin.items())
                and all(isinstance(claimed.get(key), str) and SHA.fullmatch(claimed[key])
                        for key in ("checkout_sha", "tree")), "compiler object manifest producer mismatch")
        require(digest(temporary / "objects.tar") == manifest["archive_sha256"], "compiler object archive digest mismatch")
        extracted = temporary / "accepted"
        count, size = extract_objects(temporary / "objects.tar", extracted, ctx)
        require(type(manifest["object_count"]) is int and type(manifest["object_bytes"]) is int
                and count == manifest["object_count"] and size == manifest["object_bytes"],
                "compiler object inventory mismatch")
        extracted.rename(destination)
    return claimed


def download_artifact(identifier, archive, *, limit, timeout):
    if os.name != "nt":
        return actions.download_artifact_zip(identifier, archive, limit=limit, timeout=timeout)
    return download_artifact_windows(identifier, archive, limit=limit, timeout=timeout)


def download_artifact_windows(identifier, archive, *, limit, timeout):
    # Windows selectors cannot wait on anonymous subprocess pipes. A bounded
    # reader queue preserves the same byte/time bounds without buffering a ZIP.
    require(os.environ.get("GH_TOKEN"), "Actions read token is missing")
    deadline = time.monotonic() + timeout
    chunks = queue.Queue(maxsize=2)
    stop = threading.Event()
    with tempfile.TemporaryFile() as errors, Path(archive).open("xb") as output:
        process = subprocess.Popen(["gh", "api", "--allow-escape-sequences",
            f"repos/{REPOSITORY}/actions/artifacts/{identifier}/zip"], stdout=subprocess.PIPE, stderr=errors)
        def deliver(chunk):
            while not stop.is_set():
                try:
                    chunks.put(chunk, timeout=0.1)
                    return
                except queue.Full:
                    pass
        def reader():
            try:
                while not stop.is_set():
                    chunk = process.stdout.read(65536)
                    deliver(chunk)
                    if not chunk:
                        return
            except OSError as error:
                deliver(error)
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        try:
            total = 0
            while True:
                remaining = deadline - time.monotonic()
                require(remaining > 0, "compiler artifact download exceeded deadline")
                try:
                    chunk = chunks.get(timeout=remaining)
                except queue.Empty as error:
                    raise ValueError("compiler artifact download exceeded deadline") from error
                require(not isinstance(chunk, OSError), "compiler artifact reader failed")
                if not chunk:
                    break
                total += len(chunk)
                require(total <= limit, "compiler artifact download exceeds budget")
                output.write(chunk)
            require(process.wait(timeout=max(0.01, deadline - time.monotonic())) == 0 and total > 0,
                    "compiler artifact download failed")
        finally:
            stop.set()
            if process.poll() is None:
                process.kill()
            process.wait()
            thread.join(timeout=5)
            process.stdout.close()


def _discover(ctx, *, repository_id, current_run, current_tree, get, now):
    """Read and authenticate candidates without downloading or mutating a cache."""
    prefix = f"repos/{REPOSITORY}/actions/"
    name_prefix = artifact_name(ctx, 1).rsplit("-", 1)[0] + "-"
    candidates, seen, exhausted = [], {}, True
    for page in range(1, MAX_PAGES + 1):
        listing = get(prefix + f"artifacts?per_page=100&page={page}")
        artifacts = listing.get("artifacts")
        require(isinstance(artifacts, list) and len(artifacts) <= 100
                and type(listing.get("total_count")) is int and listing["total_count"] >= len(artifacts),
                "malformed compiler artifact listing")
        page_seen = set()
        for item in artifacts:
            require(isinstance(item, dict) and type(item.get("id")) is int and item["id"] > 0,
                    f"invalid artifact listing entry on page {page}")
            identifier = item["id"]
            require(identifier not in page_seen,
                    f"duplicate artifact ID within listing page {page}: {identifier}")
            page_seen.add(identifier)
            observed = artifact_identity(item)
            if identifier in seen:
                # Repository offset pages are not a snapshot: concurrent uploads
                # can shift previously seen IDs onto the next page. Deduplicate
                # only stable identity, never contradictions, and do not extend
                # page/candidate/download/deadline budgets to make up the overlap.
                require(seen[identifier] == observed,
                        f"conflicting artifact identity across listing pages: {identifier}")
                continue
            seen[identifier] = observed
            name = item.get("name")
            if not isinstance(name, str) or not name.startswith(name_prefix) or item.get("expired") is True:
                continue
            suffix = name[len(name_prefix):]
            require(re.fullmatch(r"[1-9][0-9]{0,5}", suffix) and item.get("expired") is False,
                    "malformed compatible artifact name or expiry")
            origin = item.get("workflow_run")
            require(isinstance(origin, dict) and type(origin.get("id")) is int and origin["id"] > 0
                    and isinstance(origin.get("head_sha"), str) and SHA.fullmatch(origin["head_sha"]),
                    "missing compiler artifact origin")
            if (origin["id"] == current_run or origin.get("repository_id") != repository_id
                    or origin.get("head_repository_id") != repository_id):
                continue
            age = (now - timestamp(item.get("created_at"))).total_seconds()
            if age < 0 or age > RETENTION_DAYS * 86400:
                continue
            candidates.append((timestamp(item["created_at"]), item["id"], int(suffix), item))
        if len(artifacts) < 100 or page * 100 >= listing["total_count"]:
            exhausted = False
            break
    selected = None
    for _, identifier, attempt_number, listed in sorted(candidates, key=lambda item: item[:2], reverse=True)[:MAX_CANDIDATES]:
        artifact = get(prefix + f"artifacts/{identifier}")
        require(artifact_identity(artifact) == artifact_identity(listed),
                "selected artifact metadata changed after discovery")
        require(isinstance(artifact.get("digest"), str)
                and re.fullmatch(r"sha256:[0-9a-f]{64}", artifact["digest"])
                and type(artifact.get("size_in_bytes")) is int
                and 0 < artifact["size_in_bytes"] <= archive_limit(ctx) + MAX_MANIFEST,
                "artifact has no bounded immutable digest")
        producer = _producer(artifact, attempt_number, ctx, repository_id, get)
        if producer is None:
            continue
        origin, terminal = producer
        covered = source_covered(ctx, origin["tree"], current_tree)
        if selected is None or covered:
            selected = artifact, origin, terminal
        if covered:
            break
    return selected, exhausted


def restore(ctx, destination, *, repository_id, current_run, metadata=None, download=None, now=None,
            current_tree=None, backend_cache=None):
    destination = Path(destination)
    require(not destination.exists() and not destination.is_symlink(), "restore destination already exists")
    require(type(repository_id) is int and repository_id > 0 and type(current_run) is int and current_run > 0,
            "invalid consumer Actions identity")
    require(current_tree is None or (isinstance(current_tree, str) and SHA.fullmatch(current_tree)),
            "invalid current source tree")
    retained = backend_matches(backend_cache, ctx, repository_id, current_tree)
    get_raw, download = metadata or actions.metadata, download or download_artifact
    now = now or datetime.datetime.now(datetime.timezone.utc)
    started = time.monotonic()
    def get(endpoint):
        require(time.monotonic() - started < MAX_SECONDS, "compiler artifact discovery exceeded deadline")
        result = get_raw(endpoint)
        require(isinstance(result, dict), "invalid Actions metadata")
        return result
    result = {"restored": False, "export_needed": True, "discovery_exhausted": False,
              "compatibility_key": ctx["key"], "backend_retained": retained,
              "transport": "actions-cache" if retained else "actions-artifact",
              "acquisition": "compatible-backend-retained" if retained else "artifact-miss"}
    try:
        selected, result["discovery_exhausted"] = _discover(ctx, repository_id=repository_id,
            current_run=current_run, current_tree=current_tree, get=get, now=now)
    except actions.TransportError as error:
        # The shared transport is qualification-locked. Its direct exception
        # cause identifies request timeouts without changing authenticated bytes
        # or confusing similarly worded admission/transport failures with a miss.
        if not isinstance(error.__cause__, subprocess.TimeoutExpired):
            raise
        # Ordinary compiler objects are optional. A bounded metadata timeout is
        # no permission to import an artifact or suppress a fresh verified export.
        # Keep integrity/admission failures fatal, and leave download/install
        # outside this availability boundary so partial transfers cannot be used.
        result.update(discovery_status="unavailable", discovery_error=str(error),
                      acquisition="metadata-unavailable")
        print(f"notice: optional compiler artifact metadata unavailable: {error}", file=sys.stderr)
        return result
    if selected is None:
        return result
    artifact, origin, terminal = selected
    identifier = artifact["id"]
    result["export_needed"] = (not source_covered(ctx, origin["tree"], current_tree)
                               or (now - timestamp(artifact["created_at"])).total_seconds() >= REFRESH_SECONDS)
    if retained:
        # Discovery is useful for export suppression, but it is not an import.
        # Keep artifact_id absent so build metrics retain the backend source.
        result.update(available_artifact_id=identifier, available_artifact_digest=artifact["digest"],
                      available_artifact_provenance={"producer": origin, **terminal})
        return result
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".compiler-download-", dir=destination.parent) as directory:
        archive = Path(directory) / "archive.zip"
        remaining = int(MAX_SECONDS - (time.monotonic() - started))
        require(remaining > 0, "compiler artifact transport exceeded deadline")
        download(identifier, archive, limit=archive_limit(ctx) + MAX_MANIFEST, timeout=remaining)
        require(time.monotonic() - started <= MAX_SECONDS, "compiler artifact transport exceeded deadline")
        source = _unpack(archive, destination, artifact, ctx, origin)
    result.update(restored=True, artifact_id=identifier, artifact_digest=artifact["digest"],
                  producer=source, acquisition="artifact-restored",
                  transfer_wall_ms=(time.monotonic() - started) * 1000, **terminal)
    return result


def install(accepted, cache):
    accepted, cache = Path(accepted), Path(cache)
    require(accepted.is_dir() and not accepted.is_symlink(), "accepted objects are missing")
    require(not cache.is_symlink() and (not cache.exists() or cache.is_dir()),
            "active cache destination is unsafe")
    with tempfile.TemporaryDirectory(prefix=".compiler-prior-", dir=cache.parent) as directory:
        backup = Path(directory) / "prior"
        if cache.exists():
            cache.rename(backup)
        try:
            accepted.rename(cache)
        except OSError:
            if backup.exists():
                backup.rename(cache)
            raise


def emit(values):
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with Path(path).open("a", encoding="utf-8") as output:
            for key, value in values.items():
                text = str(value).lower() if isinstance(value, bool) else str(value)
                require("\n" not in text and "\r" not in text, "unsafe action output")
                output.write(f"{key}={text}\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("restore", "pack", "mark-backend"))
    parser.add_argument("--descriptor", required=True, type=Path)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if args.command != "mark-backend" and args.output is None:
        parser.error("--output is required for restore and pack")
    try:
        ctx = context(document(args.descriptor), args.repo_root)
        origin = identity(os.environ, args.repo_root)
        if args.command == "mark-backend":
            marker = mark_backend(args.cache, ctx, origin)
            emit({"marked": True, "backend-marker": marker, "compatibility-key": ctx["key"]})
        elif args.command == "pack":
            report = {}
            try:
                pack(args.cache, args.output, ctx, origin, report=report)
            except ExportCapacityExhausted as error:
                # Only a fully validated private copy that exhausted native LRU
                # cleanup is optional. Integrity, tool and admission errors stay fatal.
                print("KLOGG_COMPILER_EXPORT_JSON=" + json.dumps(error.report, sort_keys=True))
                emit({"packed": False, "export-status": "capacity-exhausted", "compatibility-key": ctx["key"]})
                return 0
            print("KLOGG_COMPILER_EXPORT_JSON=" + json.dumps(report, sort_keys=True))
            emit({"artifact-name": artifact_name(ctx, origin["run_attempt"]), "path": args.output,
                  "packed": True, "export-status": report["status"], "compatibility-key": ctx["key"]})
        else:
            # Existing backend objects are retained on a bridge miss. On a hit,
            # install only fully authenticated data, never extract over a cache.
            parent = args.cache.parent
            parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=".compiler-restore-", dir=parent) as directory:
                accepted = Path(directory) / "accepted"
                result = restore(ctx, accepted, repository_id=origin["repository_id"], current_run=origin["run_id"],
                                 current_tree=origin["tree"], backend_cache=args.cache)
                if result["restored"]:
                    install(accepted, args.cache)
            require(not args.output.exists() and not args.output.is_symlink(), "report destination already exists")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(canonical(result))
            emit({"restored": result["restored"], "export-needed": result["export_needed"],
                  "artifact-id": result.get("artifact_id", ""), "compatibility-key": ctx["key"], "report": args.output})
            print(json.dumps(result, sort_keys=True))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, tarfile.TarError, zipfile.BadZipFile) as error:
        parser.exit(1, f"compiler object transport rejected: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
