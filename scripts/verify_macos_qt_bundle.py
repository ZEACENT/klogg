#!/usr/bin/env python3
"""Verify that a deployed macOS app has one self-contained Qt runtime."""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from collections.abc import Iterable


SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
QPA_FAILURE_MARKERS = (
    "You might be loading two sets of Qt binaries",
    "Could not load the Qt platform plugin",
    "This application failed to start because no Qt platform plugin could be initialized",
)


class VerificationError(RuntimeError):
    pass


def dependency_issues(
    app: pathlib.Path, binary: pathlib.Path, dependencies: Iterable[str]
) -> list[str]:
    issues: list[str] = []
    app_resolved = app.resolve()
    for dependency in dependencies:
        if dependency.startswith("/") and not dependency.startswith(SYSTEM_PREFIXES):
            dependency_path = pathlib.Path(dependency)
            try:
                dependency_path.resolve().relative_to(app_resolved)
            except (OSError, ValueError):
                label = "external Qt dependency" if "Qt" in dependency else "external dependency"
                issues.append(f"{binary}: {label}: {dependency}")

        framework = re.search(r"(Qt[^/]+\.framework)(?:/|$)", dependency)
        if framework is not None:
            bundled = app / "Contents" / "Frameworks" / framework.group(1)
            if not bundled.is_dir():
                issues.append(f"{binary}: missing bundled Qt framework: {framework.group(1)}")
    return issues


def runtime_issues(app: pathlib.Path, output: str) -> list[str]:
    issues = [marker for marker in QPA_FAILURE_MARKERS if marker in output]
    if re.search(r"Class .+ is implemented in both", output):
        issues.append("Objective-C class is implemented in two runtime images")

    app_text = str(app.resolve())
    for path in re.findall(r"/(?:[^\s:'\"]+/)*Qt[^\s:'\"]+", output):
        if path.startswith(SYSTEM_PREFIXES) or path.startswith(app_text):
            continue
        issues.append(f"runtime loaded Qt outside the app bundle: {path}")
    return issues


def command_output(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise VerificationError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def macho_dependencies(path: pathlib.Path) -> list[str] | None:
    result = subprocess.run(
        ["otool", "-L", str(path)], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        return None
    dependencies: list[str] = []
    for line in result.stdout.splitlines()[1:]:
        entry = line.strip().split(" (compatibility version", 1)[0]
        if entry:
            dependencies.append(entry)
    return dependencies


def validate_qt_conf(app: pathlib.Path) -> list[str]:
    path = app / "Contents" / "Resources" / "qt.conf"
    if not path.is_file():
        return [f"missing deployed qt.conf: {path}"]
    text = path.read_text(encoding="utf-8")
    if "[Paths]" not in text or not re.search(r"^Plugins\s*=\s*PlugIns\s*$", text, re.MULTILINE):
        return [f"qt.conf must route plugins to the app bundle: {path}"]
    return []


def validate_bundle(app: pathlib.Path, expected_architecture: str) -> list[str]:
    executable = app / "Contents" / "MacOS" / "klogg"
    cocoa = app / "Contents" / "PlugIns" / "platforms" / "libqcocoa.dylib"
    issues: list[str] = []
    if not executable.is_file():
        issues.append(f"missing application executable: {executable}")
    if not cocoa.is_file():
        issues.append(f"missing deployed Cocoa platform plugin: {cocoa}")
    issues.extend(validate_qt_conf(app))
    if issues:
        return issues

    for root in ("MacOS", "Frameworks", "PlugIns"):
        directory = app / "Contents" / root
        if not directory.is_dir():
            continue
        for candidate in sorted(directory.rglob("*")):
            if not candidate.is_file() or candidate.is_symlink():
                continue
            dependencies = macho_dependencies(candidate)
            if dependencies is not None:
                issues.extend(dependency_issues(app, candidate, dependencies))

    for path in (executable, cocoa):
        architectures = command_output(["lipo", "-archs", str(path)]).split()
        if expected_architecture not in architectures:
            issues.append(
                f"{path}: expected architecture {expected_architecture}, got {architectures}"
            )
    return issues


def smoke_bundle(
    app: pathlib.Path,
    platform: str,
    *,
    required: bool,
    log_output: pathlib.Path | None,
) -> list[str]:
    plugin = app / "Contents" / "PlugIns" / "platforms" / f"libq{platform}.dylib"
    if not plugin.is_file():
        if required:
            return [f"missing platform plugin required for smoke: {plugin}"]
        return []

    executable = app / "Contents" / "MacOS" / "klogg"
    environment = os.environ.copy()
    for name in (
        "QT_PLUGIN_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QML2_IMPORT_PATH",
        "DYLD_LIBRARY_PATH",
        "DYLD_FRAMEWORK_PATH",
        "DYLD_FALLBACK_LIBRARY_PATH",
        "DYLD_FALLBACK_FRAMEWORK_PATH",
    ):
        environment.pop(name, None)
    environment["DYLD_PRINT_LIBRARIES"] = "1"

    with tempfile.TemporaryDirectory(prefix="klogg-macos-smoke-") as home:
        environment["HOME"] = home
        try:
            result = subprocess.run(
                [str(executable), "-platform", platform, "-v"],
                capture_output=True,
                text=True,
                env=environment,
                timeout=30,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return [f"{platform} startup smoke timed out"]

    output = result.stdout + result.stderr
    if log_output is not None:
        with log_output.open("a", encoding="utf-8") as stream:
            stream.write(f"===== platform={platform} =====\n{output}\n")
    issues = runtime_issues(app, output)
    if result.returncode != 0:
        issues.append(f"{platform} startup smoke exited with {result.returncode}")
    return issues


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True, type=pathlib.Path)
    parser.add_argument("--expected-architecture", required=True)
    parser.add_argument("--smoke", action="append", default=[])
    parser.add_argument("--smoke-if-present", action="append", default=[])
    parser.add_argument("--runtime-log-output", type=pathlib.Path)
    args = parser.parse_args()

    app = args.app.resolve()
    issues = validate_bundle(app, args.expected_architecture)
    for platform in args.smoke:
        issues.extend(
            smoke_bundle(
                app,
                platform,
                required=True,
                log_output=args.runtime_log_output,
            )
        )
    for platform in args.smoke_if_present:
        issues.extend(
            smoke_bundle(
                app,
                platform,
                required=False,
                log_output=args.runtime_log_output,
            )
        )

    if issues:
        raise VerificationError("\n".join(issues))
    print(f"macOS Qt bundle verified: {app}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VerificationError as error:
        print(f"macOS Qt bundle verification failed:\n{error}", file=sys.stderr)
        raise SystemExit(1) from None
