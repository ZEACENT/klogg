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
import time

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import ci_build_metrics as legacy

MetricsError = legacy.MetricsError


def build_provenance(build_root, environment=None):
    """Read generated compiler files as data, never execute CMake to inspect them."""
    env = os.environ if environment is None else environment
    artifact_id = env.get("KLOGG_CI_CACHE_ARTIFACT_ID") or None
    legacy_key = env.get("KLOGG_CI_CACHE_RESTORE_KEY") or None
    result = {"sha": env.get("GITHUB_SHA"), "run_id": env.get("GITHUB_RUN_ID"),
              "run_attempt": env.get("GITHUB_RUN_ATTEMPT"), "arch": env.get("KLOGG_CONFIG_ARCH"),
              "config": "/".join(filter(None, (env.get("KLOGG_LABEL"), env.get("KLOGG_CONFIG_PACKAGE_TAG")))),
              "cache_state": env.get("KLOGG_CI_CACHE_STATE", "unknown"),
              "cache_restore_key": None if artifact_id else legacy_key,
              "cache_artifact_id": artifact_id,
              "cache_transport": "verified-artifact" if artifact_id else "actions-cache" if legacy_key else "none",
              "superseded_cache_restore_key": legacy_key if artifact_id else None,
              "cache_compiler_check": env.get("CCACHE_COMPILERCHECK"),
              "environment": env.get("KLOGG_CI_ENVIRONMENT_IDENTITY") or
                             "/".join(filter(None, (env.get("ImageOS"), env.get("ImageVersion")))),
              "cmake_options": {}, "build_type": None, "compilers": {}, "source_root": None}
    acquisition_path = env.get("KLOGG_CI_CACHE_ACQUISITION_REPORT")
    if acquisition_path:
        path = Path(acquisition_path)
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
            raise MetricsError("missing or oversized cache acquisition report")
        acquisition = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(acquisition, dict) or not isinstance(acquisition.get("consumer"), dict):
            raise MetricsError("invalid cache acquisition report")
        for field, variable in (("run_id", "GITHUB_RUN_ID"), ("run_attempt", "GITHUB_RUN_ATTEMPT"),
                                ("checkout_sha", "GITHUB_SHA")):
            if not env.get(variable) or str(acquisition["consumer"].get(field)) != env[variable]:
                raise MetricsError("cache acquisition consumer identity mismatch: " + field)
        # Acquisition is not a measured hit rate. A bridge miss may coexist with
        # useful backend objects, and imported objects still face compiler checks.
        result["cache_acquisition"] = acquisition
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
            if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
                result["source_root"] = line.split("=", 1)[1]
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
    for key in ("compilers", "cmake_options", "build_type", "source_root"):
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


def start_diagnostics(build_root, mode):
    # Imported only on explicit opt-in; qualification-bound execution stays intact.
    from ci_execution_diagnostics import start
    return start(build_root, mode)


def run_build(command, *, repo_root, build_root, output, object_cache, runner=None, clock=None,
              provenance=None, cache_tool="ccache", execution_diagnostics=None):
    runner = subprocess.run if runner is None else runner
    if cache_tool not in ("ccache", "sccache"):
        raise MetricsError("unknown cache tool")
    diagnostics = None
    if execution_diagnostics is not None:
        try:
            diagnostics = start_diagnostics(build_root, execution_diagnostics)
        except Exception:
            print("notice: optional execution diagnostics could not start", file=sys.stderr)
    try:
        report = legacy.run_build(command, repo_root=repo_root, build_root=build_root, output=output,
                                  object_cache=object_cache, runner=sccache_adapter(runner) if cache_tool == "sccache" else runner,
                                  clock=clock)
    finally:
        if diagnostics is not None:
            try:
                diagnostics.stop()
            except Exception:
                print("notice: optional execution diagnostics could not stop", file=sys.stderr)
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


def aggregate_phases(prepare, finalize):
    """Combine build invocation durations, never unrelated Ninja time origins."""
    for name, report in (("prepare", prepare), ("finalize", finalize)):
        if (not isinstance(report, dict) or report.get("measurement_scope") != "phase"
                or report.get("phase") != name or type(report.get("build_wall_ms")) is not int
                or report["build_wall_ms"] < 0):
            raise ValueError("missing or invalid native build phase: " + name)
    before, after = prepare.get("provenance"), finalize.get("provenance")
    required = ("sha", "run_id", "run_attempt", "arch", "config", "environment", "source_root", "build_type")
    for provenance in (before, after):
        if not isinstance(provenance, dict) or any(
                not isinstance(provenance.get(key), str) or not provenance[key] for key in required):
            raise ValueError("native phase lacks complete source/run/toolchain identity")
        if (not re.fullmatch(r"[0-9a-f]{40}", provenance["sha"])
                or any(not re.fullmatch(r"[1-9][0-9]*", provenance[key]) for key in ("run_id", "run_attempt"))):
            raise ValueError("invalid native phase run/SHA identity")
        compilers = provenance.get("compilers")
        if (not isinstance(compilers, dict) or not compilers
                or any(not isinstance(compiler, dict) or not compiler.get("id") or not compiler.get("version")
                       for compiler in compilers.values())):
            raise ValueError("native phase lacks compiler identity")
    if ({key: value for key, value in before.items() if key != "cmake_options"}
            != {key: value for key, value in after.items() if key != "cmake_options"}):
        raise ValueError("native phase provenance mismatch")
    from ci_native_rendezvous import profile
    platform = before.get("native_platform", "macOS")
    # This selects platform-wide gates only; the orchestrator/rendezvous bind
    # the actual architecture. Aggregation checks its provenance for equality.
    gates = profile(platform, "x86_64")["gates"]
    options_before, options_after = before.get("cmake_options"), after.get("cmake_options")
    if (not isinstance(options_before, dict) or not isinstance(options_after, dict)
            or any(options_before.get(key) != "OFF" or options_after.get(key) != "ON" for key in gates)
            or (platform == "Windows" and options_before.get("KLOGG_ENABLE_IOS_NATIVE_STACK") != "OFF")
            or {key: value for key, value in options_before.items() if key not in gates}
            != {key: value for key, value in options_after.items() if key not in gates}):
        raise ValueError("unexpected native phase CMake option difference")
    cache_tool = prepare.get("cache_tool")
    measured = (prepare.get("object_cache") == finalize.get("object_cache") == "measured"
                and cache_tool == finalize.get("cache_tool") and cache_tool in ("ccache", "sccache"))
    disabled = prepare.get("object_cache") == finalize.get("object_cache") == "disabled"
    counters = (prepare.get("cache_deltas"), finalize.get("cache_deltas"))
    valid_counters = measured and all(
        isinstance(counter, dict) and all(type(counter.get(key)) is int and counter[key] >= 0
                                         for key in legacy.CACHE_COUNTERS)
        for counter in counters)
    return {"schema_version": 1, "measurement_scope": "phased-build",
            "build_wall_ms": prepare["build_wall_ms"] + finalize["build_wall_ms"],
            "build_wall_definition": "sum of prepare/finalize build invocations; excludes configure, native wait, tests and packaging",
            "object_cache": "measured" if valid_counters else "disabled" if disabled else "unmeasured",
            "cache_tool": cache_tool if valid_counters else None,
            "cache_deltas": {key: sum(counter[key] for counter in counters) for key in legacy.CACHE_COUNTERS}
                            if valid_counters else None,
            "provenance": after, "phases": {"prepare": prepare, "finalize": finalize}}


def publish_phased_build(build_root, expected):
    """Publish optional complete evidence only after final native build success."""
    build = Path(build_root)
    try:
        prefix = f"{expected['run_id']}-{expected['run_attempt']}"
        phases = [json.loads((build / f"ci-build-phase-{prefix}-{phase}.json").read_text(encoding="utf-8"))
                  for phase in ("prepare", "finalize")]
        report = aggregate_phases(*phases)
        for key in ("sha", "run_id", "run_attempt"):
            if str(report["provenance"][key]) != str(expected[key]):
                raise ValueError("phased build is not the current invocation: " + key)
        output = build / f"ci-build-metrics-{prefix}.json"
        if output.exists() or output.is_symlink():
            raise ValueError("whole phased-build observation already exists")
        replace_report(output, report)
        print("KLOGG_BUILD_METRICS_JSON=" + json.dumps(report, sort_keys=True), flush=True)
        return report
    except (OSError, ValueError, KeyError) as error:
        print(f"notice: optional whole phased-build observation unavailable: {error}", file=sys.stderr)
        return None


def phase_log_delta(before, after):
    """An append-only Ninja log proves phase-local work; compaction does not.

    Never truncate .ninja_log to simplify measurement: Ninja needs its command
    hashes to retain prepared objects across the second configure.
    """
    if not before:
        return after
    if not after.startswith(before):
        return None
    return before.splitlines()[0] + "\n" + after[len(before):]


def run_phase(command, *, repo_root, build_root, output, phase, object_cache="measured",
              runner=None, clock=None, provenance=None, configure=None, cache_tool="ccache"):
    if phase not in ("prepare", "finalize"):
        raise ValueError("unknown native build phase")
    if cache_tool not in ("ccache", "sccache") or object_cache not in ("measured", "disabled", "unmeasured"):
        raise ValueError("unknown native phase cache observation")
    runner = subprocess.run if runner is None else runner
    counter_runner = sccache_adapter(runner) if cache_tool == "sccache" else runner
    # Windows Python before 3.13 uses coarse GetTickCount64 for monotonic;
    # duration measurement needs perf_counter's high-resolution clock instead.
    clock = time.perf_counter if clock is None else clock
    build, path = Path(build_root), Path(output)
    if path.exists() or path.is_symlink():
        raise MetricsError("phase observation already exists")
    log = build / ".ninja_log"
    before = log.read_text(encoding="utf-8") if log.exists() else ""
    counters = None
    if object_cache == "measured":
        try:
            counters = legacy.ccache_counters(counter_runner)
        except Exception as error:
            print(f"notice: phase cache counters unavailable: {error}", file=sys.stderr)
    started = clock()
    try:
        runner(command, check=True)
    finally:
        wall = max(0, round((clock() - started) * 1000))
        # Preserve raw command-hash evidence, including on failed builds.
        try:
            after = log.read_text(encoding="utf-8")
            path.with_suffix(".ninja-log.txt").write_text(after, encoding="utf-8")
        except OSError as error:
            after = ""
            print(f"notice: phase raw Ninja log unavailable: {error}", file=sys.stderr)
    try:
        report = {"schema_version": 1, "measurement_scope": "phase", "phase": phase,
                  "build_wall_ms": wall, "object_cache": object_cache,
                  "cache_tool": cache_tool if object_cache == "measured" else None,
                  "provenance": build_provenance(build), "cache_deltas": None,
                  "ninja_log": path.with_suffix(".ninja-log.txt").name,
                  "compilation_state": "unknown"}
        report["provenance"].update(provenance or {})
        if configure is not None:
            report["configure"] = configure
        delta = phase_log_delta(before, after)
        if delta:
            try:
                database = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
                measured = legacy.measure(delta, database, str(repo_root), str(build), wall, object_cache)
                report.update(measured)
                report["compilation_state"] = "measured"
            except Exception as error:
                # Ninja uses an independent clock and log lifecycle. Invalid
                # analysis must not erase the successful invocation's evidence
                # or inflate its authoritative wall time to match Ninja.
                report["compilation_unknown_reason"] = str(error)
                print(f"notice: optional phase Ninja analysis unavailable: {error}", file=sys.stderr)
        else:
            report["compilation_unknown_reason"] = "Ninja log unavailable or compacted across phase boundary"
        if counters is not None:
            try:
                report["cache_deltas"] = legacy.cache_deltas(counters, legacy.ccache_counters(counter_runner))
            except Exception as error:
                print(f"notice: final phase cache counters unavailable: {error}", file=sys.stderr)
        if object_cache == "measured" and report["cache_deltas"] is None:
            report.update(object_cache="unmeasured", cache_tool=None)
        if report["cache_tool"] == "sccache":
            report["cache_counter_scope"] = "exclusive server; no reset/restart during phase"
        replace_report(path, report)
        return report
    except Exception as error:
        print(f"notice: optional phase observation unavailable: {error}", file=sys.stderr)
        return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    build = commands.add_parser("run-build")
    for name in ("repo-root", "build-root", "output"):
        build.add_argument("--" + name, required=True)
    build.add_argument("--object-cache", choices=("measured", "disabled", "unmeasured"), default="measured")
    build.add_argument("--cache-tool", choices=("ccache", "sccache"), default="ccache")
    build.add_argument("--execution-diagnostics", choices=("macos-tsan",), default=None)
    build.add_argument("command", nargs=argparse.REMAINDER)
    bind = commands.add_parser("bind-environment")
    bind.add_argument("--metrics", required=True, type=Path)
    bind.add_argument("--identity", required=True)
    args = parser.parse_args(argv)
    try:
        if args.mode == "bind-environment":
            report = bind_environment(args.metrics, args.identity)
        else:
            command = args.command[1:] if args.command[:1] == ["--"] else args.command
            if not command:
                parser.error("build command required after --")
            report = run_build(command, repo_root=args.repo_root, build_root=args.build_root, output=args.output,
                               object_cache=args.object_cache, cache_tool=args.cache_tool,
                               execution_diagnostics=args.execution_diagnostics)
        if report is not None:
            # Preserve current-run evidence even when optional artifact transport fails.
            print("KLOGG_BUILD_METRICS_JSON=" + json.dumps(report, sort_keys=True))
        return 0
    except subprocess.CalledProcessError as error:
        return error.returncode or 1
    except (OSError, ValueError, MetricsError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
