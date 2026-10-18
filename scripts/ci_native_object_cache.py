#!/usr/bin/env python3
"""Ordinary macOS native compiler objects, never qualified native artifact reuse."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

from ci_ccache import size_bytes
from ci_dependency_identity import core_identity
from ci_dependency_toolchain import observe_ios_host_tools

ROOT = Path(__file__).resolve().parents[1]
BUDGET = 128_000_000  # Provisional per-target allowance; measure before increasing.
COMPONENTS = {
    "adb-helper": {"namespace": "adb", "targets": ("macos-x86_64", "macos-arm64"),
                   "lock": "packaging/adb/adb-helper.lock.json"},
    "ios-native": {"namespace": "ios", "targets": ("x86_64", "arm64"),
                   "lock": "3rdparty/libimobiledevice/libimobiledevice.lock.json"},
}


def component_descriptor(component, target):
    descriptor = COMPONENTS.get(component)
    if descriptor is None or target not in descriptor["targets"]:
        raise ValueError("unsupported ordinary native cache component or target")
    return descriptor


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def digest(path):
    with Path(path).open("rb") as source:
        result = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
        return result.hexdigest()


def cache_keys(env, event, target, dependency_mode, identity, day, *, component="adb-helper"):
    descriptor = component_descriptor(component, target)
    if dependency_mode != "off":
        raise ValueError("only ordinary macOS native builds may use this cache")
    if not isinstance(event, dict) or not isinstance(event.get("repository"), dict):
        raise ValueError("malformed GitHub event repository")
    repository = event["repository"]
    if (env.get("GITHUB_REPOSITORY") != "ZEACENT/klogg"
            or repository.get("full_name") != "ZEACENT/klogg"
            or type(repository.get("id")) is not int
            or repository["id"] <= 0
            or str(repository["id"]) != env.get("GITHUB_REPOSITORY_ID")):
        raise ValueError("cache requires the canonical repository identity")
    if not re.fullmatch(r"[0-9a-f]{64}", identity) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", day):
        raise ValueError("invalid native cache identity or generation")
    base = f"native-{descriptor['namespace']}-ccache-v1-{target}-{identity}-"
    master = base + "master-"
    name, ref = env.get("GITHUB_EVENT_NAME"), env.get("GITHUB_REF")
    writable = False
    primary = master
    if name == "pull_request":
        number = event.get("number")
        pull = event.get("pull_request")
        if (not isinstance(pull, dict) or not isinstance(pull.get("head"), dict)
                or not isinstance(pull["head"].get("repo"), dict)):
            raise ValueError("malformed GitHub PR source")
        head = pull["head"]["repo"]
        if (type(number) is not int or number <= 0 or pull.get("number") != number
                or ref != f"refs/pull/{number}/merge"
                or head.get("full_name") != repository["full_name"]
                or type(head.get("id")) is not int or head["id"] != repository["id"]):
            raise ValueError("only this canonical repository's own PR namespace is writable")
        primary = base + f"pr-{number}-"
        writable = True
    elif name == "push" and ref == "refs/heads/master":
        writable = True
    elif name == "workflow_dispatch":
        inputs = event.get("inputs", {})
        if (not isinstance(inputs, dict)
                or inputs.get("dependency-mode", "off") != "off"
                or inputs.get("environment-mode", "off") != "off"):
            raise ValueError("producer and host-evidence modes cannot restore ordinary compiler objects")
    else:
        raise ValueError("unsupported native cache event or ref")
    return {"primary-key": primary + day, "primary-prefix": primary,
            "master-key": master + day, "master-prefix": master,
            "save-key": primary + day if writable else "",
            "own-pr": "true" if name == "pull_request" else "false"}


def cache_environment(cache, config, base):
    return {"CCACHE_DIR": str(cache), "CCACHE_CONFIGPATH": str(config),
            "CCACHE_BASEDIR": str(base), "CCACHE_MAXSIZE": "128M",
            "CCACHE_COMPILERCHECK": "content", "CCACHE_COMPRESS": "true",
            "CCACHE_COMPRESSLEVEL": "6", "CCACHE_SLOPPINESS": "",
            "CCACHE_PREFIX": "", "CCACHE_PREFIX_CPP": "", "CCACHE_REMOTE_STORAGE": ""}


def launcher(executable, env):
    values = ["env", *(f"{key}={value}" for key, value in sorted(env.items()) if key.startswith("CCACHE_")),
              str(executable)]
    if any(any(character in value for character in (";", "\n", "\r")) for value in values):
        raise ValueError("unsafe CMake launcher value")
    return ";".join(values)


def command(args, env=None):
    return subprocess.run(args, env=env, check=True, capture_output=True, text=True, timeout=120).stdout.strip()


def statistics(executable, env):
    return {name: int(value) for name, value in
            (line.split() for line in command([executable, "--print-stats"], env).splitlines())}


def occupancy(root):
    if Path(root).is_symlink() or (Path(root).exists() and not Path(root).is_dir()):
        raise ValueError("compiler cache root is not an ordinary directory")
    total = 0
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            if path.is_symlink():
                raise ValueError("compiler cache contains a symlink")
        for name in files:
            path = Path(directory) / name
            if not path.is_file():
                raise ValueError("compiler cache contains a special file")
            total += path.stat().st_size
    return total


def emit(values):
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with Path(path).open("a", encoding="utf-8") as output:
            for key, value in values.items():
                if any(character in str(value) for character in ("\n", "\r")):
                    raise ValueError("newline in action output")
                output.write(f"{key}={value}\n")


def run_identity():
    return {key: os.environ.get(key, "") for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_SHA")}


def tool_identity(name):
    selected = shutil.which(name)
    if not selected:
        raise ValueError("native cache tool unavailable: " + name)
    resolved = Path(selected).resolve(strict=True)
    return {"path": selected, "resolved_path": str(resolved), "sha256": digest(resolved),
            "version": command([selected, "--version"]).splitlines()[0]}


def tool_content_identity(name):
    # get-cmake installs into a per-run temporary directory. Bind its executable
    # bytes/version, not that random extraction path (nor the job's source SHA).
    observation = tool_identity(name)
    return {key: observation[key] for key in ("sha256", "version")}


def compatibility_identity(component, target, executable):
    descriptor = component_descriptor(component, target)
    compiler = command(["xcrun", "--find", "clang"])
    lock = json.loads((ROOT / descriptor["lock"]).read_text())
    identity = {"core": core_identity(component, target, lock, ROOT),
                "compiler_sha256": digest(compiler), "compiler": command([compiler, "--version"]),
                "sdk": command(["xcrun", "--show-sdk-path"]),
                "sdk_version": command(["xcrun", "--show-sdk-version"]),
                "xcode": command(["xcodebuild", "-version"]),
                "cmake": command(["cmake", "--version"]), "ninja": command(["ninja", "--version"]),
                "ccache": command([executable, "--version"]).splitlines()[0],
                "compiler_check": "content"}
    if component == "ios-native":
        # This fingerprint scopes ordinary objects, not qualified host-tool closure.
        observation = observe_ios_host_tools(target_id="ios-" + target, host_arch=target,
                                             developer_dir=os.environ.get("DEVELOPER_DIR", ""))
        identity.update(component=component, target=target, compiler_path=compiler,
                        cxx=tool_identity(command(["xcrun", "--find", "clang++"])),
                        host_tools={**observation["tools"], "make": tool_identity("make"),
                                    "glibtool": tool_identity("glibtool")},
                        homebrew=observation["homebrew"],
                        build_tools={name: tool_content_identity(executable if name == "ccache" else name)
                                     for name in ("cmake", "ninja", "ccache")},
                        tool_environment={name: os.environ.get(name, "") for name in
                                          ("DEVELOPER_DIR", "SDKROOT", "CPATH", "C_INCLUDE_PATH",
                                           "CPLUS_INCLUDE_PATH", "LIBRARY_PATH")})
    return identity


def prepare(target, component="adb-helper"):
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    # Reject candidate modes and foreign PRs before acquiring cache tooling.
    cache_keys(os.environ, event, target, "off", "0" * 64, "2000-01-01", component=component)
    descriptor = component_descriptor(component, target)
    architecture = target[len("macos-"):] if target.startswith("macos-") else target
    if platform.system() != "Darwin" or architecture != platform.machine():
        raise ValueError("native matching macOS architecture required")
    if os.environ.get("CC") or os.environ.get("CXX"):
        raise ValueError("unreviewed compiler environment override")
    executable = shutil.which("ccache")
    if not executable:
        raise ValueError("ccache executable unavailable")
    identity = compatibility_identity(component, target, executable)
    compatibility = fingerprint(identity)
    day = datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    keys = cache_keys(os.environ, event, target, "off", compatibility, day, component=component)
    parent = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    prefix = "native-" + descriptor["namespace"]
    cache = parent / (prefix + "-objects-" + target)
    session = Path(tempfile.mkdtemp(prefix=prefix + "-cache-session-", dir=parent))
    config = session / "ccache.conf"
    config.write_text("compiler_check = content\nmax_size = 128M\ncompression = true\n")
    env = cache_environment(cache, config, parent)
    state = {"component": component, "target": target, "identity": identity, "keys": keys,
             "run": run_identity(), "executable": executable, "environment": env, "cache": str(cache)}
    state_path = session / "state.json"
    state_path.write_text(json.dumps(state, indent=2) + "\n")
    identity_path = session / "compatibility.json"
    identity_path.write_text(json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n")
    emit({"enabled": "true", "state": str(state_path), "path": str(cache),
          "launcher": launcher(executable, env), "compiler-launcher": executable,
          "identity-file": str(identity_path), "compatibility-fingerprint": compatibility, **keys})
    print(json.dumps({"native_object_cache": "prepared", "component": component, "target": target,
                      "budget_bytes": BUDGET, "budget_status": "provisional", "keys": keys}, sort_keys=True))


def load_state(state_path):
    state = json.loads(state_path.read_text())
    if state["run"] != run_identity():
        raise ValueError("stale native cache state")
    return state


def execute(state_path, argv):
    if not argv:
        raise ValueError("native cache exec requires a command after --")
    state = load_state(state_path)
    # Do not embed env assignments in Autotools CC: libtool recognizes ccache,
    # but not an env prefix. Keep the executable-only launcher and environment separate.
    subprocess.run(argv, env={**os.environ, **state["environment"]}, check=True)


def update(state_path, phase):
    state = load_state(state_path)
    env = {**os.environ, **state["environment"]}
    executable = state["executable"]
    cache = Path(state["cache"])
    if phase == "start":
        state["occupancy_before_bytes"] = occupancy(cache)
        state["restored_keys"] = {name: os.environ.get(name, "") for name in
                                  ("KLOGG_NATIVE_PRIMARY_MATCH", "KLOGG_NATIVE_MASTER_MATCH")}
        state["started_ns"] = time.monotonic_ns()
        command([executable, "--zero-stats"], env)
        state["before"] = statistics(executable, env)
        state_path.write_text(json.dumps(state, indent=2) + "\n")
        return
    before, after = state["before"], statistics(executable, env)
    counters = ("cache_miss", "direct_cache_hit", "preprocessed_cache_hit")
    delta = {key: after.get(key, 0) - before.get(key, 0) for key in counters}
    if any(value < 0 for value in delta.values()) or sum(delta.values()) == 0:
        raise ValueError("native build has no valid current-build compiler cache counters")
    if size_bytes(command([executable, "--get-config", "max_size"], env)) != BUDGET:
        raise ValueError("native compiler cache budget differs from configured allowance")
    before_cleanup = occupancy(cache)
    command([executable, "--cleanup"], env)
    after_cleanup = occupancy(cache)
    if after_cleanup > BUDGET + max(1_000_000, BUDGET // 100):
        raise ValueError("native compiler cache exceeds budget after cleanup")
    report = {"schema_version": 1, "kind": "ordinary-native-compiler-cache", "run": state["run"],
              "component": state.get("component", "adb-helper"),
              "target": state["target"], "identity": state["identity"], "keys": state["keys"],
              "budget_bytes": BUDGET, "budget_status": "provisional", "cache_deltas": delta,
              "restored_keys": state["restored_keys"],
              "build_and_verification_wall_ms": (time.monotonic_ns() - state["started_ns"]) / 1_000_000,
              "occupancy_before_bytes": state["occupancy_before_bytes"],
              "occupancy_before_cleanup_bytes": before_cleanup, "occupancy_after_cleanup_bytes": after_cleanup,
              "statistics_before": before, "statistics_after": after,
              "effective_config": command([executable, "--show-config"], env)}
    report_path = state_path.parent / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, sort_keys=True))
    emit({"validated": "true", "report": str(report_path)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("eligible", "prepare", "start", "finish", "exec"))
    parser.add_argument("--target")
    parser.add_argument("--component", choices=tuple(COMPONENTS), default="adb-helper")
    parser.add_argument("--state", type=Path)
    arguments = sys.argv[1:]
    separator = arguments.index("--") if "--" in arguments else len(arguments)
    args = parser.parse_args(arguments[:separator])
    build_command = arguments[separator + 1:]
    if build_command and args.command != "exec":
        parser.error("only exec accepts a command after --")
    try:
        if args.command == "eligible":
            event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
            try:
                cache_keys(os.environ, event, args.target, "off", "0" * 64, "2000-01-01",
                           component=args.component)
            except ValueError as error:
                emit({"eligible": "false"})
                print(f"Native object cache disabled: {error}")
            else:
                emit({"eligible": "true"})
        elif args.command == "prepare":
            prepare(args.target, args.component)
        else:
            if args.state is None:
                parser.error("--state is required")
            if args.command == "exec":
                execute(args.state, build_command)
            else:
                update(args.state, args.command)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"native object cache rejected: {error}\n")


if __name__ == "__main__":
    main()
