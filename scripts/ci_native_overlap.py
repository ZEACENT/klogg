#!/usr/bin/env python3
"""Prepare unpublishable native objects, then finalize the same Ninja build tree."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

import ci_build_metrics as metrics
import ci_build_observation as observation
import ci_native_rendezvous as rendezvous

NATIVE_OPTIONS = {"KLOGG_ADB_HELPER_REQUIRED", "KLOGG_ENABLE_IOS_NATIVE_STACK",
                  "KLOGG_ADB_HELPER_ARTIFACT_ROOT", "KLOGG_IOS_NATIVE_STACK_ROOT",
                  "KLOGG_ADB_HELPER_TARGET"}
STATE = ".klogg-native-overlap.json"
CONFIGURE_STRATEGY = "one-identical-precompile-stabilization-v1"


def parse_options(value, *, windows=False):
    lexer = shlex.shlex(value, posix=True)
    lexer.whitespace_split = True
    lexer.commenters = ""
    if windows:
        lexer.escape = ""
    return list(lexer)


def option_key(argument):
    if argument.startswith("-D") and "=" in argument:
        return argument[2:].split("=", 1)[0].split(":", 1)[0]
    return None


def common_options(options):
    return [argument for argument in options if option_key(argument) not in NATIVE_OPTIONS]


def validate_options(options, phase, *, platform="macOS"):
    values = {}
    for argument in options:
        key = option_key(argument)
        if key in NATIVE_OPTIONS:
            if key in values:
                raise ValueError("duplicate native CMake option: " + key)
            values[key] = argument.split("=", 1)[1]
    contract = rendezvous.profile(platform, "x86_64")
    gates = contract["gates"]
    if platform == "Windows" and values.get("KLOGG_ENABLE_IOS_NATIVE_STACK", "OFF") != "OFF":
        raise ValueError("Windows native overlap must keep the iOS gate disabled")
    if phase == "prepare":
        if any(values.get(key, "OFF") != "OFF" for key in gates):
            raise ValueError("prepare must disable native artifact requirements")
    elif phase == "finalize":
        if any(values.get(key) != "ON" for key in gates):
            raise ValueError("native artifact gates are required for finalization")
        roots = ["KLOGG_ADB_HELPER_ARTIFACT_ROOT"]
        if platform == "macOS":
            roots.append("KLOGG_IOS_NATIVE_STACK_ROOT")
        elif values.get("KLOGG_ADB_HELPER_TARGET") != contract["target"]:
            raise ValueError("Windows native helper target mismatch")
        if any(not values.get(key) for key in roots):
            raise ValueError("verified native artifact roots are required for finalization")
    else:
        raise ValueError("unknown native build phase")


def ninja_escape(value):
    if any(character in value for character in "\r\n\x00"):
        raise ValueError("newline/NUL in Ninja path")
    return value.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def object_targets(build, reachable, *, windows=False):
    database = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    if not isinstance(database, list):
        raise ValueError("compile database must be an array")
    outputs = set()
    for entry in database:
        if not isinstance(entry, dict):
            raise ValueError("invalid compile database entry")
        output = metrics.command_output(entry, windows)
        if not output:
            raise ValueError("compile database entry has no unambiguous object output")
        if windows:
            output = output.replace("\\", "/")
        path = Path(entry.get("directory", str(build))) / output
        try:
            relative = path.resolve().relative_to(build.resolve()).as_posix()
        except ValueError as error:
            raise ValueError("object output is outside build root") from error
        # These application-only resources are rewritten at configure time.
        # Defer their objects to Windows finalization rather than compile twice.
        source = str(entry.get("file", "")).replace("\\", "/").rsplit("/", 1)[-1]
        deferred = windows and source in ("qrc_app_translations.cpp", "qrc_qt_translations.cpp")
        if relative in reachable and not deferred:
            outputs.add(relative)
    if not outputs:
        raise ValueError("no reachable compilation objects for ci_build")
    return sorted(outputs)


def object_command(build, *, windows=False):
    # Ninja owns graph reachability; the compile database owns object identity.
    # The wrapper adds one phony target without altering any compiler command or
    # deleting .ninja_log/.ninja_deps, which would force finalization to rebuild.
    result = subprocess.run(["ninja", "-t", "inputs", "ci_build"], cwd=build,
                            check=True, capture_output=True, text=True)
    inputs = shlex.split(result.stdout, posix=not windows)
    if windows:
        inputs = [word[1:-1] if len(word) >= 2 and word[0] in "\"'" and word[-1] == word[0]
                  else word for word in inputs]
        inputs = [word.replace("\\", "/") for word in inputs]
    reachable = set(inputs)
    targets = object_targets(build, reachable, windows=windows)
    # Keep expensive test links inside the overlap window as well. The existing
    # aggregate is the project's authoritative C++ test build scope. Never run
    # the tests until final native artifact verification has completed.
    listed = subprocess.run(["ninja", "-t", "targets", "all"], cwd=build,
                            check=True, capture_output=True, text=True)
    available = {line.rsplit(": ", 1)[0] for line in listed.stdout.splitlines()}
    test_aggregates = ["klogg_test_build"] if "klogg_test_build" in available else [
        name for name in available if Path(name).name == "klogg_test_build"]
    for target in test_aggregates:
        dependencies = subprocess.run(["ninja", "-t", "graph", target], cwd=build,
                                      check=True, capture_output=True, text=True)
        # Ninja 1.12 inputs omits some direct executable inputs of phony nodes;
        # its graph output includes the entire closure on both pinned/new Ninja.
        if (".app/Contents/MacOS/" in dependencies.stdout
                or (windows and re.search(r'label="(?:[^"\n]*[/\\])?(?:klogg|klogg_portable|klogg_grep)\.exe"',
                                          dependencies.stdout))):
            raise ValueError("test preparation unexpectedly depends on a package app link")
    targets += sorted(test_aggregates)
    wrapper = build / ".klogg-prepare.ninja"
    wrapper.write_text("include build.ninja\nbuild klogg_prepare_objects: phony "
                       + " ".join(ninja_escape(target) for target in targets) + "\n", encoding="utf-8")
    command = ["ninja", "-C", str(build), "-f", wrapper.name]
    parallel = os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL")
    if parallel:
        if not parallel.isdecimal() or int(parallel) < 1:
            raise ValueError("native preparation requires a positive bounded parallel level")
        command += ["-j", parallel]
    return command + ["klogg_prepare_objects"]


def compile_commands(build, *, windows=False):
    database = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    return {metrics.command_output(entry, windows): entry.get("arguments", entry.get("command"))
            for entry in database}


def execute(phase, repo, build, options, receipt=None):
    repo, build = repo.resolve(), build.resolve()
    if build == repo or repo not in build.parents:
        raise ValueError("native build root must be strictly inside the checkout")
    platform = os.environ.get("RUNNER_OS")
    windows = platform == "Windows"
    architecture = "x86_64" if windows else os.environ.get("KLOGG_IOS_ARCHITECTURE", "")
    contract = rendezvous.profile(platform, architecture)
    if windows and (os.environ.get("KLOGG_CONFIG_ARCH") != "x64"
                    or os.environ.get("KLOGG_ADB_HELPER_TARGET") != contract["target"]):
        raise ValueError("Windows native overlap requires the x64 ADB package profile")
    current = rendezvous.identity()
    if phase == "prepare":
        # The final options may already name not-yet-downloaded artifacts. Remove
        # only these options; compiler, optimization and toolchain inputs stay identical.
        options = common_options(options) + ["-DKLOGG_ADB_HELPER_REQUIRED=OFF",
                                            "-DKLOGG_ENABLE_IOS_NATIVE_STACK=OFF"]
        validate_options(options, phase, platform=platform)
        if build.exists():
            shutil.rmtree(build)
        build.mkdir(parents=True)
        state = {"schema_version": 1, "identity": current, "repo": str(repo), "build": str(build),
                 "common_options": common_options(options), "status": "preparing",
                 "configure_strategy": CONFIGURE_STRATEGY, "native_profile": contract}
    else:
        validate_options(options, phase, platform=platform)
        state = json.loads((build / STATE).read_text(encoding="utf-8"))
        if (state.get("status") != "prepared" or state.get("identity") != current
                or state.get("repo") != str(repo) or state.get("build") != str(build)
                or state.get("common_options") != common_options(options)
                or state.get("configure_strategy") != CONFIGURE_STRATEGY
                or state.get("native_profile") != contract):
            raise ValueError("native finalization does not match the prepared build")
        if receipt is None:
            raise ValueError("authenticated native rendezvous receipt is required")
        verified = json.loads(receipt.read_text(encoding="utf-8"))
        rendezvous.validate_receipt(verified, current, architecture, platform=platform)
        previous_commands = compile_commands(build, windows=windows)
        state["status"] = "finalizing"
    observation.replace_report(build / STATE, state)
    configure_command = ["cmake", "-S", str(repo), "-B", str(build), *options,
                         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DCPM_SOURCE_CACHE=" + str(repo / "cpm_cache"),
                         "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"]
    if windows:
        # Match the existing full-build MSYS sh path conversion without eval.
        # In particular /usr and native drive paths retain their prior meaning.
        configure_command = ["sh", "-c", 'exec "$@"', "klogg-native-configure", *configure_command]
    configure_passes = []
    def configure_once(purpose):
        started = time.monotonic_ns()
        subprocess.run(configure_command, check=True)
        configure_passes.append({"purpose": purpose, "wall_ms": (time.monotonic_ns() - started) // 1000000,
                                 "command_sha256": hashlib.sha256(json.dumps(configure_command).encode()).hexdigest()})
    configure_once("initial" if phase == "prepare" else "native-finalization")
    if phase == "prepare":
        # Some pinned dependencies generate a feature header before a later
        # subdirectory populates its cache checks. Stabilize exactly once BEFORE
        # compilation, so finalization does not rebuild unchanged core/test code.
        # This is normal configuration, not feature preseeding or timestamp rollback.
        configure_once("precompile-stabilization")
    cache = observation.build_provenance(build)["cmake_options"]
    expected_gate = "OFF" if phase == "prepare" else "ON"
    for gate in contract["gates"]:
        if cache.get(gate) != expected_gate:
            raise ValueError("configured native artifact gate mismatch: " + gate)
    if windows and cache.get("KLOGG_ENABLE_IOS_NATIVE_STACK") != "OFF":
        raise ValueError("configured Windows iOS gate must remain disabled")
    if phase == "prepare":
        command = object_command(build, windows=windows)
    else:
        current_commands = compile_commands(build, windows=windows)
        if any(current_commands.get(output) != command for output, command in previous_commands.items()):
            raise ValueError("native finalization changed prepared compilation commands")
        command = ["cmake", "--build", str(build), "-t", "ci_build"]
    output = build / f"ci-build-phase-{current['run_id']}-{current['run_attempt']}-{phase}.json"
    object_cache = os.environ.get("KLOGG_WINDOWS_OBJECT_CACHE", "disabled") if windows else "measured"
    report = observation.run_phase(
        command, repo_root=repo, build_root=build, output=output, phase=phase, object_cache=object_cache,
        cache_tool="sccache" if windows else "ccache",
        provenance={"native_configure_strategy": CONFIGURE_STRATEGY, "native_platform": platform},
        configure={"passes": configure_passes, "wall_ms": sum(part["wall_ms"] for part in configure_passes),
                   "excluded_from_build_wall": True})
    if report is not None:
        print("KLOGG_BUILD_PHASE_METRICS_JSON=" + json.dumps(report, sort_keys=True), flush=True)
    state["status"] = "prepared" if phase == "prepare" else "finalized"
    observation.replace_report(build / STATE, state)
    if phase == "finalize":
        observation.publish_phased_build(build, current)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", required=True, choices=("prepare", "finalize"))
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args(argv)
    try:
        repo = Path(os.environ["KLOGG_WORKSPACE"])
        execute(args.phase, repo, repo / os.environ["KLOGG_BUILD_ROOT"],
                parse_options(os.environ["KLOGG_CMAKE_OPTS"], windows=os.environ.get("RUNNER_OS") == "Windows"),
                args.receipt)
        return 0
    except subprocess.CalledProcessError as error:
        return error.returncode or 1
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
