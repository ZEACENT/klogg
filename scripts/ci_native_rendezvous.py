#!/usr/bin/env python3
"""Wait for verified-identity, same-run native producers without a DAG-wide build barrier."""
from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


POLL_SECONDS = 30


def identity(environment=None):
    env = os.environ if environment is None else environment
    repository = env.get("GITHUB_REPOSITORY", "")
    sha = env.get("GITHUB_SHA", "")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("invalid current repository")
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("invalid current checkout SHA")
    values = {}
    for key in ("run_id", "run_attempt"):
        value = env.get("GITHUB_" + key.upper(), "")
        if not re.fullmatch(r"[1-9][0-9]*", value):
            raise ValueError("invalid current " + key)
        values[key] = int(value)
    event_name = env.get("GITHUB_EVENT_NAME")
    if event_name not in ("pull_request", "push", "workflow_dispatch"):
        raise ValueError("unsupported native rendezvous event")
    head_sha = sha
    if event_name == "pull_request":
        event = json.loads(Path(env["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        head_sha = event["pull_request"]["head"]["sha"]
    if not isinstance(head_sha, str) or not re.fullmatch(r"[0-9a-f]{40}", head_sha):
        raise ValueError("invalid current run head SHA")
    return dict(repository=repository, sha=sha, head_sha=head_sha, **values)


def check_run(expected, run):
    if (not isinstance(run, dict) or not isinstance(run.get("repository"), dict)
            or run.get("id") != expected["run_id"]
            or run.get("run_attempt") != expected["run_attempt"]
            or run.get("head_sha") != expected["head_sha"]
            or (run.get("repository") or {}).get("full_name") != expected["repository"]):
        raise ValueError("current run identity changed or mismatched")


def timestamp(value):
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ValueError("missing UTC producer/artifact timestamp")
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


def positive_id(value):
    return type(value) is int and value > 0


def evaluate(expected, run, jobs, artifacts, producers):
    """Return artifact IDs only after the latest producer execution succeeded.

    A partial rerun can reuse an older successful job. A newer failed or pending
    execution always supersedes it. Artifact timestamps bind an upload to that
    execution because the artifacts API does not expose an uploading job ID.
    """
    check_run(expected, run)
    if not isinstance(jobs, list) or not isinstance(artifacts, list):
        raise ValueError("malformed native producer listing")
    if not all(isinstance(item, dict) for item in jobs + artifacts):
        raise ValueError("malformed native producer record")
    ready = {}
    waiting = False
    for name, artifact_names in producers.items():
        candidates = [job for job in jobs if job.get("name") == name]
        if not candidates:
            waiting = True
            continue
        for job in candidates:
            if (job.get("run_id") != expected["run_id"] or job.get("head_sha") != expected["head_sha"]
                    or not positive_id(job.get("id")) or not positive_id(job.get("run_attempt"))
                    or job["run_attempt"] > expected["run_attempt"]):
                raise ValueError("native producer identity mismatch: " + name)
        attempt = max(job["run_attempt"] for job in candidates)
        latest = [job for job in candidates if job["run_attempt"] == attempt]
        if len(latest) != 1:
            raise ValueError("ambiguous latest native producer: " + name)
        job = latest[0]
        if job.get("status") in ("queued", "waiting", "pending", "requested", "in_progress"):
            waiting = True
            continue
        if job.get("status") != "completed" or job.get("conclusion") != "success":
            raise ValueError(f"native producer {name}: {job.get('status')}/{job.get('conclusion')}")
        start, end = timestamp(job.get("started_at")), timestamp(job.get("completed_at"))
        if end < start:
            raise ValueError("invalid producer interval: " + name)
        for artifact_name in artifact_names:
            matches = [artifact for artifact in artifacts if artifact.get("name") == artifact_name]
            if not matches:
                waiting = True
                continue
            for artifact in matches:
                ancestry = artifact.get("workflow_run")
                if (not positive_id(artifact.get("id")) or not isinstance(ancestry, dict)
                        or ancestry.get("id") != expected["run_id"]
                        or ancestry.get("head_sha") != expected["head_sha"]):
                    raise ValueError("native artifact identity mismatch: " + artifact_name)
            current = [artifact for artifact in matches
                       if start <= timestamp(artifact.get("created_at")) <= end]
            if not current:
                raise ValueError("native artifact is outside latest producer interval: " + artifact_name)
            if len(current) != 1:
                raise ValueError("ambiguous native artifact: " + artifact_name)
            artifact = current[0]
            if artifact.get("expired") is not False:
                raise ValueError("native artifact expiry mismatch: " + artifact_name)
            ready[artifact_name] = {"artifact_id": artifact["id"], "producer_id": job["id"],
                                    "producer_attempt": attempt, "producer_name": name}
    return None if waiting else ready


def profile(platform, architecture):
    if platform == "macOS" and architecture in ("x86_64", "arm64"):
        target = "macos-" + architecture
        gates = ["KLOGG_ADB_HELPER_REQUIRED", "KLOGG_ENABLE_IOS_NATIVE_STACK"]
        artifacts = {"ios-native-" + architecture: "iOS native stack " + architecture}
    elif platform == "Windows" and architecture == "x86_64":
        target = "windows-x86_64"
        gates = ["KLOGG_ADB_HELPER_REQUIRED"]
        artifacts = {}
    else:
        raise ValueError("unsupported native overlap platform/architecture")
    artifacts.update({"adb-helper-" + target: "Source-built ADB helper " + target,
                      "adb-helper-package-support": "Build ADB helper legal assets"})
    return {"platform": platform, "architecture": architecture, "target": target,
            "gates": gates, "artifacts": artifacts}


def validate_receipt(receipt, expected, architecture, *, platform="macOS"):
    if (not isinstance(receipt, dict) or receipt.get("schema_version") != 1
            or receipt.get("identity") != expected):
        raise ValueError("native rendezvous receipt identity mismatch")
    names = profile(platform, architecture)["artifacts"]
    artifacts = receipt.get("artifacts")
    if not isinstance(artifacts, dict) or set(artifacts) != set(names):
        raise ValueError("native rendezvous receipt artifact names mismatch")
    for name, producer in names.items():
        item = artifacts[name]
        if (not isinstance(item, dict) or item.get("producer_name") != producer
                or not positive_id(item.get("artifact_id")) or not positive_id(item.get("producer_id"))
                or not positive_id(item.get("producer_attempt"))
                or item["producer_attempt"] > expected["run_attempt"]):
            raise ValueError("native rendezvous receipt producer mismatch: " + name)


def github_api(path):
    # --paginate --slurp preserves page boundaries; never accept a truncated first page.
    result = subprocess.run(["gh", "api", "--paginate", "--slurp", path],
                            check=True, capture_output=True, text=True, timeout=90)
    pages = json.loads(result.stdout)
    if not isinstance(pages, list) or not pages or not all(isinstance(page, dict) for page in pages):
        raise ValueError("malformed GitHub API pages")
    for key in ("jobs", "artifacts"):
        if key in pages[0]:
            if not all(isinstance(page.get(key), list) for page in pages):
                raise ValueError("malformed paginated " + key)
            items = [item for page in pages for item in page[key]]
            count = pages[0].get("total_count")
            if type(count) is not int or count != len(items):
                raise ValueError("incomplete paginated " + key)
            return {key: items}
    if len(pages) != 1:
        raise ValueError("unexpected paginated run identity")
    return pages[0]


def wait(expected, producers, *, api=github_api, timeout=1800, clock=time.monotonic, sleep=time.sleep):
    if not 1 <= timeout <= 3600:
        raise ValueError("native rendezvous timeout must be 1..3600 seconds")
    prefix = f"repos/{expected['repository']}/actions/runs/{expected['run_id']}"
    deadline = clock() + timeout
    while clock() < deadline:
        try:
            run = api(prefix)
            check_run(expected, run)
            jobs = api(prefix + "/jobs?filter=all&per_page=100")["jobs"]
            artifacts = api(prefix + "/artifacts?per_page=100")["artifacts"]
            ready = evaluate(expected, run, jobs, artifacts, producers)
            if ready is not None:
                # Reject a newly started attempt between job and artifact snapshots.
                check_run(expected, api(prefix))
                return ready
            print("Waiting for native producers/artifacts: " + ", ".join(producers), flush=True)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            print(f"Native rendezvous API request failed; retrying within deadline: {error}",
                  file=sys.stderr, flush=True)
        sleep(POLL_SECONDS)
    raise TimeoutError("native producer rendezvous deadline exceeded")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--architecture", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--platform", choices=("macOS", "Windows"), default="macOS")
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args(argv)
    try:
        expected = identity()
        contract = profile(args.platform, args.architecture)
        target = contract["target"]
        producers = {producer: [name] for name, producer in contract["artifacts"].items()}
        ready = wait(expected, producers, timeout=args.timeout)
        receipt = {"schema_version": 1, "identity": expected, "artifacts": ready}
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        with args.receipt.open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, sort_keys=True, indent=2)
            stream.write("\n")
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            for key, name in (("adb-artifact-id", "adb-helper-" + target),
                              ("ios-artifact-id", "ios-native-" + args.architecture),
                              ("support-artifact-id", "adb-helper-package-support")):
                if name in ready:
                    output.write(f"{key}={ready[name]['artifact_id']}\n")
        return 0
    except (OSError, ValueError, KeyError, TimeoutError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
