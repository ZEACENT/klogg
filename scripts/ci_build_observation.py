#!/usr/bin/env python3
"""Enrich optional CI metrics without modifying qualification-bound build inputs.

The legacy metrics implementation remains byte-for-byte policy compatible. This
wrapper reuses its build execution, Ninja analysis and cache delta logic. Linux
calls bind-environment on the host after the locked container action completes.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import ci_build_metrics as legacy

MetricsError = legacy.MetricsError


def build_provenance(build_root, environment=None):
    """Read generated compiler files as data, never execute CMake to inspect them."""
    env = os.environ if environment is None else environment
    result = {"sha": env.get("GITHUB_SHA"), "run_id": env.get("GITHUB_RUN_ID"),
              "run_attempt": env.get("GITHUB_RUN_ATTEMPT"), "arch": env.get("KLOGG_CONFIG_ARCH"),
              "config": "/".join(filter(None, (env.get("KLOGG_LABEL"), env.get("KLOGG_CONFIG_PACKAGE_TAG")))),
              "cache_state": env.get("KLOGG_CI_CACHE_STATE", "unknown"),
              "environment": env.get("KLOGG_CI_ENVIRONMENT_IDENTITY") or
                             "/".join(filter(None, (env.get("ImageOS"), env.get("ImageVersion")))),
              "cmake_options": {}, "build_type": None, "compilers": {}}
    for language in ("C", "CXX"):
        paths = list(Path(build_root).glob(f"CMakeFiles/*/CMake{language}Compiler.cmake"))
        if len(paths) != 1:
            continue
        text = paths[0].read_text(encoding="utf-8")
        identity = {}
        for field, suffix in (("id", "ID"), ("version", "VERSION"), ("target", "TARGET"),
                              ("architecture", "ARCHITECTURE_ID")):
            match = re.search(r'^set\(CMAKE_' + language + r'_COMPILER_' + suffix + r' "([^"\n]+)"\)$', text, re.M)
            if match:
                identity[field] = match[1]
        if "id" in identity and "version" in identity:
            result["compilers"][language] = identity
    cache = Path(build_root) / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            match = re.fullmatch(r"([^:#/][^:]*)\:(BOOL|STRING|PATH|FILEPATH)=(.*)", line)
            if not match:
                continue
            key, kind, value = match.groups()
            if key == "CMAKE_BUILD_TYPE":
                result["build_type"] = value
            if kind == "BOOL" or re.fullmatch(r"CMAKE_(?:C|CXX)_FLAGS(?:_.*)?|CMAKE_OSX_.*|CMAKE_BUILD_TYPE", key):
                result["cmake_options"][key] = value
    return result


def replace_report(path, report):
    """Atomic enrichment; failure leaves the original successful-build report intact."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix="." + path.name, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write("\n")
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def bind_environment(metrics, identity, environment=None):
    """Bind host context to this attempt's file; preserve compiler-measured identity."""
    path = Path(metrics)
    if path.is_symlink():
        raise MetricsError("refusing metrics symlink")
    if not path.is_file():
        print("notice: no optional build metrics to bind", file=sys.stderr)
        return None
    context = build_provenance(path.parent, environment)
    if not re.fullmatch(r"[0-9a-fA-F]{40}", context["sha"] or ""):
        raise MetricsError("binding requires a full GITHUB_SHA")
    for field in ("run_id", "run_attempt"):
        if not re.fullmatch(r"[1-9][0-9]*", context[field] or ""):
            raise MetricsError("binding requires " + field)
    expected = f"ci-build-metrics-{context['run_id']}-{context['run_attempt']}.json"
    if path.name != expected:
        raise MetricsError("metrics filename/run mismatch")
    report = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(report.get("build_wall_ms"), (int, float)):
        raise MetricsError("missing build wall evidence")
    previous = report.get("provenance", {})
    for key in ("sha", "run_id", "run_attempt", "arch", "config"):
        if previous.get(key) and context.get(key) and str(previous[key]) != str(context[key]):
            raise MetricsError("metrics provenance mismatch: " + key)
    for key in ("compilers", "cmake_options", "build_type"):
        if previous.get(key):
            context[key] = previous[key]
    context["environment"] = identity
    report["provenance"] = context
    replace_report(path, report)
    return report


def sccache_adapter(runner):
    """Adapt sccache counters to the existing, tested ccache delta protocol."""
    previous_requests = None
    def run(command, **kwargs):
        nonlocal previous_requests
        if command != ["ccache", "--print-stats"]:
            return runner(command, **kwargs)
        result = runner(["sccache", "--show-stats", "--stats-format=json"], **kwargs)
        stats = json.loads(result.stdout)["stats"]
        requests = stats["compile_requests"]
        values = {"direct_cache_hit": sum(stats["cache_hits"]["counts"].values()),
                  "preprocessed_cache_hit": 0, "cache_miss": sum(stats["cache_misses"]["counts"].values()),
                  "stats_zeroed_timestamp": int(previous_requests is not None and requests < previous_requests)}
        if not isinstance(requests, int) or requests < 0 or any(not isinstance(value, int) or value < 0 for value in values.values()):
            raise MetricsError("invalid sccache counters")
        previous_requests = requests
        result.stdout = "".join(f"{key}\t{value}\n" for key, value in values.items())
        return result
    return run


def run_build(command, *, repo_root, build_root, output, object_cache, runner=None, clock=None,
              provenance=None, cache_tool="ccache"):
    runner = subprocess.run if runner is None else runner
    if cache_tool not in ("ccache", "sccache"):
        raise MetricsError("unknown cache tool")
    report = legacy.run_build(command, repo_root=repo_root, build_root=build_root, output=output,
                              object_cache=object_cache, runner=sccache_adapter(runner) if cache_tool == "sccache" else runner,
                              clock=clock)
    if report is None:
        return None
    try:
        context = build_provenance(build_root)
        filename = re.fullmatch(r"ci-build-metrics-([0-9]+)-([0-9]+)\.json", Path(output).name)
        if filename:
            context["run_id"], context["run_attempt"] = filename.groups()
        context.update(provenance or {})
        report["provenance"] = context
        report["cache_tool"] = cache_tool if object_cache == "measured" else None
        if cache_tool == "sccache":
            report["cache_counter_scope"] = "exclusive server; no reset/restart during build"
        replace_report(Path(output), report)
    except Exception as error:
        print(f"notice: optional build observation unavailable: {error}", file=sys.stderr)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    build = commands.add_parser("run-build")
    for name in ("repo-root", "build-root", "output"):
        build.add_argument("--" + name, required=True)
    build.add_argument("--object-cache", choices=("measured", "disabled", "unmeasured"), default="measured")
    build.add_argument("--cache-tool", choices=("ccache", "sccache"), default="ccache")
    build.add_argument("command", nargs=argparse.REMAINDER)
    bind = commands.add_parser("bind-environment")
    bind.add_argument("--metrics", required=True, type=Path)
    bind.add_argument("--identity", required=True)
    args = parser.parse_args(argv)
    try:
        if args.mode == "bind-environment":
            bind_environment(args.metrics, args.identity)
        else:
            command = args.command[1:] if args.command[:1] == ["--"] else args.command
            if not command:
                parser.error("build command required after --")
            run_build(command, repo_root=args.repo_root, build_root=args.build_root, output=args.output,
                      object_cache=args.object_cache, cache_tool=args.cache_tool)
        return 0
    except subprocess.CalledProcessError as error:
        return error.returncode or 1
    except (OSError, ValueError, MetricsError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
