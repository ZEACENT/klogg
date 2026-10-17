#!/usr/bin/env python3
"""Collect read-only GitHub CI evidence and report comparable build samples.

Run with python3 -I. Archives are downloaded into separate fresh temporary
folders, read as bounded JSON, never extracted or executed. Missing evidence is
explicit; queue and dependency waiting cannot be separated without a supplied DAG.
"""
from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import io
import json
import math
from pathlib import Path, PurePosixPath
import statistics
import subprocess
import tempfile
import zipfile

MAX_METRICS_BYTES = 8 * 1024 * 1024
PHASES = ("configure", "compile", "configure_compile", "test", "package", "upload", "cache", "prepare", "other")


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def gh_json(endpoint):
    return json.loads(subprocess.run(["gh", "api", endpoint], check=True,
                                     capture_output=True, text=True).stdout)


def valid_build_duration(value):
    """JSON booleans and non-finite floats are not duration evidence."""
    return (type(value) in (int, float) and value >= 0
            and (isinstance(value, int) or math.isfinite(value)))


def metric_provenance(metric):
    provenance = metric.get("provenance")
    if provenance is None:
        return {}  # Legacy metrics remain readable, but cannot establish identity.
    if not isinstance(provenance, dict):
        raise ValueError("artifact provenance must be an object")
    return provenance


def read_metrics_zip(raw, run_id, attempt):
    """Read the sole metrics JSON without trusting archive paths or importing code."""
    if len(raw) > MAX_METRICS_BYTES:
        raise ValueError("metrics archive exceeds size limit")
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        members = archive.infolist()
        if len(members) != 1:
            raise ValueError("metrics archive must contain exactly one JSON file")
        member = members[0]
        path = PurePosixPath(member.filename)
        if (path.is_absolute() or ".." in path.parts or "\\" in member.filename
                or path.suffix != ".json" or member.file_size > MAX_METRICS_BYTES):
            raise ValueError("unsafe or oversized metrics member")
        value = json.loads(archive.read(member))
    if not isinstance(value, dict) or not valid_build_duration(value.get("build_wall_ms")):
        raise ValueError("missing or invalid build wall-time evidence")
    provenance = metric_provenance(value)
    if provenance and (str(provenance.get("run_id")) != str(run_id)
                       or str(provenance.get("run_attempt")) != str(attempt)):
        raise ValueError("artifact provenance belongs to another run/attempt")
    return value


def collect(repository, run_id, output, expected_sha=None, *, attempt=None, api=gh_json):
    """Persist original API responses and artifact payloads, with acquisition hashes."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    base = f"repos/{repository}/actions/runs/{run_id}"
    run = api(base if attempt is None else f"{base}/attempts/{attempt}")
    write_json(output / "run.json", run)
    if expected_sha and run["head_sha"] != expected_sha:
        raise ValueError("run head SHA does not match --expected-sha")
    attempt = run["run_attempt"]
    errors = []

    def pages(endpoint, field):
        values = []
        page = 1
        while True:
            data = api(f"{endpoint}?per_page=100&page={page}")
            write_json(output / f"{field}-page-{page}.json", data)
            batch = data[field]
            values.extend(batch)
            if len(values) >= data["total_count"]:
                return values
            if not batch:
                raise ValueError(f"incomplete {field} pagination")
            page += 1

    jobs = pages(f"{base}/attempts/{attempt}/jobs", "jobs")
    for job in jobs:
        if (job["head_sha"] != run["head_sha"] or job["run_id"] != run["id"]
                or job["run_attempt"] != attempt):
            raise ValueError("job SHA/run/attempt does not match run evidence")
    artifacts = pages(f"{base}/artifacts", "artifacts")
    metrics = []
    for artifact in artifacts:
        if not artifact["name"].startswith("build-metrics-"):
            continue
        identifier = artifact["id"]
        try:
            if artifact.get("expired"):
                raise ValueError("artifact expired")
            ancestry = artifact.get("workflow_run", {})
            if ancestry.get("id") != run["id"] or ancestry.get("head_sha") != run["head_sha"]:
                raise ValueError("artifact ancestry does not match run")
            if artifact.get("size_in_bytes", 0) > MAX_METRICS_BYTES:
                raise ValueError("metrics archive exceeds size limit")
            # Every download has its own empty directory. No interpreter ever
            # runs from here and archive contents never enter the import path.
            with tempfile.TemporaryDirectory(prefix="klogg-ci-metrics-") as temporary:
                archive_path = Path(temporary) / "artifact.zip"
                with archive_path.open("wb") as stream:
                    subprocess.run(["gh", "api", f"repos/{repository}/actions/artifacts/{identifier}/zip"],
                                   stdout=stream, check=True)
                raw = archive_path.read_bytes()
                value = read_metrics_zip(raw, run_id, attempt)
            if not value.get("provenance") and attempt != 1:
                raise ValueError("legacy artifact cannot establish rerun attempt")
            write_json(output / f"metrics-{identifier}.json", value)
            metrics.append({"artifact_id": identifier, "artifact_name": artifact["name"],
                            "archive_sha256": hashlib.sha256(raw).hexdigest(), "report": value})
        except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
            errors.append(f"artifact {identifier} ({artifact['name']}): {error}")
    result = {"schema_version": 1, "repository": repository, "run": run, "jobs": jobs,
              "metrics": metrics, "errors": errors}
    write_json(output / "evidence.json", result)
    return result


def timestamp(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def seconds(start, end):
    value = timestamp(end) - timestamp(start)
    if value < 0:
        raise ValueError("negative timestamp interval")
    return value


def phase(name):
    """Conservative vocabulary: API composite steps cannot expose inner timings."""
    name = name.lower()
    if name.startswith("post "):
        return "other"
    if name in ("configure", "cmake configure"):
        return "configure"
    if name in ("build", "compile", "cmake build"):
        return "compile"
    if "actions/agent-build" in name or "actions/docker-build" in name:
        return "configure_compile"
    if "upload" in name or "actions/upload-artifact@" in name:
        return "upload"
    if "run-tests" in name or name in ("test", "tests", "ctest"):
        return "test"
    if "agent-package" in name or "docker-package" in name or name == "package":
        return "package"
    if "cache" in name:
        return "cache"
    if "prepare" in name or "setup" in name or "download-artifact" in name:
        return "prepare"
    return "other"


def cache_hit_percent(metric):
    delta = metric.get("cache_deltas", {})
    keys = ("direct_cache_hit", "preprocessed_cache_hit", "cache_miss")
    if not isinstance(delta, dict) or any(type(delta.get(key)) is not int or delta[key] < 0 for key in keys):
        return None
    hits = delta["direct_cache_hit"] + delta["preprocessed_cache_hit"]
    total = hits + delta["cache_miss"]
    return 100 * hits / total if total else None


def summarize_run(evidence, dependencies=None):
    run = evidence["run"]
    origin = run["run_started_at"]
    active = [job for job in evidence["jobs"] if job.get("conclusion") != "skipped"]
    rows = []
    by_id = {str(job["id"]): job for job in active}
    dependencies = dependencies or {}
    for job in active:
        identifier = str(job["id"])
        start, end = job.get("started_at"), job.get("completed_at")
        phases = dict.fromkeys(PHASES)
        for step in job.get("steps", []):
            if step.get("conclusion") == "skipped" or not step.get("started_at") or not step.get("completed_at"):
                continue
            name = phase(step["name"])
            phases[name] = (phases[name] or 0) + seconds(step["started_at"], step["completed_at"])
        wait = queue = None
        if start and identifier in dependencies:
            parents = dependencies[identifier]
            if any(parent not in by_id or not by_id[parent].get("completed_at") for parent in parents):
                raise ValueError("DAG references unknown/incomplete active job")
            ready = max([timestamp(origin)] + [timestamp(by_id[parent]["completed_at"]) for parent in parents])
            wait = ready - timestamp(origin)
            queue = timestamp(start) - ready
            if queue < 0:
                raise ValueError("job starts before its declared dependencies complete")
        rows.append({"id": identifier, "name": job["name"], "conclusion": job.get("conclusion"),
                     "execution_seconds": seconds(start, end) if start and end else None,
                     "pre_start_seconds": seconds(origin, start) if start else None,
                     "needs_wait_seconds": wait, "queue_after_ready_seconds": queue,
                     "phases_seconds": phases})
    critical = None
    if by_id and set(by_id) <= set(dependencies) and all(row["execution_seconds"] is not None for row in rows):
        memo, visiting = {}, set()
        durations = {row["id"]: row["execution_seconds"] for row in rows}
        def chain(identifier):
            if identifier in visiting:
                raise ValueError("dependency cycle")
            if identifier not in memo:
                visiting.add(identifier)
                previous = max((chain(parent) for parent in dependencies[identifier]),
                               default=(0, []), key=lambda item: item[0])
                memo[identifier] = (previous[0] + durations[identifier], previous[1] + [identifier])
                visiting.remove(identifier)
            return memo[identifier]
        duration, path = max((chain(identifier) for identifier in by_id), key=lambda item: item[0])
        critical = {"execution_seconds": duration, "job_ids": path,
                    "definition": "longest dependency chain of execution durations; excludes queue"}
    ends = [job["completed_at"] for job in active if job.get("completed_at")]
    return {"run_id": run["id"], "run_attempt": run["run_attempt"], "head_sha": run["head_sha"],
            "successful": run.get("conclusion") == "success" and all(row["conclusion"] == "success" for row in rows),
            "initial_queue_seconds": seconds(run["created_at"], origin),
            "observed_wall_seconds": seconds(run["created_at"], max(ends)) if ends else None,
            "api_envelope_seconds": seconds(run["created_at"], run["updated_at"]),
            "sum_job_seconds": sum(row["execution_seconds"] or 0 for row in rows),
            "critical_path": critical, "jobs": rows, "collection_errors": evidence.get("errors", []),
            "build_metrics": [{"artifact_id": item["artifact_id"], "artifact_name": item["artifact_name"],
                               "build_wall_ms": item["report"]["build_wall_ms"],
                               "object_cache": item["report"].get("object_cache"),
                               "provenance": item["report"].get("provenance"),
                               "current_run_cache_hit_percent": cache_hit_percent(item["report"])}
                              for item in evidence.get("metrics", [])]}


def compare_metrics(metrics):
    """Only same SHA, compiler, environment, architecture and config are comparable."""
    groups = {}
    keys = ("sha", "arch", "config", "environment", "build_type", "cmake_options", "compilers")
    for metric in metrics:
        provenance = metric_provenance(metric)
        identity = {key: provenance.get(key) for key in keys}
        identity["object_cache"] = metric.get("object_cache")
        identity["cache_tool"] = metric.get("cache_tool", "ccache" if metric.get("object_cache") == "measured" else None)
        key = json.dumps(identity, sort_keys=True)
        groups.setdefault(key, []).append(metric)
    result = []
    for key, members in sorted(groups.items()):
        identity = json.loads(key)
        issues = []
        options = identity["cmake_options"]
        if (any(not identity[field] for field in keys) or not isinstance(options, dict)
                or any(not isinstance(value, str) for value in options.values())):
            issues.append("missing SHA/toolchain/environment/architecture/configuration identity")
        if identity["object_cache"] == "unmeasured":
            issues.append("object cache measurement unavailable")
        values = {"cold": [], "warm": []}
        seen, hit_rates = set(), []
        for member in members:
            provenance = metric_provenance(member)
            sample = (provenance.get("run_id"), provenance.get("run_attempt"))
            if not all(sample) or sample in seen:
                issues.append("missing or duplicate run/attempt sample")
                continue
            seen.add(sample)
            state = provenance.get("cache_state", "unknown")
            wall = member.get("build_wall_ms")
            if state not in values or not valid_build_duration(wall):
                issues.append("unknown cache state or unavailable build duration")
                continue
            values[state].append(wall)
            hit_rate = cache_hit_percent(member)
            hit_rates.append(hit_rate)
            if identity["object_cache"] == "measured" and hit_rate is None:
                issues.append("measured cache requires valid current-run cache-hit evidence")
        summaries = {state: {"samples": len(samples), "p50_ms": statistics.median(samples) if samples else None,
                             "max_ms": max(samples) if samples else None} for state, samples in values.items()}
        if len(values["cold"]) < 1 or len(values["warm"]) < 3:
            issues.append("requires at least one cold and three distinct comparable warm runs")
        result.append({"identity": identity, **summaries, "complete": not issues,
                       "issues": sorted(set(issues)), "current_run_cache_hit_percent": hit_rates})
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    collector = commands.add_parser("collect")
    collector.add_argument("--repo", required=True)
    collector.add_argument("--run-id", required=True, type=int)
    collector.add_argument("--attempt", type=int)
    collector.add_argument("--expected-sha")
    collector.add_argument("--output", required=True, type=Path, help="new durable evidence directory")
    reporter = commands.add_parser("report")
    reporter.add_argument("evidence", nargs="+", type=Path, help="collected evidence.json files")
    reporter.add_argument("--dag", type=Path, help="run-ID -> job-ID -> dependency job-ID arrays")
    args = parser.parse_args(argv)
    try:
        if args.command == "collect":
            data = collect(args.repo, args.run_id, args.output, args.expected_sha, attempt=args.attempt)
            print(json.dumps({"path": str(args.output), "errors": data["errors"]}, indent=2))
            return 1 if data["errors"] else 0
        evidence = [json.loads(path.read_text(encoding="utf-8")) for path in args.evidence]
        dag = json.loads(args.dag.read_text(encoding="utf-8")) if args.dag else {}
        result = {"schema_version": 1,
                  "runs": [summarize_run(item, dag.get(str(item["run"]["id"]))) for item in evidence],
                  "build_comparisons": compare_metrics([metric["report"] for item in evidence for metric in item["metrics"]])}
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
