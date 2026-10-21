#!/usr/bin/env python3
"""Observe branch-independent compiler-cache inputs and emit trusted configuration.

This describes ordinary compiler objects, not native-core qualification. Native
identities come from the native cache helper; Windows identities come from its
current-run verified sccache state. Application source revisions are intentionally
not cache compatibility keys: each compiler-cache invocation hashes its inputs.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

# Isolated Python omits the script directory. Admit only this trusted sibling
# directory, never CWD, a downloaded archive directory, or an input repo root.
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import ci_ccache
import ci_compiler_object_transport as transport

ROOT = Path(__file__).resolve().parents[1]
MATERIAL_FILES = ("CMakeLists.txt", "scripts/materialize_boost.py", "packaging/adb/adb-helper.lock.json")
COMMON_POLICY = (
    "scripts/ci_compiler_object_descriptor.py",
    "scripts/ci_ccache.py",
    "scripts/ci_build_observation.py",
    "scripts/ci_build_metrics.py",
    "scripts/materialize_boost.py",
    ".github/actions/prepare-workspace-env/action.yml",
)
PLATFORM_POLICY = {
    "macOS": (
        ".github/actions/agent-build/action.yml", ".github/actions/agent-run-tests/action.yml",
        ".github/actions/agent-package-mac/action.yml", "scripts/ci_native_overlap.py",
        "scripts/ci_native_rendezvous.py", "scripts/ci_execution_diagnostics.py",
    ),
    "Linux": (
        ".github/actions/docker-build/action.yml", ".github/actions/docker-run-tests/action.yml",
        ".github/actions/docker-package/action.yml", ".github/actions/prepare-linux-environment/action.yml",
    ),
    "Windows": (
        ".github/actions/agent-build/action.yml", ".github/actions/agent-run-tests/action.yml",
        ".github/actions/agent-package-win/action.yml", "scripts/ci_windows_object_cache.py",
        "cmake/WindowsObjectCache.cmake", "ci/tools/sccache-windows.json",
        "scripts/ci_native_overlap.py", "scripts/ci_native_rendezvous.py",
    ),
}
NATIVE_COMMON_POLICY = (
    "scripts/ci_compiler_object_descriptor.py", "scripts/ci_native_object_cache.py",
    "scripts/ci_dependency_identity.py", "scripts/ci_ccache.py", "scripts/ci_dependency_toolchain.py",
)
NATIVE_POLICY = {
    "adb-helper": NATIVE_COMMON_POLICY + (
        "scripts/build_adb_helper.py", "scripts/verify_adb_helper_artifact.py", "scripts/smoke_adb_helper.py",
        "scripts/verify_adb_helper_toolchain.py", ".github/actions/build-adb-helper/action.yml",
        "packaging/adb/adb-helper.lock.json", "packaging/adb/superbuild/CMakeLists.txt",
    ),
    "ios-native": NATIVE_COMMON_POLICY + (
        "scripts/build_ios_native_stack.py", "scripts/verify_ios_native_stack.py",
        "3rdparty/libimobiledevice/libimobiledevice.lock.json", "packaging/ios-native/superbuild/CMakeLists.txt",
    ),
}
NATIVE_IDENTITY_FIELDS = {
    "core", "compiler_sha256", "compiler", "sdk", "sdk_version", "xcode", "cmake", "ninja", "ccache", "compiler_check",
}
IOS_IDENTITY_FIELDS = {
    "component", "target", "compiler_path", "cxx", "host_tools", "homebrew", "build_tools", "tool_environment",
}
CONFIG_KEYS = (
    "KLOGG_CONFIG_ARCH", "KLOGG_CONFIG_OS", "KLOGG_QT_VERSION", "KLOGG_QT_ARCH",
    "KLOGG_QT_MODULES", "KLOGG_QT_ARCHIVES", "KLOGG_CONFIG_PACKAGE_TAG",
    "KLOGG_CMAKE_OPTS", "KLOGG_CONFIG_CMAKE_OPTS", "KLOGG_SANITIZER",
    "CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "SDKROOT",
    "MACOSX_DEPLOYMENT_TARGET",
)


def require(condition, message):
    transport.require(condition, message)


def command(arguments, *, cwd=None):
    return subprocess.run(arguments, cwd=cwd, check=True, capture_output=True,
                          text=True, timeout=60).stdout.strip()


def eligible(env, event):
    """Skip foreign/unsupported contexts before observing any host toolchain."""
    if env.get("GITHUB_REPOSITORY") != transport.REPOSITORY:
        return False
    repository = event.get("repository")
    require(isinstance(repository, dict) and repository.get("full_name") == transport.REPOSITORY
            and type(repository.get("id")) is int and repository["id"] > 0
            and env.get("GITHUB_REPOSITORY_ID") == str(repository["id"]),
            "malformed canonical repository identity")
    name = env.get("GITHUB_EVENT_NAME")
    if name == "pull_request":
        pull, number = event.get("pull_request"), event.get("number")
        require(isinstance(pull, dict) and isinstance(pull.get("head"), dict)
                and isinstance(pull["head"].get("repo"), dict)
                and type(number) is int and number > 0 and pull.get("number") == number
                and env.get("GITHUB_REF") == f"refs/pull/{number}/merge", "malformed pull request identity")
        head = pull["head"]["repo"]
        return head.get("id") == repository["id"] and head.get("full_name") == transport.REPOSITORY
    if name == "push":
        return (env.get("GITHUB_REF") == "refs/heads/master" and event.get("ref") == "refs/heads/master"
                and event.get("after") == env.get("GITHUB_SHA"))
    return False


def normalize(value, env):
    """Normalize checkout location only; do not erase flags, versions or SDKs."""
    if isinstance(value, dict):
        return {key: normalize(item, env) for key, item in value.items()}
    if isinstance(value, list):
        return [normalize(item, env) for item in value]
    if not isinstance(value, str):
        return value
    roots = {env.get(key, "") for key in ("GITHUB_WORKSPACE", "KLOGG_WORKSPACE")}
    for root in sorted((root for root in roots if root), key=len, reverse=True):
        for spelling in sorted({root, root.replace("\\", "/"), root.replace("/", "\\")}, key=len, reverse=True):
            value = value.replace(spelling, "<workspace>")
    return value


def materials(root, probe):
    result = {}
    for name in ("3rdparty", "cmake"):
        tree = probe(["git", "rev-parse", "HEAD:" + name], cwd=root)
        require(isinstance(tree, str) and transport.SHA.fullmatch(tree), "invalid dependency Git tree")
        result[name + "_tree"] = tree
    # A dirty dependency recipe is not described by HEAD's dependency tree.
    probe(["git", "diff", "--exit-code", "HEAD", "--", "3rdparty", "cmake"], cwd=root)
    for name in MATERIAL_FILES:
        path = root / name
        require(path.is_file() and not path.is_symlink(), "missing dependency recipe material")
        result[name] = transport.digest(path)
    return result


def macos_toolchain(env, probe):
    require(not env.get("CC") and not env.get("CXX"), "unreviewed macOS compiler environment override")
    options = env.get("KLOGG_CMAKE_OPTS", "") + " " + env.get("KLOGG_CONFIG_CMAKE_OPTS", "")
    require(not re.search(r"-DCMAKE_(?:C|CXX)_COMPILER(?::[^= ]+)?=", options),
            "unreviewed macOS CMake compiler override")
    compilers = {}
    for name in ("clang", "clang++"):
        executable = probe(["xcrun", "--find", name])
        require(Path(executable).is_file(), "observed macOS compiler is missing")
        compilers[name] = {"sha256": transport.digest(executable),
                           "version": probe([executable, "--version"])}
    qmake = shutil.which("qmake") or shutil.which("qmake6")
    require(qmake is not None, "installed Qt qmake is missing")
    qt_version = probe([qmake, "-query", "QT_VERSION"])
    require(qt_version == env.get("KLOGG_QT_VERSION"), "observed Qt version differs from selected Qt")
    return {"compilers": compilers, "sdk": probe(["xcrun", "--show-sdk-path"]),
            "sdk_version": probe(["xcrun", "--show-sdk-version"]),
            "xcode": probe(["xcodebuild", "-version"]), "qmake_sha256": transport.digest(qmake),
            "qt_version": qt_version, "cmake": probe(["cmake", "--version"]),
            "ninja": probe(["ninja", "--version"]), "ccache": probe(["ccache", "--version"])}


def native_component(identity):
    require(isinstance(identity, dict) and NATIVE_IDENTITY_FIELDS <= set(identity)
            and all(isinstance(identity[key], str) and identity[key] for key in NATIVE_IDENTITY_FIELDS)
            and all(re.fullmatch(r"[0-9a-f]{64}", identity[key]) for key in ("core", "compiler_sha256"))
            and identity["compiler_check"] == "content", "invalid native identity inputs")
    if set(identity) == NATIVE_IDENTITY_FIELDS:
        # The existing ADB schema predates an explicit component field. Keep its
        # fingerprint intact, but recognize that exact schema, never the label.
        return "adb-helper"
    require(set(identity) == NATIVE_IDENTITY_FIELDS | IOS_IDENTITY_FIELDS
            and identity.get("component") == "ios-native" and identity.get("target") in ("x86_64", "arm64")
            and isinstance(identity.get("compiler_path"), str) and identity["compiler_path"]
            and all(isinstance(identity.get(key), dict) and identity[key] for key in
                    ("cxx", "host_tools", "homebrew", "build_tools", "tool_environment")),
            "invalid extended native identity schema")
    return "ios-native"


def make_descriptor(component, label, producer_job, required_steps, export_step, *, root, env,
                    native_identity=None, windows_state=None, image=None,
                    native_budget=128_000_000, probe=command):
    root = Path(root).resolve(strict=True)
    require(isinstance(label, str) and re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,63}", label),
            "invalid cache descriptor label")
    if component == "native":
        require(native_identity is not None and windows_state is None and image is None,
                "native descriptor requires only its existing native identity")
        compatibility = transport.document(native_identity)
        native = native_component(compatibility)
        kind, budget, policy = "ccache", native_budget, NATIVE_POLICY[native]
    else:
        require(component == "application" and native_identity is None, "invalid descriptor component")
        platform = env.get("RUNNER_OS")
        require(platform in PLATFORM_POLICY and env.get("KLOGG_LABEL") == label,
                "application descriptor differs from active platform or label")
        require(env.get("KLOGG_CONFIG_ARCH") and env.get("KLOGG_CMAKE_OPTS"),
                "application compiler configuration is missing")
        policy = COMMON_POLICY + PLATFORM_POLICY[platform]
        if platform == "Windows":
            require(windows_state is not None and image is None, "Windows descriptor requires verified cache state")
            state = transport.document(windows_state)
            run = {key: env.get(key, "") for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_SHA")}
            require(state.get("ready") is True and state.get("run") == run
                    and isinstance(state.get("identity"), dict) and state["identity"],
                    "Windows descriptor requires current-run verified cache state")
            toolchain, kind, budget = normalize(state["identity"], env), "sccache", state.get("budget")
        else:
            require(windows_state is None, "unexpected Windows state for ccache")
            budget = ci_ccache.size_bytes(ci_ccache.environment(label)["CCACHE_MAXSIZE"])
            kind = "ccache"
            if platform == "macOS":
                require(image is None, "unexpected container image for macOS")
                toolchain = macos_toolchain(env, probe)
            else:
                require(isinstance(image, str) and re.fullmatch(r"(?:[A-Za-z0-9./:_-]+@)?sha256:[0-9a-f]{64}", image),
                        "Linux application cache requires a digest-qualified image")
                toolchain = {"image": image}
        compatibility = {"component": "application", "platform": platform,
                         "configuration": normalize({key: env.get(key, "") for key in CONFIG_KEYS}, env),
                         "dependencies": materials(root, probe), "toolchain": toolchain,
                         "cache_policy": ({"tool": "sccache"} if kind == "sccache" else ci_ccache.environment(label))}
    require(type(budget) is int and 0 < budget <= transport.MAX_BYTES, "invalid existing compiler cache budget")
    return {"schema_version": 1, "kind": kind, "label": label, "compatibility": compatibility,
            "policy_files": list(policy), "producer_job": producer_job,
            "required_steps": list(required_steps), "export_step": export_step, "budget_bytes": budget}


def trusted_config(root, descriptor, env):
    """Configuration is caller-owned and deliberately outside replayed data."""
    root = Path(root).resolve(strict=True)
    directory = root / ".ci-cache-config"
    require(not directory.is_symlink() and (not directory.exists() or directory.is_dir()),
            "unsafe compiler cache configuration directory")
    directory.mkdir(exist_ok=True)
    path = directory / (descriptor["label"] + ".conf")
    require(not path.is_symlink() and (not path.exists() or path.is_file()), "unsafe compiler cache configuration file")
    settings = ci_ccache.environment(descriptor["label"])
    path.write_text("\n".join((
        # ccache interprets a unitless config number as GiB, not bytes.
        "max_size = " + settings["CCACHE_MAXSIZE"],
        "compiler_check = " + settings["CCACHE_COMPILERCHECK"],
        "compression = true", "compression_level = 6", "sloppiness =",
        "prefix_command =", "prefix_command_cpp =", "remote_storage =", "",
    )), encoding="utf-8")
    result = {"config-path": str(path), "container-config-path": "/usr/local/.ci-cache-config/" + path.name}
    environment_path = env.get("GITHUB_ENV")
    if environment_path:
        values = {**settings, "CCACHE_CONFIGPATH": str(path), "CCACHE_SLOPPINESS": "",
                  "CCACHE_PREFIX": "", "CCACHE_PREFIX_CPP": "", "CCACHE_REMOTE_STORAGE": ""}
        with Path(environment_path).open("a", encoding="utf-8") as output:
            for key, value in values.items():
                require("\n" not in value and "\r" not in value, "unsafe compiler configuration environment")
                output.write(f"{key}={value}\n")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", choices=("application", "native"), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--producer-job", required=True)
    parser.add_argument("--required-step", action="append", required=True)
    parser.add_argument("--export-step", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--native-identity", type=Path)
    parser.add_argument("--native-budget", type=int, default=128_000_000)
    parser.add_argument("--windows-state", type=Path)
    parser.add_argument("--image")
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        event = transport.document(os.environ["GITHUB_EVENT_PATH"])
        if not eligible(os.environ, event):
            # Forks and dispatches still compile with the existing cache policy.
            # Emit trusted configuration without probing tools or admitting any
            # shared artifact; never fall back to replayed ccache.conf settings.
            extra = {}
            if (args.component == "application" and os.environ.get("RUNNER_OS") in ("Linux", "macOS")
                    and args.label in ci_ccache.BUDGETS):
                require(os.environ.get("KLOGG_LABEL") == args.label, "application cache label mismatch")
                cache = {"label": args.label, "budget_bytes": ci_ccache.size_bytes(
                    ci_ccache.environment(args.label)["CCACHE_MAXSIZE"])}
                extra = trusted_config(args.repo_root, cache, os.environ)
            transport.emit({"eligible": False, **extra})
            print("Compiler artifact descriptor skipped outside canonical ordinary PR/master scope")
            return 0
        transport.identity(os.environ, args.repo_root)
        value = make_descriptor(args.component, args.label, args.producer_job, args.required_step,
                                args.export_step, root=args.repo_root, env=os.environ,
                                native_identity=args.native_identity, windows_state=args.windows_state,
                                image=args.image, native_budget=args.native_budget)
        # Validate all repository policy paths and action identities before any
        # descriptor or configuration is made available to the caller.
        transport.context(value, args.repo_root)
        require(not args.output.exists() and not args.output.is_symlink(), "descriptor output already exists")
        extra = trusted_config(args.repo_root, value, os.environ) if args.component == "application" and value["kind"] == "ccache" else {}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(transport.canonical(value))
        transport.emit({"eligible": True, "descriptor": args.output, "path": args.output,
                        "kind": value["kind"], "budget-bytes": value["budget_bytes"], **extra})
        print(json.dumps({"eligible": True, "component": args.component, "label": args.label,
                          "kind": value["kind"], "budget_bytes": value["budget_bytes"]}, sort_keys=True))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"compiler object descriptor rejected: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
