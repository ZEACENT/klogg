#!/usr/bin/env python3
"""Confirm compiler-cache persistence through the read-only GitHub cache API."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import urlencode


def validate_scope(repository, key, ref):
    if not re.fullmatch(r"[A-Za-z0-9-]+/[A-Za-z0-9][A-Za-z0-9_.-]*", repository):
        raise ValueError("invalid cache repository")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,511}", key):
        raise ValueError("invalid compiler cache key")
    if (not re.fullmatch(r"refs/(?:heads|tags)/[A-Za-z0-9_][A-Za-z0-9._/-]*"
                         r"|refs/pull/[1-9][0-9]*/(?:head|merge)", ref)
            or ".." in ref or "//" in ref or ref.endswith(("/", ".", ".lock"))):
        raise ValueError("invalid cache ref")


def recovery_key(primary, run_id, attempt):
    validate_scope("owner/repo", primary, "refs/heads/master")
    if any(not re.fullmatch(r"[1-9][0-9]*", str(value)) for value in (run_id, attempt)):
        raise ValueError("invalid cache recovery run identity")
    key = f"{primary}-recovery-{run_id}-{attempt}"
    if len(key) > 512:
        raise ValueError("cache recovery key exceeds the backend limit")
    return key


def select_cache(pages, primary, ref, *, matched_key=""):
    """Require compatible-version lookup plus complete, exact-ref REST evidence."""
    validate_scope("owner/repo", primary, ref)
    if not isinstance(pages, list) or not pages:
        raise ValueError("cache API did not return complete paginated objects")
    total, entries = None, []
    for index, page in enumerate(pages):
        if (not isinstance(page, dict) or type(page.get("total_count")) is not int
                or page["total_count"] < 0 or not isinstance(page.get("actions_caches"), list)):
            raise ValueError("malformed cache API page")
        if total is None:
            total = page["total_count"]
        if page["total_count"] != total:
            raise ValueError("cache API count changed during pagination")
        batch = page["actions_caches"]
        if len(batch) > 100 or (index < len(pages) - 1 and len(batch) != 100):
            raise ValueError("incomplete cache API pagination")
        entries.extend(batch)
    if len(entries) != total or len(pages) != max(1, (total + 99) // 100):
        raise ValueError("incomplete cache API pagination")
    identifiers, eligible = set(), set()
    recovery = re.compile(re.escape(primary) + r"-recovery-[1-9][0-9]*-[1-9][0-9]*")
    if matched_key and matched_key != primary and not recovery.fullmatch(matched_key):
        raise ValueError("compatible lookup returned a key outside the compiler cache generation")
    for entry in entries:
        if (not isinstance(entry, dict) or type(entry.get("id")) is not int or entry["id"] <= 0
                or not isinstance(entry.get("key"), str) or not isinstance(entry.get("ref"), str)
                or type(entry.get("size_in_bytes")) is not int or entry["size_in_bytes"] < 0):
            raise ValueError("malformed cache API entry")
        if entry["id"] in identifiers:
            raise ValueError("duplicate cache API entry across pages")
        identifiers.add(entry["id"])
        if (entry["ref"] == ref and entry["size_in_bytes"] > 0
                and (entry["key"] == primary or recovery.fullmatch(entry["key"]))):
            eligible.add(entry["key"])
    if matched_key and matched_key in eligible:
        return {"state": "persisted", "persisted-key": matched_key}
    # The SDK can swallow lookup service errors, so a blank match is not absence
    # while REST has eligible rows. Old-version-only rows are also ambiguous:
    # deliberate path/protocol migrations must bump the caller's key namespace.
    state = "unconfirmed" if matched_key or eligible else "missing"
    return {"state": state, "persisted-key": ""}


def fetch_caches(repository, key, ref):
    validate_scope(repository, key, ref)
    query = urlencode({"key": key, "ref": ref, "per_page": 100})
    result = subprocess.run(
        ["gh", "api", f"repos/{repository}/actions/caches?{query}", "--paginate", "--slurp"],
        capture_output=True, text=True, check=True, timeout=10)
    return json.loads(result.stdout)


def probe(repository, key, ref, *, matched_key="", fetch=fetch_caches):
    """Observe once; retries belong to the action so they refresh lookup proof too."""
    validate_scope(repository, key, ref)
    return select_cache(fetch(repository, key, ref), key, ref, matched_key=matched_key)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", required=True)
    parser.add_argument("--matched-key", required=True,
                        help="Compatible-version key from the pinned cache lookup action; empty on miss")
    parser.add_argument("--require-persisted", action="store_true")
    args = parser.parse_args(argv)
    try:
        key = recovery_key(args.key, os.environ["GITHUB_RUN_ID"], os.environ["GITHUB_RUN_ATTEMPT"])
        result = probe(os.environ["GITHUB_REPOSITORY"], args.key, os.environ["GITHUB_REF"],
                       matched_key=args.matched_key)
        if args.require_persisted and result["state"] != "persisted":
            raise ValueError("compiler cache persistence unconfirmed after primary and recovery saves")
        result["recovery-key"] = key
        with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
            output.write("".join(f"{name}={value}\n" for name, value in result.items()))
        print(json.dumps(result, sort_keys=True))
        return 0
    except (KeyError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Compiler cache persistence verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
