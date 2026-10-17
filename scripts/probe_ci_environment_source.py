#!/usr/bin/env python3
"""Read-only source-artifact metadata eligibility, never payload qualification."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys

import ci_environment_source_cache as cache

MAX_METADATA_REQUESTS = 4
IDENTIFIER = re.compile(r"[1-9][0-9]{0,19}\Z")
BRANCH = re.compile(r"[A-Za-z0-9._/-]+\Z")


class ProbeParser(argparse.ArgumentParser):
    def error(self, message):
        # Do not echo rejected workflow inputs in CLI error messages.
        self.exit(2, "source metadata probe: invalid arguments\n")


def positive_identifier(value):
    if IDENTIFIER.fullmatch(value) is None:
        raise argparse.ArgumentTypeError("invalid positive identifier")
    return int(value)


def dispatch_context(expected_source):
    source = os.environ.get("GITHUB_SHA", "")
    branch = os.environ.get("GITHUB_REF", "")
    default_branch = os.environ.get("KLOGG_PROBE_DEFAULT_BRANCH", "")
    run_id = os.environ.get("GITHUB_RUN_ID", "")
    cache.require(
        os.environ.get("GITHUB_ACTIONS") == "true"
        and os.environ.get("GITHUB_EVENT_NAME") == "workflow_dispatch"
        and os.environ.get("GITHUB_REPOSITORY") == cache.REPOSITORY
        and branch.startswith("refs/heads/")
        and BRANCH.fullmatch(branch[len("refs/heads/"):]) is not None
        and BRANCH.fullmatch(default_branch) is not None
        and branch != "refs/heads/" + default_branch
        and cache.SHA.fullmatch(source) is not None and source != "0" * 40
        and source == expected_source
        and IDENTIFIER.fullmatch(run_id) is not None
        and os.environ.get("GITHUB_RUN_ATTEMPT") == "1"
        and bool(os.environ.get("GH_TOKEN")),
        "requires a first-attempt canonical nondefault dispatch at the exact expected source",
    )
    return {"source_sha": source, "run_id": int(run_id), "run_attempt": 1}


def main(argv=None) -> int:
    parser = ProbeParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--expected-source-sha", required=True)
    parser.add_argument("--prior-sha", required=True)
    parser.add_argument("--run-id", required=True, type=positive_identifier)
    parser.add_argument("--run-attempt", required=True, type=positive_identifier)
    parser.add_argument("--artifact-id", required=True, type=positive_identifier)
    args = parser.parse_args(argv)
    try:
        context = dispatch_context(args.expected_source_sha)
        requests = 0

        def metadata(path):
            nonlocal requests
            cache.require(requests < MAX_METADATA_REQUESTS, "metadata request limit exceeded")
            requests += 1
            return cache._metadata(path)

        identity = cache.authenticate_artifact(
            args.repo_root, context["source_sha"], args.prior_sha,
            args.run_id, args.run_attempt, args.artifact_id, metadata=metadata,
        )
        cache.require(requests == MAX_METADATA_REQUESTS, "incomplete metadata probe")
        print(json.dumps({
            "schema_version": 1,
            "status": "metadata-eligible",
            "scope": "source-artifact-metadata-only",
            "qualified": False,
            "archive_downloaded": False,
            "metadata_requests": requests,
            "probe": context,
            "transport": {key: identity[key] for key in (
                "digest", "size", "run_id", "run_attempt", "artifact_id", "prior_sha")},
        }, sort_keys=True))
    except cache.SourceCacheError as error:
        print("source metadata probe: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
