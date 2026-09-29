#!/usr/bin/env python3
"""Collect unreviewed iOS host diagnostics without issuing qualification evidence."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import re
import subprocess
import tempfile

from ci_dependency_toolchain import BREW_FORMULAS, ToolchainError, observe_ios_host_tools

MAX_EVIDENCE_BYTES = 256 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
SHA40 = re.compile(r"[0-9a-f]{40}\Z")
FIELDS = {"schema_version", "kind", "target_id", "host_arch", "developer_dir",
          "runner_image", "source", "tools", "homebrew", "bottles", "trace",
          "interpreters", "modules", "dylibs", "capability", "build_completed",
          "tool_observation_error"}


def _hashed_path(entry: object) -> bool:
    return (isinstance(entry, dict)
            and isinstance(entry.get("resolved_path"), str)
            and pathlib.PurePath(entry["resolved_path"]).is_absolute()
            and isinstance(entry.get("sha256"), str)
            and SHA256.fullmatch(entry["sha256"]) is not None)


def evaluate_observation(report: dict) -> dict:
    """Describe absent proof; diagnostics cannot confer native qualification."""
    if not isinstance(report, dict):
        raise ValueError("iOS host observation must be an object")
    missing = []
    if set(report) - FIELDS:
        missing.append("unknown observation fields")
    trace = report.get("trace")
    if (not isinstance(trace, dict) or trace.get("available") is not True
            or trace.get("truncated") is not False or trace.get("error")):
        missing.append("complete process trace")
    executions = trace.get("executions") if isinstance(trace, dict) else None
    if (not isinstance(executions, list) or not executions
            or any(not _hashed_path(item) or not isinstance(item.get("argv"), list)
                   for item in executions)):
        missing.append("resolved executed binary bytes")
    for field in ("interpreters", "modules", "dylibs"):
        entries = report.get(field)
        if not isinstance(entries, list) or not entries or any(
                not _hashed_path(item) for item in entries):
            missing.append("resolved " + field + " bytes")
    bottles = report.get("bottles")
    if (not isinstance(bottles, list) or not bottles
            or any(not isinstance(item, dict)
                   or not isinstance(item.get("formula"), str)
                   or not isinstance(item.get("dependencies"), list)
                   or item.get("archive_verified") is not True
                   or not isinstance(item.get("sha256"), str)
                   or SHA256.fullmatch(item["sha256"]) is None
                   for item in bottles)):
        missing.append("verified bottle and recursive dependencies")
    tools = report.get("tools")
    if (not isinstance(tools, dict) or len(tools) < 8
            or any(not _hashed_path(item) for item in tools.values())):
        missing.append("selected host tool bytes")
    if report.get("build_completed") is not True:
        missing.append("observed real native build")
    # Even an apparently complete self-report cannot replace independent review
    # and same-attempt verification by the (currently blocking) native Gate.
    missing.append("independent host-input review and Gate verification")
    return {"complete": False, "missing": missing}


def _dtrace_capability() -> dict:
    """Try a bounded non-SIP child; success does not trace a later build."""
    try:
        with tempfile.TemporaryDirectory(prefix="ios-host-exec-probe-") as directory:
            root = pathlib.Path(directory)
            source = root / "probe.c"
            binary = root / "probe"
            source.write_text("int main(void) { return 0; }\n", encoding="ascii")
            built = subprocess.run(["clang", "-x", "c", str(source), "-o", str(binary)],
                                   capture_output=True, text=True, timeout=15, check=False)
            if built.returncode:
                return {"dtrace_exec_probe": False, "reason": "probe compilation failed"}
            command = ["sudo", "-n", "/usr/sbin/dtrace", "-q", "-n",
                       'proc:::exec-success { printf("%d\\n", pid); }',
                       "-c", str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=15,
                                    check=False)
    except (OSError, subprocess.SubprocessError):
        return {"dtrace_exec_probe": False, "reason": "probe could not execute"}
    available = result.returncode == 0 and any(
        line.strip().isdigit() for line in result.stdout.splitlines())
    return {"dtrace_exec_probe": available,
            "reason": "exec event not captured" if not available else "captured unprotected exec"}


def observe_brew_metadata() -> list[dict]:
    """Record published bottle metadata, not proof of the poured archive bytes."""
    environment = {**os.environ, "HOMEBREW_NO_AUTO_UPDATE": "1",
                   "HOMEBREW_NO_ANALYTICS": "1"}
    try:
        result = subprocess.run(["brew", "info", "--json=v2", *BREW_FORMULAS],
                                capture_output=True, text=True, timeout=30,
                                check=False, env=environment)
        if result.returncode or len(result.stdout) > 4 * 1024 * 1024:
            return []
        data = json.loads(result.stdout)
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    if not isinstance(data, dict) or not isinstance(data.get("formulae"), list):
        return []
    observed = []
    for formula in data["formulae"]:
        if not isinstance(formula, dict) or formula.get("name") not in BREW_FORMULAS:
            continue
        installed = formula.get("installed")
        bottle = formula.get("bottle")
        stable = bottle.get("stable") if isinstance(bottle, dict) else None
        files = stable.get("files") if isinstance(stable, dict) else None
        published = {tag: record["sha256"] for tag, record in files.items()
                     if isinstance(tag, str) and isinstance(record, dict)
                     and isinstance(record.get("sha256"), str)
                     and SHA256.fullmatch(record["sha256"])} if isinstance(files, dict) else {}
        dependencies = []
        for field in ("dependencies", "build_dependencies"):
            values = formula.get(field)
            if isinstance(values, list):
                dependencies.extend(value for value in values if isinstance(value, str))
        observed.append({"formula": formula["name"],
                         "installed_versions": [record.get("version") for record in installed
                                                if isinstance(record, dict)]
                         if isinstance(installed, list) else [],
                         "dependencies": sorted(set(dependencies)),
                         "published_bottle_sha256": published,
                         "sha256": None, "archive_verified": False})
    return sorted(observed, key=lambda item: item["formula"])


def _write_diagnostic(path: pathlib.Path, report: dict) -> None:
    encoded = (json.dumps(report, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")
    if len(encoded) > MAX_EVIDENCE_BYTES:
        raise ValueError("iOS host diagnostic exceeds size limit")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-id", required=True, choices=("ios-x86_64", "ios-arm64"))
    parser.add_argument("--output", required=True, type=pathlib.Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--stub", action="store_true")
    mode.add_argument("--preflight", action="store_true")
    mode.add_argument("--post-build", action="store_true")
    args = parser.parse_args(argv)
    source_sha = os.environ.get("GITHUB_SHA", "")
    expected_sha = os.environ.get("KLOGG_EXPECTED_SOURCE_SHA", "")
    if (not SHA40.fullmatch(source_sha) or source_sha == "0" * 40
            or source_sha != expected_sha
            or os.environ.get("GITHUB_REPOSITORY") != "ZEACENT/klogg"):
        parser.error("diagnostic source identity is missing or changed")
    report = {"schema_version": 1, "kind": "unreviewed-ios-host-diagnostic",
              "target_id": args.target_id, "host_arch": platform.machine(),
              "developer_dir": os.environ.get("DEVELOPER_DIR"),
              "runner_image": {"os": os.environ.get("ImageOS"),
                               "version": os.environ.get("ImageVersion")},
              "source": {"sha": source_sha, "run_id": os.environ.get("GITHUB_RUN_ID"),
                         "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT")},
              "build_completed": args.post_build,
              "trace": {"available": False, "truncated": False,
                        "error": "the native build execution chain was not captured"},
              "interpreters": [], "modules": [], "dylibs": [], "bottles": []}
    if args.stub:
        report.update({"tools": {}, "homebrew": {},
                       "tool_observation_error": "host setup did not finish",
                       "capability": {"dtrace_exec_probe": False, "reason": "not tested"}})
        report.update(evaluate_observation(report))
        _write_diagnostic(args.output, report)
        return 0
    try:
        selected = observe_ios_host_tools(
            target_id=args.target_id, host_arch=platform.machine(),
            developer_dir=os.environ.get("DEVELOPER_DIR", ""))
        report["tools"] = selected["tools"]
        report["homebrew"] = selected["homebrew"]
    except ToolchainError as error:
        report["tools"] = {}
        report["homebrew"] = {}
        report["tool_observation_error"] = str(error)
    report["bottles"] = observe_brew_metadata()
    report["capability"] = _dtrace_capability()
    report.update(evaluate_observation(report))
    _write_diagnostic(args.output, report)
    # A live DTrace probe permits an evidence-only build, but never qualifies
    # its untraced execution or permits the ordinary native preflight to pass.
    return 0 if (args.preflight and "tool_observation_error" not in report
                 and report["capability"]["dtrace_exec_probe"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
