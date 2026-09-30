#!/usr/bin/env python3
"""Discover canonical ordinary-run source artifacts as current-lock-verified transport."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

import ci_environment_source_cache as cache

MAX_CANDIDATES = 20
MAX_ATTEMPTS = 3
MAX_ZIP_DOWNLOADS = 2
MAX_DISCOVERY_SECONDS = 180
MAX_TOTAL_SECONDS = 900
ARTIFACT_NAME = "adb-helper-source-cache"
INVALID_REF_CHARS = re.compile(r"[\x00-\x20\x7f~^:?*\[\\]")


def _valid_branch(value: object) -> bool:
    """Accept Git branch syntax, including +, @ and non-ASCII names."""
    return (isinstance(value, str) and bool(value) and value != "@"
            and not value.startswith(("-", "/")) and not value.endswith(("/", "."))
            and "//" not in value and ".." not in value and "@{" not in value
            and INVALID_REF_CHARS.search(value) is None
            and all(not part.startswith(".") and not part.endswith(".lock")
                    for part in value.split("/")))


def require(condition: bool, message: str) -> None:
    cache.require(condition, message)


def _document(path: pathlib.Path) -> dict:
    try:
        require(path.is_file() and not path.is_symlink()
                and path.stat().st_size <= cache.core.MAX_METADATA_BYTES,
                "missing or oversized authenticated Actions event")
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise cache.SourceCacheError("invalid authenticated Actions event") from error
    require(isinstance(document, dict), "Actions event is not an object")
    return document


def _identity(environ: dict[str, str], git, repo_root: pathlib.Path):
    require(environ.get("GITHUB_REPOSITORY") == cache.REPOSITORY,
            "ordinary source transport requires canonical repository")
    event = environ.get("GITHUB_EVENT_NAME")
    require(event in ("pull_request", "push"), "unsupported ordinary Actions event")
    sha = environ.get("GITHUB_SHA")
    require(isinstance(sha, str) and cache.SHA.fullmatch(sha) is not None
            and sha != "0" * 40, "invalid checked-out revision")
    require(git(["git", "rev-parse", "HEAD"], cwd=repo_root).strip() == sha,
            "Actions checkout differs from event revision")
    path = environ.get("GITHUB_EVENT_PATH")
    require(isinstance(path, str) and path, "authenticated Actions event is missing")
    document = _document(pathlib.Path(path))
    repository = document.get("repository")
    require(isinstance(repository, dict) and type(repository.get("id")) is int
            and repository["id"] > 0 and repository.get("full_name") == cache.REPOSITORY,
            "Actions event is not from canonical repository")
    run_id = environ.get("GITHUB_RUN_ID")
    require(isinstance(run_id, str) and run_id.isdecimal() and int(run_id) > 0,
            "invalid current Actions run identity")
    if event == "push":
        require(environ.get("GITHUB_REF") == "refs/heads/master"
                and document.get("ref") == "refs/heads/master"
                and document.get("after") == sha,
                "source transport push is not the canonical master revision")
        return event, sha, None, repository["id"], int(run_id)
    pull = document.get("pull_request")
    head = pull.get("head") if isinstance(pull, dict) else None
    require(isinstance(head, dict), "pull request head is missing")
    head_sha, branch, head_repo = head.get("sha"), head.get("ref"), head.get("repo")
    require(isinstance(head_sha, str) and cache.SHA.fullmatch(head_sha) is not None
            and head_sha != "0" * 40 and _valid_branch(branch)
            and branch == environ.get("GITHUB_HEAD_REF")
            and isinstance(head_repo, dict) and type(head_repo.get("id")) is int
            and isinstance(head_repo.get("full_name"), str),
            "pull request head identity is malformed")
    if head_repo["id"] != repository["id"] or head_repo["full_name"] != cache.REPOSITORY:
        return event, head_sha, False, repository["id"], int(run_id)
    require(_ancestor(head_sha, sha, repo_root, git),
            "pull request event head is not in checked-out merge revision")
    return event, head_sha, branch, repository["id"], int(run_id)


def _git(command: list[str], *, cwd: pathlib.Path) -> str:
    try:
        result = subprocess.run(command, cwd=cwd, check=True, capture_output=True,
                                text=True, timeout=30)
    except subprocess.CalledProcessError as error:
        if command[1:3] == ["merge-base", "--is-ancestor"] and error.returncode == 1:
            raise
        raise cache.SourceCacheError("ordinary source provenance git command failed") from error
    except (OSError, subprocess.SubprocessError) as error:
        raise cache.SourceCacheError("ordinary source provenance git command failed") from error
    return result.stdout


def _ancestor(prior: str, current: str, repo_root: pathlib.Path, git) -> bool:
    try:
        git(["git", "merge-base", "--is-ancestor", prior, current], cwd=repo_root)
        return True
    except subprocess.CalledProcessError as error:
        if error.returncode == 1:
            return False
        raise cache.SourceCacheError("source ancestry could not be verified") from error
    except (OSError, subprocess.SubprocessError) as error:
        raise cache.SourceCacheError("source ancestry could not be verified") from error


def _candidate(artifact: object, repository_id: int, current_run: int):
    require(isinstance(artifact, dict), "malformed source artifact list entry")
    identifier = artifact.get("id")
    require(type(identifier) is int and identifier > 0, "invalid source artifact list identity")
    if artifact.get("name") != ARTIFACT_NAME or artifact.get("expired") is True:
        return None
    require(artifact.get("expired") is False, "source artifact expiry is unknown")
    origin = artifact.get("workflow_run")
    require(isinstance(origin, dict), "source artifact origin is missing")
    run_id = origin.get("id")
    require(type(run_id) is int and run_id > 0
            and type(origin.get("repository_id")) is int
            and type(origin.get("head_repository_id")) is int,
            "source artifact origin is malformed")
    if run_id == current_run or origin["repository_id"] != repository_id or origin["head_repository_id"] != repository_id:
        return None
    sha, branch = origin.get("head_sha"), origin.get("head_branch")
    if (not isinstance(sha, str) or cache.SHA.fullmatch(sha) is None
            or sha == "0" * 40 or not _valid_branch(branch)):
        return None
    stamp = cache._timestamp(artifact.get("created_at"))
    return stamp, identifier, run_id, sha, branch


def _producer(artifact: dict, run: dict, run_id: int, prior: str,
              branch: str, repository_id: int, event: str, get, check_deadline):
    repository, head_repository = run.get("repository"), run.get("head_repository")
    origin = artifact["workflow_run"]
    require(type(run.get("id")) is int and run["id"] == run_id
            and isinstance(repository, dict) and isinstance(head_repository, dict)
            and repository.get("id") == repository_id
            and head_repository.get("id") == repository_id
            and repository.get("full_name") == cache.REPOSITORY
            and head_repository.get("full_name") == cache.REPOSITORY
            and origin.get("repository_id") == repository_id
            and origin.get("head_repository_id") == repository_id
            and origin.get("id") == run_id and origin.get("head_sha") == prior
            and origin.get("head_branch") == branch
            and run.get("head_sha") == prior and run.get("head_branch") == branch
            and run.get("event") in ({"pull_request"} if event == "pull_request" else {"pull_request", "push"})
            and run.get("path") == cache.WORKFLOW
            and type(run.get("run_attempt")) is int and run["run_attempt"] > 0,
            "source artifact lacks canonical run provenance")
    prefix = "repos/" + cache.REPOSITORY + "/actions/runs/" + str(run_id)
    for number in range(run["run_attempt"], max(0, run["run_attempt"] - MAX_ATTEMPTS), -1):
        check_deadline()
        attempt = get(prefix + "/attempts/" + str(number))
        jobs = get(prefix + "/attempts/{}/jobs?per_page=100".format(number))
        require(isinstance(attempt, dict) and isinstance(jobs, dict)
                and type(attempt.get("id")) is int and attempt["id"] == run_id
                and type(attempt.get("run_attempt")) is int and attempt["run_attempt"] == number
                and attempt.get("head_sha") == prior
                and isinstance(jobs.get("jobs"), list)
                and type(jobs.get("total_count")) is int
                and jobs["total_count"] == len(jobs["jobs"]) <= 100,
                "source artifact attempt provenance is incomplete")
        producers = [job for job in jobs["jobs"] if isinstance(job, dict)
                     and job.get("name") == cache.PREFETCH_JOB]
        require(len(producers) <= 1, "ambiguous source artifact producer jobs")
        if not producers:
            continue
        job = producers[0]
        require(type(job.get("run_attempt")) is int and job["run_attempt"] == number,
                "source artifact producer attempt is mismatched")
        if job.get("status") != "completed" or job.get("conclusion") != "success":
            continue
        started = cache._timestamp(attempt.get("run_started_at"))
        job_start = cache._timestamp(job.get("started_at"))
        created = cache._timestamp(artifact.get("created_at"))
        job_end = cache._timestamp(job.get("completed_at"))
        if started <= job_start <= created <= job_end:
            return True
    return False


def discover(lock_path: pathlib.Path, repo_root: pathlib.Path,
             download_root: pathlib.Path, *, environ=None, metadata=None,
             git_runner=None, download=None, validator=None) -> int:
    """Return 0 imported, 2 no compatible transport; raise on broken trust/transport."""
    environment = os.environ if environ is None else environ
    get = metadata or cache._metadata
    git = git_runner or _git
    repo_root = pathlib.Path(repo_root).resolve()
    output_root = pathlib.Path(download_root)
    require(not output_root.exists() and not output_root.is_symlink(),
            "refusing to replace a source cache")
    event, head, branch, repository_id, current_run = _identity(environment, git, repo_root)
    if branch is False:
        print("fork pull request has no canonical source artifact", file=sys.stderr)
        return 2
    require(isinstance(environment.get("GH_TOKEN"), str) and environment["GH_TOKEN"],
            "authenticated Actions read token is missing")
    start = time.monotonic()
    transport_seconds = 0.0

    def check_deadline():
        require(time.monotonic() - start - transport_seconds <= MAX_DISCOVERY_SECONDS,
                "source artifact discovery exceeded deadline")

    prefix = "repos/" + cache.REPOSITORY + "/actions/"
    check_deadline()
    listing = get(prefix + "artifacts?name=" + ARTIFACT_NAME + "&per_page=20")
    require(isinstance(listing, dict) and isinstance(listing.get("artifacts"), list)
            and type(listing.get("total_count")) is int and listing["total_count"] >= len(listing["artifacts"])
            and len(listing["artifacts"]) <= MAX_CANDIDATES,
            "source artifact listing is malformed or exceeds candidate budget")
    candidates = []
    seen_ids = set()
    for artifact in listing["artifacts"]:
        require(isinstance(artifact, dict) and type(artifact.get("id")) is int
                and artifact["id"] not in seen_ids,
                "duplicate or invalid source artifact list identity")
        seen_ids.add(artifact["id"])
        candidate = _candidate(artifact, repository_id, current_run)
        if candidate is not None and (event == "push" or
           (candidate[4] == branch and _ancestor(candidate[3], head, repo_root, git))):
            candidates.append(candidate)
    candidates.sort(reverse=True)
    downloaded = 0
    output_root.parent.mkdir(parents=True, exist_ok=True)
    for _, artifact_id, run_id, prior, producer_branch in candidates:
        if downloaded >= MAX_ZIP_DOWNLOADS:
            break
        check_deadline()
        require(time.monotonic() - start < MAX_TOTAL_SECONDS,
                "source artifact transport exceeded total deadline")
        artifact = get(prefix + "artifacts/" + str(artifact_id))
        run = get(prefix + "runs/" + str(run_id))
        require(isinstance(artifact, dict) and isinstance(run, dict)
                and artifact.get("id") == artifact_id
                and artifact.get("name") == ARTIFACT_NAME
                and artifact.get("expired") is False,
                "selected source artifact metadata differs from discovery")
        match = cache.DIGEST.fullmatch(artifact.get("digest", "")) if isinstance(artifact.get("digest"), str) else None
        require(type(artifact.get("size_in_bytes")) is int
                and 0 < artifact["size_in_bytes"] <= cache.MAX_CACHE_BYTES and match is not None,
                "selected source artifact is missing size or digest")
        require(_candidate(artifact, repository_id, current_run) ==
                next(candidate for candidate in candidates if candidate[1] == artifact_id),
                "selected source artifact identity changed after discovery")
        if not _producer(artifact, run, run_id, prior, producer_branch,
                         repository_id, event, get, check_deadline):
            continue
        check_deadline()
        downloaded += 1
        transport_started = time.monotonic()
        with tempfile.TemporaryDirectory(dir=output_root.parent,
                                         prefix=".source-cache-transport-") as directory:
            temporary = pathlib.Path(directory)
            zipped = temporary / "archive.zip"
            (download or cache.download_artifact_zip)(artifact_id, zipped,
                limit=cache.MAX_CACHE_BYTES,
                timeout=max(1, min(cache.MAX_DOWNLOAD_SECONDS,
                                   int(MAX_TOTAL_SECONDS - (time.monotonic() - start)))))
            require(time.monotonic() - start <= MAX_TOTAL_SECONDS,
                    "source artifact transport exceeded total deadline")
            require(zipped.is_file() and not zipped.is_symlink()
                    and zipped.stat().st_size == artifact["size_in_bytes"],
                    "source artifact ZIP size differs from authenticated metadata")
            accepted = temporary / "accepted"
            try:
                cache.extract_verified_zip(zipped, accepted, lock_path, match.group(1),
                                           allow_incompatible=True)
            except cache.IncompatibleSourceCacheError:
                transport_seconds += time.monotonic() - transport_started
                print("source artifact {} differs from current lock".format(artifact_id),
                      file=sys.stderr)
                continue
            (validator or cache._offline_validator)(pathlib.Path(lock_path), accepted)
            manifest = accepted / cache.PREFETCH_MANIFEST_NAME
            require(manifest.is_file() and not manifest.is_symlink()
                    and manifest.stat().st_size <= cache.MAX_MANIFEST_BYTES,
                    "offline source validator did not produce its bounded manifest")
            manifest.unlink()
            require(not output_root.exists() and not output_root.is_symlink(),
                    "source cache destination appeared during verification")
            accepted.rename(output_root)
            print("imported authenticated source artifact {} against current lock".format(artifact_id))
            return 0
    print("no compatible authenticated source artifact after {} ZIP downloads".format(downloaded),
          file=sys.stderr)
    return 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--download-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        return discover(args.lock, args.repo_root, args.download_root)
    except (cache.SourceCacheError, OSError, ValueError) as error:
        print("ordinary source transport: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
