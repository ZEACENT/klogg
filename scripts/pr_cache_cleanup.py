#!/usr/bin/env python3
"""Audit closed-PR Actions caches; deletion requires an approved master dispatch.

Only exact refs/pull/N/{merge,head} are eligible. Cache keys and platform usage
are reporting metadata, never deletion authority. Master, branch, tag and open-PR
caches are protected regardless of budget pressure. Run without --apply first.
No plan file is accepted for execution: apply gathers fresh evidence itself.

GitHub has no atomic compare-and-delete cache API. Rechecks narrow but cannot
eliminate the race with a concurrent PR reopen or workflow rerun. Apply should
be approved during a quiet window; any observed change aborts remaining deletes.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from urllib.parse import urlencode

PR_REF = re.compile(r"refs/pull/([1-9][0-9]*)/(merge|head)\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
ACTIVE_STATUSES = ("queued", "in_progress", "waiting", "requested", "pending")
CONFIRMATION = "delete-closed-pr-caches"
MASTER_REF = "refs/heads/master"
MAX_CACHE_RECORDS = 10000
# GitHub's filtered run API returns at most 1000 results. Hitting that boundary
# is ambiguous, so reject even an apparently complete 1000-record response.
MAX_ACTIVE_RECORDS = 999


class CleanupError(ValueError):
    """Incomplete evidence must never authorize cache deletion."""


def require(condition, message):
    if not condition:
        raise CleanupError(message)


def _positive_integer(value):
    return type(value) is int and value > 0


def _text(value):
    return isinstance(value, str) and bool(value) and all(ord(char) >= 32 for char in value)


def _sha(value):
    return isinstance(value, str) and SHA.fullmatch(value) is not None and value != "0" * 40


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate API JSON field")
        result[key] = value
    return result


class GitHubAPI:
    """Use the repository's gh API idiom, explicit methods and a fixed API host."""

    def __init__(self, repository):
        require(isinstance(repository, str) and re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_.-]+", repository) is not None
            and repository.split("/")[1] not in (".", ".."), "invalid repository")
        self.repository = repository

    def _request(self, method, endpoint):
        require(endpoint.startswith("/") and not endpoint.startswith("//")
                and ".." not in endpoint and "#" not in endpoint,
                "invalid API endpoint")
        command = ["gh", "api", "--hostname", "github.com", "--method", method,
                   "-H", "Accept: application/vnd.github+json",
                   "-H", "X-GitHub-Api-Version: 2022-11-28",
                   f"/repos/{self.repository}{endpoint}"]
        try:
            response = subprocess.run(command, check=True, capture_output=True,
                                      text=True, timeout=90)
        except (OSError, subprocess.SubprocessError) as error:
            raise CleanupError(f"GitHub API {method} failed: {endpoint}") from error
        require(len(response.stdout) <= 10 * 1024 * 1024, "oversized API response")
        return response.stdout

    def get_json(self, endpoint):
        try:
            result = json.loads(self._request("GET", endpoint), object_pairs_hook=_unique_object)
        except (ValueError, UnicodeError) as error:
            raise CleanupError("invalid API JSON") from error
        require(isinstance(result, dict), "API response must be an object")
        return result

    def delete_cache(self, identifier):
        require(_positive_integer(identifier), "invalid cache ID")
        # Never delete by key/ref: those endpoints can remove multiple caches.
        self._request("DELETE", f"/actions/caches/{identifier}")


def _list(api, endpoint, field, *, maximum, parameters=None):
    """Bounded, stable-count pagination; duplicates/short pages abort the plan."""
    records = []
    identifiers = set()
    expected_total = None
    for page in range(1, maximum // 100 + 2):
        query = urlencode({**(parameters or {}), "per_page": 100, "page": page})
        response = api.get_json(f"{endpoint}?{query}")
        require(isinstance(response, dict), "malformed paginated API response")
        items, total = response.get(field), response.get("total_count")
        require(type(total) is int and 0 <= total <= maximum
                and isinstance(items, list) and len(items) <= 100,
                "malformed or oversized paginated API results")
        if expected_total is None:
            expected_total = total
        require(total == expected_total, "API pagination total changed")
        require(len(records) + len(items) <= total, "API pagination exceeds total")
        for item in items:
            require(isinstance(item, dict) and _positive_integer(item.get("id"))
                    and item["id"] not in identifiers, "invalid or duplicate paginated API ID")
            identifiers.add(item["id"])
            records.append(item)
        if len(records) == total:
            return records
        require(len(items) == 100, "truncated API pagination")
    raise CleanupError("API pagination exceeded bound")


def _timestamp(value):
    require(isinstance(value, str), "missing cache timestamp")
    try:
        timestamp = datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise CleanupError("invalid cache timestamp") from error
    require(timestamp.tzinfo is not None, "cache timestamp lacks timezone")


def _caches(api, ref=None):
    records = _list(api, "/actions/caches", "actions_caches", maximum=MAX_CACHE_RECORDS,
                    parameters={"ref": ref} if ref is not None else None)
    for record in records:
        require(all(_text(record.get(field)) for field in ("ref", "key", "version"))
                and type(record.get("size_in_bytes")) is int and record["size_in_bytes"] >= 0,
                "malformed cache metadata")
        for field in ("created_at", "last_accessed_at"):
            _timestamp(record.get(field))
        require(ref is None or record["ref"] == ref, "cache API returned a different ref")
    return records


def _pull(api, number):
    record = api.get_json(f"/pulls/{number}")
    require(isinstance(record, dict) and type(record.get("number")) is int
            and record["number"] == number and record.get("state") in ("open", "closed")
            and type(record.get("merged")) is bool, "malformed or mismatched PR metadata")
    head = record.get("head")
    require(isinstance(head, dict) and _sha(head.get("sha")) and _text(head.get("ref")),
            "malformed PR head")
    merge_sha = record.get("merge_commit_sha")
    require("merge_commit_sha" in record and (merge_sha is None or _sha(merge_sha)),
            "malformed PR merge SHA")
    require(not record["merged"] or record["state"] == "closed", "inconsistent PR state")
    return {"number": number, "state": record["state"], "merged": record["merged"],
            "head_sha": head["sha"], "head_ref": head["ref"], "merge_sha": merge_sha}


def _active_runs(api):
    """Read every nonterminal status, not just queued/in_progress or one page.

    Querying only one head SHA misses older attempts and fork runs with empty PR
    associations. Match by PR number, head/merge SHA and branch/ref instead.
    """
    result = []
    seen = set()
    for status in ACTIVE_STATUSES:
        runs = _list(api, "/actions/runs", "workflow_runs", maximum=MAX_ACTIVE_RECORDS,
                     parameters={"status": status})
        for run in runs:
            require(run.get("status") == status and _sha(run.get("head_sha"))
                    and _text(run.get("head_branch")) and isinstance(run.get("pull_requests"), list),
                    "malformed active workflow run")
            require(run["id"] not in seen, "active run changed status during pagination")
            seen.add(run["id"])
            for pr in run["pull_requests"]:
                require(isinstance(pr, dict) and _positive_integer(pr.get("number")),
                        "malformed workflow run PR association")
            result.append(run)
    return result


def _needed(pr, runs):
    number = pr["number"]
    refs = {pr["head_ref"], f"refs/heads/{pr['head_ref']}"}
    for suffix in ("head", "merge"):
        refs.update((f"refs/pull/{number}/{suffix}", f"pull/{number}/{suffix}", f"{number}/{suffix}"))
    return any(run["head_sha"] in (pr["head_sha"], pr["merge_sha"])
               or run["head_branch"] in refs
               or any(item["number"] == number for item in run["pull_requests"])
               for run in runs)


def _platform(key):
    if key.startswith(("ubuntu-", "linux-")):
        return "linux"
    if key.startswith(("intel-qt", "arm64-qt", "macos-")):
        return "macos"
    if key.startswith(("x64-qt", "x86-qt", "windows-", "msys2-")):
        return "windows"
    return "shared_or_unknown"


def _authorize(api, apply, confirmation, environment):
    require(apply or not confirmation, "confirmation requires explicit --apply")
    if not apply:
        return
    require(confirmation == CONFIRMATION, "--apply requires the exact manual confirmation")
    require(environment.get("GITHUB_EVENT_NAME") == "workflow_dispatch"
            and environment.get("GITHUB_REF") == MASTER_REF
            and environment.get("GITHUB_REPOSITORY") == api.repository,
            "--apply requires a workflow_dispatch on master in the same repository")


def cleanup(api, *, apply=False, confirmation="", environment=None):
    """Return a JSON-safe audit; optionally revalidate and delete exact cache IDs."""
    _authorize(api, apply, confirmation, os.environ if environment is None else environment)
    caches = _caches(api)
    active = _active_runs(api)
    pulls = {}
    candidates, protected = [], []
    usage = {platform: {"total_bytes": 0, "master_bytes": 0, "eligible_bytes": 0}
             for platform in ("linux", "macos", "windows", "shared_or_unknown")}
    for cache in caches:
        platform = usage[_platform(cache["key"])]
        platform["total_bytes"] += cache["size_in_bytes"]
        if cache["ref"] == MASTER_REF:
            platform["master_bytes"] += cache["size_in_bytes"]
        match = PR_REF.fullmatch(cache["ref"])
        reason = "not-an-explicit-pr-ref"
        if match:
            number = int(match.group(1))
            if number not in pulls:
                pulls[number] = _pull(api, number)
            pr = pulls[number]
            if pr["state"] != "closed":
                reason = "open-pr"
            elif _needed(pr, active):
                reason = "active-run"
            else:
                candidates.append({"cache": cache, "pull_request": pr})
                platform["eligible_bytes"] += cache["size_in_bytes"]
                continue
        protected.append({"cache": cache, "reason": reason})
    result = {"mode": "apply" if apply else "dry-run", "repository": api.repository,
              "candidates": candidates, "protected": protected, "usage_by_platform": usage,
              "deleted": [], "policy": "Exact closed-PR refs only; master/open/active caches are protected. "
              "Platform bytes are an audit, not an eviction budget. Rechecks are not an atomic lock."}
    if apply:
        try:
            for candidate in candidates:
                cache, expected_pr = candidate["cache"], candidate["pull_request"]
                number = expected_pr["number"]
                pr = _pull(api, number)
                require(pr == expected_pr and pr["state"] == "closed", "PR changed before deletion")
                current = {item["id"]: item for item in _caches(api, cache["ref"])}
                require(current.get(cache["id"]) == cache, "cache changed or disappeared before deletion")
                require(not _needed(pr, _active_runs(api)), "active run appeared before deletion")
                require(_pull(api, number) == expected_pr, "PR changed during recheck")
                api.delete_cache(cache["id"])
                result["deleted"].append(cache["id"])
        except CleanupError as error:
            raise CleanupError(f"{error}; stopped, already deleted IDs: {result['deleted']}") from error
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""),
                        help="GitHub owner/repository (defaults to GITHUB_REPOSITORY)")
    parser.add_argument("--apply", action="store_true", help="Delete after fresh safety checks (default: audit only)")
    parser.add_argument("--confirm", default="", help=f"Required with --apply: {CONFIRMATION}")
    args = parser.parse_args(argv)
    try:
        result = cleanup(GitHubAPI(args.repo), apply=args.apply, confirmation=args.confirm)
    except CleanupError as error:
        print(f"Cache cleanup refused: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
