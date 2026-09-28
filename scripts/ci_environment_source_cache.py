#!/usr/bin/env python3
"""Import only lock-verified ADB source bytes from an explicit prior CI artifact."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import selectors
import stat
import subprocess
import sys
import tempfile
import time
import zipfile

import ci_environment as core
from prefetch_adb_helper_sources import PREFETCH_MANIFEST_NAME, validated_records

REPOSITORY = "ZEACENT/klogg"
WORKFLOW = ".github/workflows/ci-build.yml"
PREFETCH_JOB = "Prefetch locked ADB helper source closure"
MAX_CACHE_BYTES = 536870912
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_DOWNLOAD_SECONDS = 900
SHA = re.compile(r"[0-9a-f]{40}\Z")
DIGEST = re.compile(r"sha256:([0-9a-f]{64})\Z")


class SourceCacheError(ValueError):
    """Prior-run source transport cannot satisfy the current lock."""


class IncompatibleSourceCacheError(SourceCacheError):
    """A valid older closure contains different archives than the current lock."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SourceCacheError(message)


def _timestamp(value: object) -> datetime.datetime:
    require(isinstance(value, str), "source artifact timestamp is missing")
    try:
        result = datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise SourceCacheError("invalid source artifact timestamp") from error
    require(result.tzinfo is not None, "source artifact timestamp lacks timezone")
    return result


def _metadata(path: str) -> dict:
    try:
        result = subprocess.run(["gh", "api", path], check=True, capture_output=True,
                                timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise SourceCacheError("authenticated Actions metadata request failed") from error
    require(len(result.stdout) <= core.MAX_METADATA_BYTES, "oversized Actions metadata")
    try:
        document = core._parse_json(result.stdout, "source cache Actions metadata")
    except ValueError as error:
        raise SourceCacheError("invalid Actions metadata") from error
    require(isinstance(document, dict), "Actions metadata must be an object")
    return document


def _git(command: list[str], *, cwd: pathlib.Path) -> str:
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True,
                                cwd=cwd, timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise SourceCacheError("source cache provenance is not an ancestor of checkout") from error
    return result.stdout


def authenticate_artifact(repo_root: pathlib.Path, source_sha: str, prior_sha: str,
                          run_id: int, run_attempt: int, artifact_id: int, *,
                          metadata=None, git_runner=None) -> dict:
    """Authenticate a prior PR artifact as transport, never as qualification."""
    require(all(type(value) is int and value > 0 for value in
                (run_id, run_attempt, artifact_id)), "invalid explicit source artifact identity")
    require(isinstance(source_sha, str) and SHA.fullmatch(source_sha) is not None
            and source_sha != "0" * 40 and isinstance(prior_sha, str)
            and SHA.fullmatch(prior_sha) is not None and prior_sha != "0" * 40,
            "invalid explicit source revision")
    repo_root = pathlib.Path(repo_root).resolve()
    git = git_runner or _git
    try:
        require(git(["git", "rev-parse", "HEAD"], cwd=repo_root).strip() == source_sha,
                "checked-out source differs from qualification head")
        git(["git", "merge-base", "--is-ancestor", prior_sha, source_sha], cwd=repo_root)
    except (OSError, subprocess.SubprocessError) as error:
        raise SourceCacheError("prior source revision is not an ancestor") from error

    get = metadata or _metadata
    prefix = "repos/" + REPOSITORY + "/actions/"
    artifact = get(prefix + "artifacts/" + str(artifact_id))
    run = get(prefix + "runs/" + str(run_id))
    attempt = get(prefix + "runs/{}/attempts/{}".format(run_id, run_attempt))
    jobs = get(prefix + "runs/{}/attempts/{}/jobs?per_page=100".format(run_id, run_attempt))
    require(all(isinstance(document, dict) for document in (artifact, run, attempt, jobs)),
            "invalid source artifact provenance response")
    match = DIGEST.fullmatch(artifact.get("digest", "")) if isinstance(artifact.get("digest"), str) else None
    require(type(artifact.get("id")) is int and artifact["id"] == artifact_id
            and artifact.get("name") == "adb-helper-source-cache"
            and artifact.get("expired") is False
            and type(artifact.get("size_in_bytes")) is int
            and 0 < artifact["size_in_bytes"] <= MAX_CACHE_BYTES
            and match is not None, "source cache artifact is missing, oversized or substituted")
    origin = artifact.get("workflow_run")
    repository = run.get("repository")
    head_repository = run.get("head_repository")
    require(isinstance(origin, dict) and isinstance(repository, dict)
            and isinstance(head_repository, dict)
            and type(repository.get("id")) is int and repository["id"] > 0
            and repository.get("full_name") == REPOSITORY
            and head_repository.get("id") == repository["id"]
            and head_repository.get("full_name") == REPOSITORY
            and origin.get("repository_id") == repository["id"]
            and origin.get("head_repository_id") == repository["id"]
            and type(origin.get("id")) is int and origin["id"] == run_id
            and origin.get("head_sha") == prior_sha
            and origin.get("head_branch") == run.get("head_branch")
            and type(run.get("id")) is int and run["id"] == run_id
            and type(run.get("run_attempt")) is int and run["run_attempt"] >= run_attempt
            and run.get("head_sha") == prior_sha
            and run.get("event") == "pull_request" and run.get("path") == WORKFLOW
            and isinstance(run.get("head_branch"), str)
            and re.fullmatch(r"[A-Za-z0-9._/-]+", run["head_branch"]) is not None,
            "source artifact is not from the explicit canonical PR run and attempt")
    current_ref = os.environ.get("GITHUB_REF")
    if current_ref is not None:
        require(current_ref == "refs/heads/" + run["head_branch"],
                "prior source artifact branch differs from current dispatch")
    require(isinstance(jobs.get("jobs"), list) and type(jobs.get("total_count")) is int
            and jobs["total_count"] == len(jobs["jobs"]) <= 100,
            "source producer job list is incomplete")
    producers = [job for job in jobs["jobs"] if isinstance(job, dict)
                 and job.get("name") == PREFETCH_JOB]
    require(len(producers) == 1 and producers[0].get("status") == "completed"
            and producers[0].get("conclusion") == "success"
            and type(producers[0].get("run_attempt")) is int
            and producers[0]["run_attempt"] == run_attempt,
            "source cache artifact lacks a successful attempt-specific prefetch job")
    require(type(attempt.get("id")) is int and attempt["id"] == run_id
            and type(attempt.get("run_attempt")) is int
            and attempt["run_attempt"] == run_attempt
            and attempt.get("head_sha") == prior_sha,
            "source artifact attempt identity differs from the selected run")
    require(_timestamp(attempt.get("run_started_at"))
            <= _timestamp(producers[0].get("started_at"))
            <= _timestamp(artifact.get("created_at"))
            <= _timestamp(producers[0].get("completed_at")),
            "source cache artifact was not created in the selected attempt's prefetch job")
    return {"digest": match.group(1), "size": artifact["size_in_bytes"],
            "run_id": run_id, "run_attempt": run_attempt,
            "artifact_id": artifact_id, "prior_sha": prior_sha}


def download_artifact_zip(artifact_id: int, archive: pathlib.Path, *,
                          limit: int, timeout: int) -> None:
    """Stream an authenticated GitHub ZIP with a byte and wall-clock bound."""
    require(isinstance(os.environ.get("GH_TOKEN"), str) and os.environ["GH_TOKEN"],
            "Actions read token is missing")
    command = ["gh", "api", "--allow-escape-sequences",
               "repos/" + REPOSITORY + "/actions/artifacts/{}/zip".format(artifact_id)]
    deadline = time.monotonic() + timeout
    with tempfile.TemporaryFile() as errors, pathlib.Path(archive).open("xb") as output:
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors)
        except OSError as error:
            raise SourceCacheError("could not start authenticated artifact download") from error
        try:
            total = 0
            with selectors.DefaultSelector() as watcher:
                watcher.register(process.stdout, selectors.EVENT_READ)
                while True:
                    remaining = deadline - time.monotonic()
                    require(remaining > 0, "source cache ZIP download exceeded deadline")
                    require(watcher.select(remaining), "source cache ZIP download exceeded deadline")
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    total += len(chunk)
                    require(total <= limit, "source cache ZIP download exceeds byte limit")
                    output.write(chunk)
            remaining = deadline - time.monotonic()
            require(remaining > 0, "source cache ZIP download exceeded deadline")
            require(process.wait(timeout=remaining) == 0 and total > 0,
                    "authenticated source cache ZIP download failed")
        except (OSError, subprocess.SubprocessError) as error:
            raise SourceCacheError("authenticated source cache ZIP download failed") from error
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            process.stdout.close()


def extract_verified_zip(archive: pathlib.Path, output_root: pathlib.Path,
                         lock_path: pathlib.Path, digest: str, *,
                         max_cache_bytes: int = MAX_CACHE_BYTES,
                         allow_incompatible: bool = False) -> None:
    """Verify transport and original locked archive bytes before publication."""
    require(type(allow_incompatible) is bool, "invalid source compatibility policy")
    require(type(max_cache_bytes) is int and 0 < max_cache_bytes <= MAX_CACHE_BYTES,
            "invalid source cache budget")
    archive, output_root = pathlib.Path(archive), pathlib.Path(output_root)
    require(archive.is_file() and not archive.is_symlink()
            and 0 < archive.stat().st_size <= max_cache_bytes,
            "source cache ZIP is missing or oversized")
    require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
            "invalid source cache ZIP digest")
    require(not output_root.exists() and not output_root.is_symlink(),
            "refusing to replace a source cache")
    hasher = hashlib.sha256()
    with archive.open("rb") as stream:
        while chunk := stream.read(65536):
            hasher.update(chunk)
    require(hasher.hexdigest() == digest, "source cache ZIP digest differs from Actions metadata")
    lock = json.loads(pathlib.Path(lock_path).read_text(encoding="utf-8"))
    records = validated_records(lock, require_download_urls=False)
    expected = {record["archive_file"]: record["archive_sha256"] for record in records}
    require(expected, "locked source closure is empty")
    expected[PREFETCH_MANIFEST_NAME] = None
    output_root.parent.mkdir(parents=True, exist_ok=True)
    try:
        with zipfile.ZipFile(archive) as source:
            members = source.infolist()
            names = [member.filename for member in members]
            require(len(names) <= 256 and len(names) == len(set(names)),
                    "source ZIP has too many or duplicate entries")
            advertised = 0
            for member in members:
                mode = member.external_attr >> 16
                advertised += member.file_size
                require(member.filename == pathlib.PurePosixPath(member.filename).name
                        and not member.is_dir() and not member.flag_bits & 1
                        and stat.S_IFMT(mode) in (0, stat.S_IFREG)
                        and member.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                        and 0 <= member.file_size <= max_cache_bytes
                        and 0 <= member.compress_size <= max_cache_bytes
                        and advertised <= max_cache_bytes,
                        "source ZIP has unsafe or oversized members")
                if member.filename == PREFETCH_MANIFEST_NAME:
                    require(member.file_size <= MAX_MANIFEST_BYTES,
                            "oversized source ZIP prefetch manifest")
            require(PREFETCH_MANIFEST_NAME in names,
                    "source ZIP lacks the mandatory generated prefetch manifest")
            if len(names) != len(expected) or set(names) != set(expected):
                if allow_incompatible:
                    raise IncompatibleSourceCacheError("source ZIP archive set differs from the current lock")
                raise SourceCacheError("source ZIP has missing or unreviewed entries")
            with tempfile.TemporaryDirectory(dir=output_root.parent,
                                             prefix=".source-cache-import-") as directory:
                staging = pathlib.Path(directory) / "cache"
                staging.mkdir()
                total = 0
                for member in members:
                    target = staging / member.filename
                    actual = hashlib.sha256()
                    size = 0
                    with source.open(member) as input_stream, target.open("xb") as output:
                        while chunk := input_stream.read(65536):
                            size += len(chunk)
                            total += len(chunk)
                            require(size <= member.file_size and total <= max_cache_bytes,
                                    "source ZIP expanded beyond reviewed budget")
                            actual.update(chunk)
                            output.write(chunk)
                    require(size == member.file_size, "source ZIP member size differs")
                    if expected[member.filename] is not None and actual.hexdigest() != expected[member.filename]:
                        message = "source ZIP archive differs from the current lock: " + member.filename
                        if allow_incompatible:
                            raise IncompatibleSourceCacheError(message)
                        raise SourceCacheError(message)
                (staging / PREFETCH_MANIFEST_NAME).unlink()
                require(not output_root.exists() and not output_root.is_symlink(),
                        "source cache destination appeared during import")
                staging.rename(output_root)
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
        if isinstance(error, SourceCacheError):
            raise
        raise SourceCacheError("invalid source cache ZIP transport") from error


def _offline_validator(lock_path: pathlib.Path, directory: pathlib.Path) -> None:
    command = [sys.executable, str(pathlib.Path(__file__).with_name("prefetch_adb_helper_sources.py")),
               "--lock", str(lock_path), "--download-root", str(directory),
               "--offline", "--max-cache-bytes", str(MAX_CACHE_BYTES)]
    try:
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.SubprocessError) as error:
        raise SourceCacheError("imported source closure failed full offline verification") from error


def import_source_cache(lock_path: pathlib.Path, repo_root: pathlib.Path,
                        source_sha: str, prior_sha: str, run_id: int,
                        run_attempt: int, artifact_id: int,
                        output_root: pathlib.Path, *, metadata=None,
                        git_runner=None, download=None, validator=None) -> None:
    """Publish no source tree until the transport and every lock entry pass."""
    identity = authenticate_artifact(repo_root, source_sha, prior_sha, run_id,
                                     run_attempt, artifact_id, metadata=metadata,
                                     git_runner=git_runner)
    output_root = pathlib.Path(output_root)
    require(not output_root.exists() and not output_root.is_symlink(),
            "refusing to replace a source cache")
    output_root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output_root.parent,
                                     prefix=".source-cache-transport-") as directory:
        temporary = pathlib.Path(directory)
        zipped = temporary / "archive.zip"
        (download or download_artifact_zip)(artifact_id, zipped,
                                            limit=MAX_CACHE_BYTES, timeout=MAX_DOWNLOAD_SECONDS)
        require(zipped.is_file() and not zipped.is_symlink()
                and zipped.stat().st_size == identity["size"],
                "source cache ZIP size differs from Actions metadata")
        accepted = temporary / "accepted"
        extract_verified_zip(zipped, accepted, lock_path, identity["digest"])
        (validator or _offline_validator)(pathlib.Path(lock_path), accepted)
        generated_manifest = accepted / PREFETCH_MANIFEST_NAME
        require(generated_manifest.is_file() and not generated_manifest.is_symlink()
                and generated_manifest.stat().st_size <= MAX_MANIFEST_BYTES,
                "offline source validator did not produce its bounded manifest")
        generated_manifest.unlink()
        require(not output_root.exists() and not output_root.is_symlink(),
                "source cache destination appeared during verification")
        accepted.rename(output_root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--run-id", required=True, type=int)
    parser.add_argument("--run-attempt", required=True, type=int)
    parser.add_argument("--artifact-id", required=True, type=int)
    parser.add_argument("--prior-sha", required=True)
    parser.add_argument("--output-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        branch = os.environ.get("GITHUB_REF", "")
        require(branch.startswith("refs/heads/") and os.environ.get("GITHUB_REPOSITORY") == REPOSITORY,
                "source cache import requires the canonical branch dispatch")
        import_source_cache(args.lock, args.repo_root, args.source_sha, args.prior_sha,
                            args.run_id, args.run_attempt, args.artifact_id,
                            args.output_root)
        print("verified prior-run ADB source bytes against current lock")
    except SourceCacheError as error:
        parser.exit(1, "source cache transport: " + str(error) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
